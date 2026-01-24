#include "stream_core.h"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstdarg>
#include <cstdio>

// FFmpeg Includes
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#define LOG_PREFIX "[Moonlight-StreamCore] "

// Codec Families
#define CODEC_FAMILY_H264 0
#define CODEC_FAMILY_H265 1
#define CODEC_FAMILY_AV1 2

using namespace godot;

static MoonlightStreamCore *singleton_instance = nullptr;
Mutex *MoonlightStreamCore::lib_global_mutex = nullptr;

// ============================================================================
// AudioStreamPlaybackMoonlight
// ============================================================================

AudioStreamPlaybackMoonlight::AudioStreamPlaybackMoonlight() : active(false) {}
AudioStreamPlaybackMoonlight::~AudioStreamPlaybackMoonlight() {}

void AudioStreamPlaybackMoonlight::_start(double p_from_pos) { active = true; }
void AudioStreamPlaybackMoonlight::_stop() { active = false; }
bool AudioStreamPlaybackMoonlight::_is_playing() const { return active; }
int32_t AudioStreamPlaybackMoonlight::_get_loop_count() const { return 0; }
double AudioStreamPlaybackMoonlight::_get_playback_position() const { return 0.0; }
void AudioStreamPlaybackMoonlight::_seek(double p_time) {}

int32_t AudioStreamPlaybackMoonlight::_mix_resampled(AudioFrame *p_buffer, int32_t p_frames) {
	if (!active || base.is_null())
		return 0;
	return base->read_samples(p_buffer, p_frames);
}

float AudioStreamPlaybackMoonlight::_get_stream_sampling_rate() const {
	if (base.is_valid())
		return (float)base->mix_rate;
	return 48000.0f;
}

void AudioStreamPlaybackMoonlight::_bind_methods() {}

// ============================================================================
// AudioStreamMoonlight
// ============================================================================

AudioStreamMoonlight::AudioStreamMoonlight() : mix_rate(48000) {
	buffer_mutex.instantiate();
	rb_capacity = 48000 * 2 * 0.2; // 200ms buffer
	ring_buffer.resize(rb_capacity);
	ring_buffer.fill(0);
}

Ref<AudioStreamPlayback> AudioStreamMoonlight::_instantiate_playback() const {
	Ref<AudioStreamPlaybackMoonlight> playback;
	playback.instantiate();
	playback->base = Ref<AudioStreamMoonlight>(this);
	return playback;
}

String AudioStreamMoonlight::_get_stream_name() const { return "Moonlight Audio"; }

