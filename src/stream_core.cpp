#include "stream_core.h"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstdarg>
#include <cstdio>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#define LOG_PREFIX "[Moonlight-Godot-StreamCore] "

using namespace godot;

static MoonlightStreamCore *singleton_instance = nullptr;

// Initialize static mutex pointer
Mutex *MoonlightStreamCore::lib_global_mutex = nullptr;

// ============================================================================
// AudioStreamPlaybackMoonlight Implementation
// ============================================================================

AudioStreamPlaybackMoonlight::AudioStreamPlaybackMoonlight() : active(false) {}

AudioStreamPlaybackMoonlight::~AudioStreamPlaybackMoonlight() {}

void AudioStreamPlaybackMoonlight::_bind_methods() {}

void AudioStreamPlaybackMoonlight::_start(double p_from_pos) {
	active = true;
	_seek(p_from_pos);
}

void AudioStreamPlaybackMoonlight::_stop() {
	active = false;
}

bool AudioStreamPlaybackMoonlight::_is_playing() const {
	return active;
}

int32_t AudioStreamPlaybackMoonlight::_get_loop_count() const {
	return 0;
}

double AudioStreamPlaybackMoonlight::_get_playback_position() const {
	return 0.0;
}

void AudioStreamPlaybackMoonlight::_seek(double p_time) {}

int32_t AudioStreamPlaybackMoonlight::_mix_resampled(AudioFrame *p_buffer, int32_t p_frames) {
	if (!active || base.is_null()) {
		return 0;
	}
	return base->read_samples(p_buffer, p_frames);
}

float AudioStreamPlaybackMoonlight::_get_stream_sampling_rate() const {
	if (base.is_valid()) {
		return (float)base->mix_rate;
	}
	return 48000.0f;
}

// ============================================================================
// AudioStreamMoonlight Implementation
// ============================================================================

AudioStreamMoonlight::AudioStreamMoonlight() : mix_rate(48000), channels(2) {
	buffer_mutex.instantiate();
	// Latency optimization: Reduce buffer size.
	// 48000 Hz * 2 channels * 0.1s = 9600 samples.
	// Too small might cause crackling, too large causes delay.
	rb_capacity = 48000 * 2 * 0.1;
	ring_buffer.resize(rb_capacity);
	ring_buffer.fill(0);
}

Ref<AudioStreamPlayback> AudioStreamMoonlight::_instantiate_playback() const {
	Ref<AudioStreamPlaybackMoonlight> playback;
	playback.instantiate();
	playback->base = Ref<AudioStreamMoonlight>(this);
	return playback;
}

String AudioStreamMoonlight::_get_stream_name() const {
	return "Moonlight Audio Stream";
}

void AudioStreamMoonlight::push_audio(const float *samples, int count) {
	buffer_mutex->lock();

	int required_space = count;
	int free_space = rb_capacity - rb_used;

	if (required_space > free_space) {
		// Overflow: Advance read pointer (drop old audio) to maintain low latency
		int overflow = required_space - free_space;
		rb_read_pos = (rb_read_pos + overflow) % rb_capacity;
		rb_used -= overflow;
	}

	int first_chunk = MIN(count, rb_capacity - rb_write_pos);
	int second_chunk = count - first_chunk;

	float *ptr = ring_buffer.ptrw();
	memcpy(ptr + rb_write_pos, samples, first_chunk * sizeof(float));
	if (second_chunk > 0) {
		memcpy(ptr, samples + first_chunk, second_chunk * sizeof(float));
	}

	rb_write_pos = (rb_write_pos + count) % rb_capacity;
	rb_used += count;

	buffer_mutex->unlock();
}

int AudioStreamMoonlight::read_samples(AudioFrame *dst_buffer, int frame_count) {
	buffer_mutex->lock();

	int available_frames = rb_used / 2;
	int frames_to_read = MIN(frame_count, available_frames);

	const float *ptr = ring_buffer.ptr();
	int current_pos = rb_read_pos;

	for (int i = 0; i < frames_to_read; i++) {
		float l = ptr[current_pos];
		current_pos = (current_pos + 1) % rb_capacity;
		float r = ptr[current_pos];
		current_pos = (current_pos + 1) % rb_capacity;

		dst_buffer[i].left = l;
		dst_buffer[i].right = r;
	}

	rb_read_pos = current_pos;
	rb_used -= (frames_to_read * 2);

	buffer_mutex->unlock();

	// Fill silence if underrun
	if (frames_to_read < frame_count) {
		for (int i = frames_to_read; i < frame_count; i++) {
			dst_buffer[i].left = 0.0f;
			dst_buffer[i].right = 0.0f;
		}
	}

	return frame_count;
}

void AudioStreamMoonlight::clear_buffer() {
	buffer_mutex->lock();
	rb_write_pos = 0;
	rb_read_pos = 0;
	rb_used = 0;
	buffer_mutex->unlock();
}

void AudioStreamMoonlight::_bind_methods() {}

// ============================================================================
// MoonlightStreamCore Implementation
// ============================================================================

MoonlightStreamCore::MoonlightStreamCore() {
	singleton_instance = this;
	is_streaming = false;
	new_frame_available = false;

	video_mutex.instantiate();

	if (lib_global_mutex == nullptr) {
		lib_global_mutex = memnew(Mutex);
	}

	LiInitializeStreamConfiguration(&stream_config);
	LiInitializeServerInformation(&server_info);
	LiInitializeConnectionCallbacks(&cl_callbacks);
	LiInitializeVideoCallbacks(&dr_callbacks);
	LiInitializeAudioCallbacks(&ar_callbacks);

	cl_callbacks.stageStarting = _cl_stage_starting;
	cl_callbacks.connectionStarted = _cl_connection_started;
	cl_callbacks.connectionTerminated = _cl_connection_terminated;
	cl_callbacks.logMessage = _cl_log_message;

	dr_callbacks.setup = _dr_setup;
	dr_callbacks.cleanup = _dr_cleanup;
	dr_callbacks.submitDecodeUnit = nullptr;
	dr_callbacks.capabilities = CAPABILITY_PULL_RENDERER; // Pull mode is better for controlled loop

	ar_callbacks.init = _ar_init;
	ar_callbacks.cleanup = _ar_cleanup;
	ar_callbacks.decodeAndPlaySample = _ar_decode_and_play_sample;
}

MoonlightStreamCore::~MoonlightStreamCore() {
	stop_play_stream();
	if (singleton_instance == this) {
		singleton_instance = nullptr;
	}
}

void MoonlightStreamCore::start_play_stream(Dictionary options) {
	if (is_streaming) {
		UtilityFunctions::printerr(LOG_PREFIX "Stream already running");
		return;
	}

	// 1. Parse Options
	stream_config.width = options.get("width", 1280);
	stream_config.height = options.get("height", 720);
	stream_config.fps = options.get("fps", 60);
	stream_config.bitrate = options.get("bitrate", 10000);
	stream_config.packetSize = options.get("packet_size", 1024);
	stream_config.streamingRemotely = STREAM_CFG_AUTO;
	stream_config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;

	// Handle audio configuration
	if (options.has("surround_audio_info")) {
		// Just passing the raw integer if provided, logic handles conversion if needed elsewhere
	}

	stream_config.supportedVideoFormats = VIDEO_FORMAT_H264 | VIDEO_FORMAT_H265;
	// AV1 Check could be added here based on platform support
	if (options.has("enable_av1") && (bool)options["enable_av1"]) {
		stream_config.supportedVideoFormats |= VIDEO_FORMAT_AV1_MAIN8;
	}

	// Crypto Keys
	String rikey = options.get("rikey", "");
	if (!rikey.is_empty()) {
		PackedByteArray key_bytes = rikey.hex_decode();
		if (key_bytes.size() >= 16) {
			memcpy(stream_config.remoteInputAesKey, key_bytes.ptr(), 16);
		}
		memset(stream_config.remoteInputAesIv, 0, 16);
	}

	// Server Info
	ip_storage = String(options.get("ip", "")).utf8().get_data();
	session_url_storage = String(options.get("session_url", "")).utf8().get_data();
	app_version_storage = String(options.get("app_version", "0.0.0.0")).utf8().get_data();
	gfe_version_storage = String(options.get("gfe_version", "")).utf8().get_data();

	server_info.address = ip_storage.c_str();
	server_info.rtspSessionUrl = session_url_storage.c_str();
	server_info.serverInfoAppVersion = app_version_storage.c_str();
	server_info.serverInfoGfeVersion = gfe_version_storage.c_str();
	server_info.serverCodecModeSupport = options.get("server_codec_mode_support", 1);

	is_streaming = true;

	// 2. Start Threads
	connection_thread.instantiate();
	connection_thread->start(callable_mp(this, &MoonlightStreamCore::_thread_func_start_connection));

	video_thread.instantiate();
	video_thread->start(callable_mp(this, &MoonlightStreamCore::_thread_func_video));
}