void AudioStreamMoonlight::push_audio(const float *samples, int count) {
	buffer_mutex->lock();

	int free_space = rb_capacity - rb_used;
	if (count > free_space) {
		int overflow = count - free_space;
		rb_read_pos = (rb_read_pos + overflow) % rb_capacity;
		rb_used -= overflow;
	}

	int first_chunk = MIN(count, rb_capacity - rb_write_pos);
	int second_chunk = count - first_chunk;

	float *ptr = ring_buffer.ptrw();
	memcpy(ptr + rb_write_pos, samples, first_chunk * sizeof(float));
	if (second_chunk > 0)
		memcpy(ptr, samples + first_chunk, second_chunk * sizeof(float));

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
// MoonlightStreamCore
// ============================================================================

MoonlightStreamCore::MoonlightStreamCore() {
	singleton_instance = this;
	is_streaming = false;
	new_frame_available = false;
	selected_codec_config = CODEC_H264;

	texture_mutex.instantiate();
	queue_mutex.instantiate();
	decode_sem.instantiate();

	if (lib_global_mutex == nullptr) {
		lib_global_mutex = memnew(Mutex);
	}

	// Initialize all callbacks to zero to prevent garbage pointers
	memset(&stream_config, 0, sizeof(stream_config));
	memset(&server_info, 0, sizeof(server_info));
	memset(&cl_callbacks, 0, sizeof(cl_callbacks));
	memset(&dr_callbacks, 0, sizeof(dr_callbacks));
	memset(&ar_callbacks, 0, sizeof(ar_callbacks));

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
	dr_callbacks.submitDecodeUnit = nullptr; // Must be NULL for CAPABILITY_PULL_RENDERER
	dr_callbacks.capabilities = CAPABILITY_PULL_RENDERER;

	ar_callbacks.init = _ar_init;
	ar_callbacks.cleanup = _ar_cleanup;
	ar_callbacks.decodeAndPlaySample = _ar_decode_and_play_sample;

	// Critical: Ensure audio stream is created immediately so we don't drop packets
	get_audio_stream();
}

MoonlightStreamCore::~MoonlightStreamCore() {
	stop_play_stream();
	if (singleton_instance == this)
		singleton_instance = nullptr;
}

void MoonlightStreamCore::start_play_stream(Dictionary options) {
	// 1. Cleanup previous session thoroughly
	if (is_streaming || (connection_thread.is_valid() && connection_thread->is_started())) {
		UtilityFunctions::print(LOG_PREFIX "Stream already running, stopping first...");
		stop_play_stream();
		// WAIT: Give underlying C library time to reset global state (winsock, etc)
		OS::get_singleton()->delay_usec(1000000); // 1.0 second delay
	}

	// Ensure audio stream exists and is cleared
	get_audio_stream();
	if (audio_stream.is_valid()) {
		audio_stream->clear_buffer();
	}

	// Reset callbacks again to ensure clean state
	memset(&dr_callbacks, 0, sizeof(dr_callbacks));
	LiInitializeVideoCallbacks(&dr_callbacks);
	dr_callbacks.setup = _dr_setup;
	dr_callbacks.cleanup = _dr_cleanup;
	dr_callbacks.submitDecodeUnit = nullptr;
	dr_callbacks.capabilities = CAPABILITY_PULL_RENDERER;

	// 2. Codec Selection
	String codec_str = options.get("video_codec", "H264");
	if (codec_str == "H265" || codec_str == "HEVC")
		selected_codec_config = CODEC_H265;
	else if (codec_str == "AV1")
		selected_codec_config = CODEC_AV1;
	else
		selected_codec_config = CODEC_H264;

	int supported_formats = _probe_video_format(selected_codec_config);
	UtilityFunctions::print(LOG_PREFIX "Codec Selection: ", codec_str, " | Mask: 0x", String::num_int64(supported_formats, 16));

	// 3. Configure Stream
	LiInitializeStreamConfiguration(&stream_config);
	stream_config.width = options.get("width", 1280);
	stream_config.height = options.get("height", 720);
	stream_config.fps = options.get("fps", 60);
	stream_config.bitrate = options.get("bitrate", 10000);
	stream_config.packetSize = options.get("packet_size", 1392);
	stream_config.streamingRemotely = STREAM_CFG_AUTO;
	stream_config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
	stream_config.supportedVideoFormats = supported_formats;

	if (options.has("surround_audio_info")) {
		int surround_info = options["surround_audio_info"];
		int count = (surround_info >> 8) & 0xFF;
		if (count == 6)
			stream_config.audioConfiguration = AUDIO_CONFIGURATION_51_SURROUND;
		if (count == 8)
			stream_config.audioConfiguration = AUDIO_CONFIGURATION_71_SURROUND;
	}

	if (options.has("color_space"))
		stream_config.colorSpace = options["color_space"];
	if (options.has("color_range"))
		stream_config.colorRange = options["color_range"];

	String rikey = options.get("rikey", "");
	if (!rikey.is_empty()) {
		PackedByteArray key_bytes = rikey.hex_decode();
		if (key_bytes.size() >= 16) {
			memcpy(stream_config.remoteInputAesKey, key_bytes.ptr(), 16);
		}
		memset(stream_config.remoteInputAesIv, 0, 16);
		stream_config.encryptionFlags = ENCFLG_NONE;
	}

	// 4. Server Info
	ip_storage = String(options.get("ip", "")).utf8().get_data();
	session_url_storage = String(options.get("session_url", "")).utf8().get_data();
	app_version_storage = String(options.get("app_version", "0.0.0.0")).utf8().get_data();
	gfe_version_storage = String(options.get("gfe_version", "")).utf8().get_data();

	server_info.address = ip_storage.c_str();
	server_info.rtspSessionUrl = session_url_storage.c_str();
	server_info.serverInfoAppVersion = app_version_storage.c_str();
	server_info.serverInfoGfeVersion = gfe_version_storage.c_str();
	server_info.serverCodecModeSupport = options.get("server_codec_mode_support", 0);

	is_streaming = true;

	// 5. Start Threads
	if (connection_thread.is_valid()) {
		connection_thread->wait_to_finish();
		connection_thread.unref();
	}
	connection_thread.instantiate();
	connection_thread->start(callable_mp(this, &MoonlightStreamCore::_thread_func_connection));

	if (video_pull_thread.is_valid()) {
		video_pull_thread->wait_to_finish();
		video_pull_thread.unref();
	}
	video_pull_thread.instantiate();
	video_pull_thread->start(callable_mp(this, &MoonlightStreamCore::_thread_func_video_pull));

	if (video_decode_thread.is_valid()) {
		video_decode_thread->wait_to_finish();
		video_decode_thread.unref();
	}
	video_decode_thread.instantiate();
	video_decode_thread->start(callable_mp(this, &MoonlightStreamCore::_thread_func_video_decode));
}

void MoonlightStreamCore::stop_play_stream() {
	if (!is_streaming)
		return;

	UtilityFunctions::print(LOG_PREFIX "Stopping stream...");

	// 1. Set Flag
	is_streaming = false;

	// 2. Unblock Decoder Thread
	decode_sem->post();

	// 3. Break blocking network calls
	LiInterruptConnection();
	LiWakeWaitForVideoFrame();

	// 4. Stop Library
	LiStopConnection();

	// 5. Join Threads
	if (connection_thread.is_valid()) {
		connection_thread->wait_to_finish();
		connection_thread.unref();
	}
	if (video_pull_thread.is_valid()) {
		video_pull_thread->wait_to_finish();
		video_pull_thread.unref();
	}
	if (video_decode_thread.is_valid()) {
		video_decode_thread->wait_to_finish();
		video_decode_thread.unref();
	}

	// 6. Cleanup FFmpeg & Queue
	queue_mutex->lock();
	while (packet_queue.size() > 0) {
		AVPacket *pkt = packet_queue.front()->get();
		packet_queue.pop_front();
		av_packet_free(&pkt);
	}
	queue_mutex->unlock();

	_cleanup_ffmpeg_video();
	_cleanup_ffmpeg_audio();
	reset_render_target();
}

void MoonlightStreamCore::set_render_target(TextureRect *target) {
	display_rect = target;
	if (display_rect) {
		if (display_texture.is_null()) {
			display_texture.instantiate();
			Ref<Image> img = Image::create(1280, 720, false, Image::FORMAT_RGBA8);
			img->fill(Color(0, 0, 0, 1));
			display_texture->set_image(img);
		}
		display_rect->set_texture(display_texture);
	}
}
void MoonlightStreamCore::reset_render_target() { display_rect = nullptr; }
Ref<AudioStream> MoonlightStreamCore::get_audio_stream() {
	if (audio_stream.is_null())
		audio_stream.instantiate();
	return audio_stream;
}
void MoonlightStreamCore::reset_audio_stream(bool free_stream) {
	if (audio_stream.is_valid()) {
		audio_stream->clear_buffer();
		if (free_stream)
			audio_stream.unref();
	}
}

void MoonlightStreamCore::_process(double delta) {
	if (!is_streaming)
		return;
	if (new_frame_available && texture_mutex.is_valid()) {
		texture_mutex->lock();
		if (last_decoded_image.is_valid() && display_texture.is_valid()) {
			display_texture->update(last_decoded_image);
		}
		new_frame_available = false;
		texture_mutex->unlock();
	}
}

// ============================================================================
// Thread Logic
// ============================================================================

void MoonlightStreamCore::_thread_func_connection() {
	int res = LiStartConnection(&server_info, &stream_config, &cl_callbacks, &dr_callbacks, &ar_callbacks, this, 0, this, 0);
	if (res != 0)
		UtilityFunctions::printerr(LOG_PREFIX "Connection failed: ", res);
	is_streaming = false;
	decode_sem->post();
	LiWakeWaitForVideoFrame();
}

void MoonlightStreamCore::_thread_func_video_pull() {
	UtilityFunctions::print(LOG_PREFIX "Video Pull Thread Started");
	VIDEO_FRAME_HANDLE frame_handle;
	PDECODE_UNIT decode_unit;

	while (is_streaming) {
		// BLOCKING: Wait for frame data from network
		if (LiWaitForNextVideoFrame(&frame_handle, &decode_unit)) {
			// Create Packet
			AVPacket *pkt = av_packet_alloc();
			bool packet_ready = false;

			if (pkt && av_new_packet(pkt, decode_unit->fullLength) >= 0) {
				int offset = 0;
				PLENTRY entry = decode_unit->bufferList;
				while (entry != nullptr) {
					if (entry->length > 0 && entry->data) {
						memcpy(pkt->data + offset, entry->data, entry->length);
						offset += entry->length;
					}
					entry = entry->next;
				}
				packet_ready = true;
			}

			// CRITICAL: Release the network buffer immediately
			LiCompleteVideoFrame(frame_handle, DR_OK);

			if (packet_ready) {
				queue_mutex->lock();
				packet_queue.push_back(pkt);
				queue_mutex->unlock();
				// Wake up decoder
				decode_sem->post();
			} else {
				if (pkt)
					av_packet_free(&pkt);
			}
		} else {
			// Interrupt or failure
			if (is_streaming)
				OS::get_singleton()->delay_usec(1000);
		}
	}
	UtilityFunctions::print(LOG_PREFIX "Video Pull Thread Exited");
}

void MoonlightStreamCore::_thread_func_video_decode() {
	UtilityFunctions::print(LOG_PREFIX "Video Decode Thread Started");

	if (!v_frame)
		v_frame = av_frame_alloc();

	while (is_streaming) {
		// Wait for packet
		decode_sem->wait();
		if (!is_streaming)
			break;

		AVPacket *pkt = nullptr;
		queue_mutex->lock();
		if (packet_queue.size() > 0) {
			pkt = packet_queue.front()->get();
			packet_queue.pop_front();
		}
		queue_mutex->unlock();

		if (pkt) {
			if (v_codec_ctx) {
				int ret = avcodec_send_packet(v_codec_ctx, pkt);
				if (ret >= 0) {
					while (true) {
						ret = avcodec_receive_frame(v_codec_ctx, v_frame);
						if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
							break;
						if (ret < 0)
							break;

						// Got Frame -> Convert -> Display
						int w = v_frame->width;
						int h = v_frame->height;
						if (w > 0 && h > 0) {
							if (!sws_ctx || video_width != w || video_height != h) {
								if (sws_ctx)
									sws_freeContext(sws_ctx);
								sws_ctx = sws_getContext(w, h, (AVPixelFormat)v_frame->format, w, h, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
								video_width = w;
								video_height = h;
							}

							PackedByteArray img_data;
							img_data.resize(w * h * 4);
							uint8_t *dest[4] = { img_data.ptrw(), nullptr, nullptr, nullptr };
							int dest_linesize[4] = { w * 4, 0, 0, 0 };

							sws_scale(sws_ctx, v_frame->data, v_frame->linesize, 0, h, dest, dest_linesize);

							if (texture_mutex.is_valid()) {
								texture_mutex->lock();
								last_decoded_image = Image::create_from_data(w, h, false, Image::FORMAT_RGBA8, img_data);
								new_frame_available = true;
								texture_mutex->unlock();
							}
						}
					}
				}
			}
			av_packet_free(&pkt);
		}
	}
	UtilityFunctions::print(LOG_PREFIX "Video Decode Thread Exited");
}

// ============================================================================
// FFmpeg Helper Methods
// ============================================================================

int MoonlightStreamCore::_probe_video_format(VideoCodecConfig preference) {
	int supported_mask = 0;
	int test_w = 1280;
	int test_h = 720;

	bool h264_ok = false;
	Vector<String> h264_candidates = _get_candidate_decoders(CODEC_FAMILY_H264);
	for (int i = 0; i < h264_candidates.size(); i++) {
		if (_try_open_decoder(h264_candidates[i], test_w, test_h) == 0) {
			h264_ok = true;
			_cleanup_ffmpeg_video();
			break;
		}
	}

	bool hevc_ok = false;
	if (preference == CODEC_H265 || preference == CODEC_AUTO) {
		Vector<String> hevc_candidates = _get_candidate_decoders(CODEC_FAMILY_H265);
		for (int i = 0; i < hevc_candidates.size(); i++) {
			if (_try_open_decoder(hevc_candidates[i], test_w, test_h) == 0) {
				hevc_ok = true;
				_cleanup_ffmpeg_video();
				break;
			}
		}
	}

	bool av1_ok = false;
	if (preference == CODEC_AV1 || preference == CODEC_AUTO) {
		Vector<String> av1_candidates = _get_candidate_decoders(CODEC_FAMILY_AV1);
		for (int i = 0; i < av1_candidates.size(); i++) {
			if (_try_open_decoder(av1_candidates[i], test_w, test_h) == 0) {
				av1_ok = true;
				_cleanup_ffmpeg_video();
				break;
			}
		}
	}

	if (preference == CODEC_AV1 && av1_ok)
		supported_mask |= VIDEO_FORMAT_MASK_AV1;
	else if (preference == CODEC_H265 && hevc_ok)
		supported_mask |= VIDEO_FORMAT_MASK_H265;
	else if (h264_ok)
		supported_mask |= VIDEO_FORMAT_MASK_H264;

	if (preference == CODEC_AUTO) {
		if (h264_ok)
			supported_mask |= VIDEO_FORMAT_MASK_H264;
		if (hevc_ok)
			supported_mask |= VIDEO_FORMAT_MASK_H265;
		if (av1_ok)
			supported_mask |= VIDEO_FORMAT_MASK_AV1;
	}

	if (supported_mask == 0) {
		UtilityFunctions::printerr(LOG_PREFIX "Warning: No supported decoders found, forcing H.264");
		supported_mask = VIDEO_FORMAT_MASK_H264;
	}
	return supported_mask;
}

Vector<String> MoonlightStreamCore::_get_candidate_decoders(int codec_family) {
	String platform = OS::get_singleton()->get_name();
	String adapter = RenderingServer::get_singleton()->get_video_adapter_name().to_lower();
	Vector<String> candidates;

	bool is_nvidia = adapter.contains("nvidia") || adapter.contains("geforce") || adapter.contains("quadro");
	bool is_amd = adapter.contains("amd") || adapter.contains("radeon");
	bool is_intel = adapter.contains("intel") || adapter.contains("uhd") || adapter.contains("iris") || adapter.contains("arc");

	UtilityFunctions::print(LOG_PREFIX "Detected GPU: ", adapter);

	if (platform == "Windows") {
		// Hardware wrappers
		if (is_nvidia) {
			if (codec_family == CODEC_FAMILY_H264)
				candidates.push_back("h264_cuvid");
			else if (codec_family == CODEC_FAMILY_H265)
				candidates.push_back("hevc_cuvid");
			else if (codec_family == CODEC_FAMILY_AV1)
				candidates.push_back("av1_cuvid");
		}
		if (is_amd) {
			if (codec_family == CODEC_FAMILY_H264)
				candidates.push_back("h264_amf");
			else if (codec_family == CODEC_FAMILY_H265)
				candidates.push_back("hevc_amf");
			else if (codec_family == CODEC_FAMILY_AV1)
				candidates.push_back("av1_amf");
		}
		if (is_intel) {
			if (codec_family == CODEC_FAMILY_H264)
				candidates.push_back("h264_qsv");
			else if (codec_family == CODEC_FAMILY_H265)
				candidates.push_back("hevc_qsv");
			else if (codec_family == CODEC_FAMILY_AV1)
				candidates.push_back("av1_qsv");
		}
		// Windows Generic Hardware (Media Foundation)
		if (codec_family == CODEC_FAMILY_H264)
			candidates.push_back("h264_mf");
		else if (codec_family == CODEC_FAMILY_H265)
			candidates.push_back("hevc_mf");
	} else if (platform == "Linux" || platform == "FreeBSD") {
		if (codec_family == CODEC_FAMILY_H264) {
			candidates.push_back("h264_cuvid");
			candidates.push_back("h264_vaapi");
			candidates.push_back("h264_v4l2m2m");
		} else if (codec_family == CODEC_FAMILY_H265) {
			candidates.push_back("hevc_cuvid");
			candidates.push_back("hevc_vaapi");
		} else if (codec_family == CODEC_FAMILY_AV1) {
			candidates.push_back("av1_cuvid");
			candidates.push_back("av1_vaapi");
		}
	} else if (platform == "Android") {
		if (codec_family == CODEC_FAMILY_H264)
			candidates.push_back("h264_mediacodec");
		else if (codec_family == CODEC_FAMILY_H265)
			candidates.push_back("hevc_mediacodec");
		else if (codec_family == CODEC_FAMILY_AV1)
			candidates.push_back("av1_mediacodec");
	} else if (platform == "macOS" || platform == "iOS") {
		if (codec_family == CODEC_FAMILY_H264)
			candidates.push_back("h264_videotoolbox");
		else if (codec_family == CODEC_FAMILY_H265)
			candidates.push_back("hevc_videotoolbox");
	}

	// Software Fallback
	if (codec_family == CODEC_FAMILY_H264)
		candidates.push_back("h264");
	else if (codec_family == CODEC_FAMILY_H265)
		candidates.push_back("hevc");
	else if (codec_family == CODEC_FAMILY_AV1) {
		candidates.push_back("libdav1d");
		candidates.push_back("av1");
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
	ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	ctx->flags2 |= AV_CODEC_FLAG2_FAST;

	int thread_count = OS::get_singleton()->get_processor_count();
	if (thread_count > 4)
		thread_count = 4;
	ctx->thread_count = thread_count;
	if (codec->capabilities & AV_CODEC_CAP_FRAME_THREADS)
		ctx->thread_type = FF_THREAD_FRAME;
	else if (codec->capabilities & AV_CODEC_CAP_SLICE_THREADS)
		ctx->thread_type = FF_THREAD_SLICE;
	else
		ctx->thread_count = 1;

	if (avcodec_open2(ctx, codec, nullptr) < 0) {
		avcodec_free_context(&ctx);
		return -1;
	}

	v_codec = codec;
	v_codec_ctx = ctx;
	if (!v_frame)
		v_frame = av_frame_alloc();

	return 0;
}

int MoonlightStreamCore::_handle_dr_setup(int video_fmt, int width, int height) {
	_cleanup_ffmpeg_video();
	UtilityFunctions::print(LOG_PREFIX "Setup Video: Fmt=0x", String::num_int64(video_fmt, 16), " Size=", width, "x", height);

	int family = -1;
	if (video_fmt & VIDEO_FORMAT_MASK_H264)
		family = CODEC_FAMILY_H264;
	else if (video_fmt & VIDEO_FORMAT_MASK_H265)
		family = CODEC_FAMILY_H265;
	else if (video_fmt & VIDEO_FORMAT_MASK_AV1)
		family = CODEC_FAMILY_AV1;

	if (family == -1)
		return -1;

	Vector<String> candidates = _get_candidate_decoders(family);
	bool opened = false;
	String opened_name = "";

	for (int i = 0; i < candidates.size(); i++) {
		if (_try_open_decoder(candidates[i], width, height) == 0) {
			opened_name = candidates[i];
			opened = true;
			break;
		}
	}

	if (!opened) {
		UtilityFunctions::printerr(LOG_PREFIX "No usable decoder found!");
		return -1;
	}

	UtilityFunctions::print(LOG_PREFIX "Initialized FFmpeg Decoder: ", opened_name);
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
}

int MoonlightStreamCore::_handle_ar_init(int audio_cfg) {
	_cleanup_ffmpeg_audio();
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
	if (!codec) {
		UtilityFunctions::printerr(LOG_PREFIX "Opus decoder not found");
		return -1;
	}

	a_codec_ctx = avcodec_alloc_context3(codec);
	if (!a_codec_ctx) return -1;

	// Force standard stereo configuration for Opus as expected by Moonlight/GameStream
	av_channel_layout_default(&a_codec_ctx->ch_layout, 2);
	a_codec_ctx->sample_rate = 48000;
	
	if (avcodec_open2(a_codec_ctx, codec, nullptr) < 0) {
		UtilityFunctions::printerr(LOG_PREFIX "Failed to open Opus codec");
		return -1;
	}

	a_frame = av_frame_alloc();
	a_packet = av_packet_alloc();
	swr_ctx = swr_alloc();
	
	AVChannelLayout stereo; av_channel_layout_default(&stereo, 2);
	
	// Configure Resampler: Input (Opus Decoder) -> Output (Godot Float Stereo)
	av_opt_set_chlayout(swr_ctx, "in_chlayout", &a_codec_ctx->ch_layout, 0);
	av_opt_set_int(swr_ctx, "in_sample_rate", 48000, 0);
	av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", a_codec_ctx->sample_fmt, 0);
	
	av_opt_set_chlayout(swr_ctx, "out_chlayout", &stereo, 0);
	av_opt_set_int(swr_ctx, "out_sample_rate", 48000, 0);
	av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0); // Godot uses Float
	
	if (swr_init(swr_ctx) < 0) {
		UtilityFunctions::printerr(LOG_PREFIX "Failed to init Audio Resampler");
		return -1;
	}
	
	UtilityFunctions::print(LOG_PREFIX "Audio Initialized: Opus 48kHz Stereo");
	return 0;
}

void MoonlightStreamCore::_handle_ar_decode_and_play_sample(char *data, int len) {
	if (!a_codec_ctx || audio_stream.is_null()) return;

	// Prepare packet
	av_packet_unref(a_packet);
	if (av_new_packet(a_packet, len) < 0) return;
	memcpy(a_packet->data, data, len);

	int ret = avcodec_send_packet(a_codec_ctx, a_packet);
	av_packet_unref(a_packet); // Data copied to decoder, unref packet
	if (ret < 0) return;

	while (ret >= 0) {
		ret = avcodec_receive_frame(a_codec_ctx, a_frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
		if (ret < 0) break;

		int max_out_samples = swr_get_out_samples(swr_ctx, a_frame->nb_samples);
		if (max_out_samples > 0) {
			// Use stack buffer for common small frames (~10ms opus = 480 samples)
			// 480 samples * 2 channels * 4 bytes = ~3.8KB. 16KB is plenty.
			uint8_t stack_buf[16384];
			float *out_buf = (float *)stack_buf;
			
			// 2 channels * sizeof(float) = 8 bytes per sample
			bool huge_frame = (size_t)(max_out_samples * 8) > sizeof(stack_buf);
			if (huge_frame) out_buf = (float *)av_malloc(max_out_samples * 8);

			int out_samples = swr_convert(swr_ctx, (uint8_t **)&out_buf, max_out_samples, (const uint8_t **)a_frame->data, a_frame->nb_samples);
			
			if (out_samples > 0) {
				// Push interleaved float stereo samples
				audio_stream->push_audio(out_buf, out_samples * 2);
			}

			if (huge_frame) av_free(out_buf);
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
// Static Callbacks & Bindings
// ============================================================================

void MoonlightStreamCore::_cl_stage_starting(int stage) { UtilityFunctions::print(LOG_PREFIX "Stage Starting: ", LiGetStageName(stage)); }
void MoonlightStreamCore::_cl_connection_started() { UtilityFunctions::print(LOG_PREFIX "Connection Started"); }
void MoonlightStreamCore::_cl_connection_terminated(int error_code) { UtilityFunctions::print(LOG_PREFIX "Connection Terminated: ", error_code); }
void MoonlightStreamCore::_cl_log_message(const char *format, ...) {
	va_list args;
	va_start(args, format);
	char buffer[2048];
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	UtilityFunctions::print(LOG_PREFIX "Log from lib: ", String(buffer));
}
int MoonlightStreamCore::_dr_setup(int fmt, int w, int h, int rate, void *ctx, int flags) { return ((MoonlightStreamCore *)ctx)->_handle_dr_setup(fmt, w, h); }
void MoonlightStreamCore::_dr_cleanup(void) {
	if (singleton_instance)
		singleton_instance->_cleanup_ffmpeg_video();
}
int MoonlightStreamCore::_dr_submit_decode_unit(PDECODE_UNIT du) { return DR_OK; }
int MoonlightStreamCore::_ar_init(int cfg, const POPUS_MULTISTREAM_CONFIGURATION opus, void *ctx, int flags) { return ((MoonlightStreamCore *)ctx)->_handle_ar_init(cfg); }
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