void MoonlightStreamCore::stop_play_stream() {
	bool was_streaming = is_streaming;
	is_streaming = false;

	if (was_streaming) {
		UtilityFunctions::print(LOG_PREFIX "Stopping stream...");
		LiInterruptConnection();
		LiWakeWaitForVideoFrame();
	}

	// Stop Li Connection first via API if still running?
	// Usually LiInterruptConnection causes LiStartConnection to return.
	// But explicit stop is good practice.
	LiStopConnection();

	if (connection_thread.is_valid()) {
		if (connection_thread->is_started()) {
			connection_thread->wait_to_finish();
		}
		connection_thread.unref();
	}

	if (video_thread.is_valid()) {
		if (video_thread->is_started()) {
			video_thread->wait_to_finish();
		}
		video_thread.unref();
	}

	_cleanup_ffmpeg_video();
	_cleanup_ffmpeg_audio();

	reset_render_target();
	// reset_audio_stream(); // User might want to keep the stream object, so don't auto reset unless requested
}

void MoonlightStreamCore::set_render_target(TextureRect *target) {
	display_rect = target;
	if (display_rect) {
		if (display_texture.is_null()) {
			display_texture.instantiate();
			int w = stream_config.width > 0 ? stream_config.width : 1280;
			int h = stream_config.height > 0 ? stream_config.height : 720;
			Ref<Image> img = Image::create(w, h, false, Image::FORMAT_RGBA8);
			img->fill(Color(0, 0, 0));
			display_texture->set_image(img);
		}
		display_rect->set_texture(display_texture);
	}
}

void MoonlightStreamCore::reset_render_target() {
	display_rect = nullptr;
}

Ref<AudioStream> MoonlightStreamCore::get_audio_stream() {
	if (audio_stream.is_null()) {
		audio_stream.instantiate();
	}
	return audio_stream;
}

void MoonlightStreamCore::reset_audio_stream(bool free_stream) {
	if (audio_stream.is_valid()) {
		audio_stream->clear_buffer();
		if (free_stream) {
			audio_stream.unref();
		}
	}
}

// ============================================================================
// Thread Functions
// ============================================================================

void MoonlightStreamCore::_thread_func_start_connection() {
	if (lib_global_mutex)
		lib_global_mutex->lock();

	UtilityFunctions::print(LOG_PREFIX "Starting connection to ", server_info.address);
	int ret = LiStartConnection(&server_info, &stream_config, &cl_callbacks, &dr_callbacks, &ar_callbacks, this, 0, this, 0);

	if (ret != 0) {
		UtilityFunctions::printerr(LOG_PREFIX "LiStartConnection failed: ", ret);
	}

	// Stream ended
	is_streaming = false;
	LiStopConnection(); // Clean up internal state

	if (lib_global_mutex)
		lib_global_mutex->unlock();

	// Ensure video thread wakes up and exits
	LiWakeWaitForVideoFrame();
}

void MoonlightStreamCore::_thread_func_video() {
	UtilityFunctions::print(LOG_PREFIX "Video thread started");

	VIDEO_FRAME_HANDLE frame_handle;
	PDECODE_UNIT decode_unit;

	// Lazy alloc packets
	if (v_packet == nullptr)
		v_packet = av_packet_alloc();
	if (v_frame == nullptr)
		v_frame = av_frame_alloc();

	while (is_streaming) {
		if (LiWaitForNextVideoFrame(&frame_handle, &decode_unit)) {
			if (v_codec_ctx) {
				// Assemble packet
				if (av_new_packet(v_packet, decode_unit->fullLength) >= 0) {
					int offset = 0;
					PLENTRY entry = decode_unit->bufferList;
					while (entry != nullptr) {
						memcpy(v_packet->data + offset, entry->data, entry->length);
						offset += entry->length;
						entry = entry->next;
					}

					// Send to decoder
					int ret = avcodec_send_packet(v_codec_ctx, v_packet);
					av_packet_unref(v_packet);

					if (ret >= 0) {
						while (true) {
							ret = avcodec_receive_frame(v_codec_ctx, v_frame);
							if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
								break;
							if (ret < 0)
								break;

							// Process Frame
							// Note: Ideally we would use GPU texture sharing here.
							// For now, we perform sws_scale to RGBA for Godot Image.

							int w = v_frame->width;
							int h = v_frame->height;

							if (w > 0 && h > 0) {
								if (!sws_ctx || video_width != w || video_height != h) {
									if (sws_ctx)
										sws_freeContext(sws_ctx);
									sws_ctx = sws_getContext(w, h, (AVPixelFormat)v_frame->format,
											w, h, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
									video_width = w;
									video_height = h;
								}

								PackedByteArray img_data;
								img_data.resize(w * h * 4);

								uint8_t *dest[4] = { img_data.ptrw(), nullptr, nullptr, nullptr };
								int dest_linesize[4] = { w * 4, 0, 0, 0 };

								// Hardware frames need transfer, but usually avcodec_receive_frame
								// handles transfer to SW frame if not configured for HW surfaces only.
								// If we used a HW device context, v_frame might be NV12/etc.
								// sws_scale handles most logic.

								sws_scale(sws_ctx, v_frame->data, v_frame->linesize, 0, h, dest, dest_linesize);

								if (video_mutex.is_valid()) {
									video_mutex->lock();
									last_decoded_image = Image::create_from_data(w, h, false, Image::FORMAT_RGBA8, img_data);
									new_frame_available = true;
									video_mutex->unlock();
								}
							}
						}
					}
				}
						}

			LiCompleteVideoFrame(frame_handle, DR_OK);
		} else {
			if (is_streaming)
				OS::get_singleton()->delay_usec(1000);
		}
	}
	UtilityFunctions::print(LOG_PREFIX "Video thread exiting");
}

// ============================================================================
// FFmpeg Logic
// ============================================================================

Vector<String> MoonlightStreamCore::_get_candidate_decoders(int format_mask) {
	String platform = OS::get_singleton()->get_name();
	Vector<String> candidates;

	bool is_h264 = (format_mask & VIDEO_FORMAT_MASK_H264);
	bool is_hevc = (format_mask & VIDEO_FORMAT_MASK_H265);
	bool is_av1 = (format_mask & VIDEO_FORMAT_MASK_AV1);

	// Priority: HW Decoder > Standard Decoder
	if (platform == "Windows") {
		if (is_h264) {
			candidates.push_back("h264_cuvid"); // Nvidia
			candidates.push_back("h264_qsv"); // Intel
			candidates.push_back("h264_amf"); // AMD
		} else if (is_hevc) {
			candidates.push_back("hevc_cuvid");
			candidates.push_back("hevc_qsv");
			candidates.push_back("hevc_amf");
		} else if (is_av1) {
			candidates.push_back("av1_cuvid");
			candidates.push_back("av1_qsv");
			candidates.push_back("av1_amf");
		}
	} else if (platform == "Linux" || platform == "FreeBSD") {
		if (is_h264) {
			candidates.push_back("h264_cuvid");
			candidates.push_back("h264_v4l2m2m");
			candidates.push_back("h264_vaapi");
		} else if (is_hevc) {
			candidates.push_back("hevc_cuvid");
			candidates.push_back("hevc_vaapi");
		}
	} else if (platform == "Android") {
		if (is_h264)
			candidates.push_back("h264_mediacodec");
		else if (is_hevc)
			candidates.push_back("hevc_mediacodec");
	} else if (platform == "macOS" || platform == "iOS") {
		if (is_h264)
			candidates.push_back("h264_videotoolbox");
		else if (is_hevc)
			candidates.push_back("hevc_videotoolbox");
	}

	return candidates;
}

int MoonlightStreamCore::_try_open_decoder(const String &codec_name, int width, int height) {
	const AVCodec *codec = avcodec_find_decoder_by_name(codec_name.utf8().get_data());
	if (!codec)
		return -1;

	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx)
		return -1;

	ctx->width = width;
	ctx->height = height;

	// Critical for low latency streaming
	ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	ctx->flags2 |= AV_CODEC_FLAG2_FAST;

	// Multithreading
	ctx->thread_count = 4;
	ctx->thread_type = FF_THREAD_SLICE | FF_THREAD_FRAME;

	// 尝试打开解码器，这是关键步骤
	// 如果缺少 DLL (如 nvcuvid.dll) 或硬件不支持，这里会返回负值
	if (avcodec_open2(ctx, codec, nullptr) < 0) {
		// UtilityFunctions::print(LOG_PREFIX "Optional: Failed to open candidate codec: ", codec_name);
		avcodec_free_context(&ctx);
		return -1;
	}

	// 成功打开，移交给成员变量
	v_codec = codec;
	v_codec_ctx = ctx;

	if (!v_frame)
		v_frame = av_frame_alloc();
	if (!v_packet)
		v_packet = av_packet_alloc();

	return 0;
}

int MoonlightStreamCore::_handle_dr_setup(int video_fmt, int width, int height) {
	_cleanup_ffmpeg_video();
	UtilityFunctions::print(LOG_PREFIX "Setup Video: Fmt=", video_fmt, " ", width, "x", height);

	Vector<String> candidates = _get_candidate_decoders(video_fmt);
	bool opened = false;

	// 1. 尝试硬件解码器
	for (int i = 0; i < candidates.size(); i++) {
		if (_try_open_decoder(candidates[i], width, height) == 0) {
			UtilityFunctions::print(LOG_PREFIX "Selected HW Video Codec: ", candidates[i]);
			opened = true;
			break;
		}
	}

	// 2. 如果硬件解码失败，尝试软解
	if (!opened) {
		String sw_name = "";
		if (video_fmt & VIDEO_FORMAT_MASK_H264)
			sw_name = "h264";
		else if (video_fmt & VIDEO_FORMAT_MASK_H265)
			sw_name = "hevc";
		else if (video_fmt & VIDEO_FORMAT_MASK_AV1)
			sw_name = "av1";

		if (!sw_name.is_empty()) {
			UtilityFunctions::print(LOG_PREFIX "HW decoders failed. Trying software: ", sw_name);
			if (_try_open_decoder(sw_name, width, height) == 0) {
				opened = true;
			}
		}
	}

	if (!opened) {
		UtilityFunctions::printerr(LOG_PREFIX "No usable decoder found for format ", video_fmt);
		return -1;
	}

	video_width = width;
	video_height = height;
	video_format = video_fmt;

	return DR_OK;
}

void MoonlightStreamCore::_cleanup_ffmpeg_video() {
	if (sws_ctx) {
		sws_freeContext(sws_ctx);
		sws_ctx = nullptr;
	}
	if (v_codec_ctx) {
		avcodec_free_context(&v_codec_ctx);
		v_codec_ctx = nullptr;
	}
	if (v_frame) {
		av_frame_free(&v_frame);
		v_frame = nullptr;
	}
	if (v_packet) {
		av_packet_free(&v_packet);
		v_packet = nullptr;
	}
}

int MoonlightStreamCore::_handle_ar_init(int audio_cfg) {
	_cleanup_ffmpeg_audio();

	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
	if (!codec)
		return -1;

	a_codec_ctx = avcodec_alloc_context3(codec);
	av_channel_layout_default(&a_codec_ctx->ch_layout, 2);
	a_codec_ctx->sample_rate = 48000;
	a_codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY; // Opus is naturally low delay, but good to set

	if (avcodec_open2(a_codec_ctx, codec, nullptr) < 0)
		return -1;

	a_frame = av_frame_alloc();
	a_packet = av_packet_alloc();
	swr_ctx = swr_alloc();

	AVChannelLayout stereo;
	av_channel_layout_default(&stereo, 2);

	av_opt_set_chlayout(swr_ctx, "in_chlayout", &a_codec_ctx->ch_layout, 0);
	av_opt_set_int(swr_ctx, "in_sample_rate", a_codec_ctx->sample_rate, 0);
	av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", a_codec_ctx->sample_fmt, 0);

	av_opt_set_chlayout(swr_ctx, "out_chlayout", &stereo, 0);
	av_opt_set_int(swr_ctx, "out_sample_rate", 48000, 0);
	av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

	swr_init(swr_ctx);
	return 0;
}

void MoonlightStreamCore::_handle_ar_decode_and_play_sample(char *data, int len) {
	if (!a_codec_ctx || audio_stream.is_null())
		return;

	if (av_new_packet(a_packet, len) < 0)
		return;
	memcpy(a_packet->data, data, len);

	int ret = avcodec_send_packet(a_codec_ctx, a_packet);
	av_packet_unref(a_packet);

	if (ret < 0)
		return;

	while (ret >= 0) {
		ret = avcodec_receive_frame(a_codec_ctx, a_frame);
		if (ret < 0)
			break;

		int max_samples = swr_get_out_samples(swr_ctx, a_frame->nb_samples);
		if (max_samples > 0) {
			float *buffer = (float *)av_malloc(max_samples * 2 * sizeof(float));
			int out_samples = swr_convert(swr_ctx, (uint8_t **)&buffer, max_samples,
					(const uint8_t **)a_frame->data, a_frame->nb_samples);

			if (out_samples > 0) {
				audio_stream->push_audio(buffer, out_samples * 2);
			}
			av_free(buffer);
		}
	}
}

void MoonlightStreamCore::_cleanup_ffmpeg_audio() {
	if (swr_ctx) {
		swr_free(&swr_ctx);
		swr_ctx = nullptr;
	}
	if (a_codec_ctx) {
		avcodec_free_context(&a_codec_ctx);
		a_codec_ctx = nullptr;
	}
	if (a_frame) {
		av_frame_free(&a_frame);
		a_frame = nullptr;
	}
	if (a_packet) {
		av_packet_free(&a_packet);
		a_packet = nullptr;
	}
}

// ============================================================================
// Process / Update
// ============================================================================

void MoonlightStreamCore::_process(double delta) {
	if (!is_inside_tree())
		return;

	if (new_frame_available && video_mutex.is_valid()) {
		video_mutex->lock();
		if (last_decoded_image.is_valid() && display_texture.is_valid()) {
			display_texture->update(last_decoded_image);
		}
		new_frame_available = false;
		video_mutex->unlock();
	}
}

// ============================================================================
// Callbacks (Static -> Instance)
// ============================================================================

void MoonlightStreamCore::_cl_stage_starting(int stage) {
	UtilityFunctions::print(LOG_PREFIX "Stage: ", stage);
}
void MoonlightStreamCore::_cl_connection_started() {
	UtilityFunctions::print(LOG_PREFIX "Connection Started");
}
void MoonlightStreamCore::_cl_connection_terminated(int error_code) {
	UtilityFunctions::print(LOG_PREFIX "Connection Terminated: ", error_code);
}
void MoonlightStreamCore::_cl_log_message(const char *format, ...) {
	// Simple logging wrapper
}

int MoonlightStreamCore::_dr_setup(int fmt, int w, int h, int rate, void *ctx, int flags) {
	return ((MoonlightStreamCore *)ctx)->_handle_dr_setup(fmt, w, h);
}
void MoonlightStreamCore::_dr_cleanup(void) {
	if (singleton_instance)
		singleton_instance->_cleanup_ffmpeg_video();
}
int MoonlightStreamCore::_dr_submit_decode_unit(PDECODE_UNIT du) { return DR_OK; }

int MoonlightStreamCore::_ar_init(int cfg, const POPUS_MULTISTREAM_CONFIGURATION opus, void *ctx, int flags) {
	return ((MoonlightStreamCore *)ctx)->_handle_ar_init(cfg);
}
void MoonlightStreamCore::_ar_cleanup(void) {
	if (singleton_instance)
		singleton_instance->_cleanup_ffmpeg_audio();
}
void MoonlightStreamCore::_ar_decode_and_play_sample(char *data, int len) {
	if (singleton_instance)
		singleton_instance->_handle_ar_decode_and_play_sample(data, len);
}

void MoonlightStreamCore::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start_play_stream", "options"), &MoonlightStreamCore::start_play_stream);
	ClassDB::bind_method(D_METHOD("stop_play_stream"), &MoonlightStreamCore::stop_play_stream);
	ClassDB::bind_method(D_METHOD("set_render_target", "texture_rect"), &MoonlightStreamCore::set_render_target);
	ClassDB::bind_method(D_METHOD("reset_render_target"), &MoonlightStreamCore::reset_render_target);
	ClassDB::bind_method(D_METHOD("get_audio_stream"), &MoonlightStreamCore::get_audio_stream);
	ClassDB::bind_method(D_METHOD("reset_audio_stream", "free_stream"), &MoonlightStreamCore::reset_audio_stream, DEFVAL(false));
}
