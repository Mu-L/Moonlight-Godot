#include "stream_core.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

// FFmpeg 包括
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

// ============================================================================
// AudioStreamPlaybackMoonlight 实现
// ============================================================================

AudioStreamPlaybackMoonlight::AudioStreamPlaybackMoonlight() : active(false) {}

AudioStreamPlaybackMoonlight::~AudioStreamPlaybackMoonlight() {}

void AudioStreamPlaybackMoonlight::_bind_methods() {
	// 即使没有方法需要绑定，GDCLASS 也需要这个函数存在
}

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

void AudioStreamPlaybackMoonlight::_seek(double p_time) {
	// 实时流，不支持寻址
}

int32_t AudioStreamPlaybackMoonlight::_mix_resampled(AudioFrame *p_buffer, int32_t p_frames) {
	if (!active || base.is_null()) {
		return 0;
	}

	// 修复：使用 -> 访问 Ref<Mutex>
	base->buffer_mutex->lock();
	int available = base->audio_buffer.size();
	int to_read = MIN(available, p_frames * 2); // 2 个声道（立体声）

	// 用可用数据填充缓冲区
	int i = 0;
	for (; i < to_read / 2; i++) {
		float l = base->audio_buffer.front()->get();
		base->audio_buffer.pop_front();
		float r = base->audio_buffer.front()->get();
		base->audio_buffer.pop_front();
		// 修复 AudioFrame 构造错误，直接赋值
		p_buffer[i].left = l;
		p_buffer[i].right = r;
	}
	base->buffer_mutex->unlock();

	// 如果缓冲区有下溢，剩余部分用静音填充
	for (; i < p_frames; i++) {
		p_buffer[i].left = 0.0f;
		p_buffer[i].right = 0.0f;
	}

	return p_frames;
}

float AudioStreamPlaybackMoonlight::_get_stream_sampling_rate() const {
	if (base.is_valid()) {
		return (float)base->mix_rate;
	}
	return 48000.0f;
}

// ============================================================================
// AudioStreamMoonlight 实现
// ============================================================================

AudioStreamMoonlight::AudioStreamMoonlight() : mix_rate(48000), channels(2) {
	buffer_mutex.instantiate(); // 修复：实例化 Ref<Mutex>
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
	buffer_mutex->lock(); // 修复：使用 ->
	// 简单环形缓冲区保护
	if (audio_buffer.size() > 48000 * 2 * 0.5) {
		audio_buffer.clear();
	}
	for (int i = 0; i < count; i++) {
		audio_buffer.push_back(samples[i]);
	}
	buffer_mutex->unlock(); // 修复：使用 ->
}

void AudioStreamMoonlight::clear_buffer() {
	buffer_mutex->lock(); // 修复：使用 ->
	audio_buffer.clear();
	buffer_mutex->unlock(); // 修复：使用 ->
}

void AudioStreamMoonlight::_bind_methods() {
	// GDExtension 中的 AudioStream 实现通常不需要手动绑定 _instantiate_playback 等虚函数，
	// 只要 override 正确即可。但可以绑定一些自定义工具函数。
}

// ============================================================================
// StreamCore 实现
// ============================================================================

MoonlightStreamCore::MoonlightStreamCore() {
	singleton_instance = this;
	is_streaming = false;
	new_frame_available = false;

	video_mutex.instantiate(); // 修复：实例化 Ref<Mutex>

	// 初始化结构体
	LiInitializeStreamConfiguration(&stream_config);
	LiInitializeServerInformation(&server_info);
	LiInitializeConnectionCallbacks(&cl_callbacks);
	LiInitializeVideoCallbacks(&dr_callbacks);
	LiInitializeAudioCallbacks(&ar_callbacks);

	// 设置回调
	cl_callbacks.stageStarting = _cl_stage_starting;
	cl_callbacks.connectionStarted = _cl_connection_started;
	cl_callbacks.connectionTerminated = _cl_connection_terminated;
	cl_callbacks.logMessage = _cl_log_message;

	dr_callbacks.setup = _dr_setup;
	dr_callbacks.cleanup = _dr_cleanup;
	dr_callbacks.submitDecodeUnit = _dr_submit_decode_unit;

	ar_callbacks.init = _ar_init;
	ar_callbacks.cleanup = _ar_cleanup;
	ar_callbacks.decodeAndPlaySample = _ar_decode_and_play_sample;
}

MoonlightStreamCore::~MoonlightStreamCore() {
	stop_play_stream();
	if (singleton_instance == this) {
		singleton_instance = nullptr;
	}
	_cleanup_ffmpeg_video();
	_cleanup_ffmpeg_audio();
}

void MoonlightStreamCore::start_play_stream(Dictionary options) {
	if (is_streaming) {
		UtilityFunctions::printerr(LOG_PREFIX "Stream already running");
		return;
	}

	// 1. 将选项解析为配置
	stream_config.width = options.get("width", 1280);
	stream_config.height = options.get("height", 720);
	stream_config.fps = options.get("fps", 60);
	stream_config.bitrate = options.get("bitrate", 10000);
	stream_config.packetSize = options.get("packet_size", 1024);
	stream_config.streamingRemotely = STREAM_CFG_AUTO;
	stream_config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
	stream_config.supportedVideoFormats = VIDEO_FORMAT_H264 | VIDEO_FORMAT_H265;

	// AES 密钥
	String rikey = options.get("rikey", "");
	if (!rikey.is_empty()) {
		PackedByteArray key_bytes = rikey.hex_decode();
		if (key_bytes.size() >= 16) {
			memcpy(stream_config.remoteInputAesKey, key_bytes.ptr(), 16);
		}
		memset(stream_config.remoteInputAesIv, 0, 16);
	}

	// 服务器信息
	String ip = options.get("ip", "");
	String session_url = options.get("session_url", "");
	String app_version = options.get("app_version", "0.0.0.0"); // 默认值防止空指针崩溃
	String gfe_version = options.get("gfe_version", "");

	// 更新成员变量存储
	ip_storage = ip.utf8().get_data();
	session_url_storage = session_url.utf8().get_data();
	app_version_storage = app_version.utf8().get_data();
	gfe_version_storage = gfe_version.utf8().get_data();

	// 赋值给 C 结构体
	server_info.address = ip_storage.c_str();
	server_info.rtspSessionUrl = session_url_storage.c_str();
	server_info.serverInfoAppVersion = app_version_storage.c_str();
	server_info.serverInfoGfeVersion = gfe_version_storage.c_str();

	// 设置 ServerCodecModeSupport，如果未提供（或为0），则默认为 H.264 以避免断言失败
	server_info.serverCodecModeSupport = options.get("server_codec_mode_support", 0);
	if (server_info.serverCodecModeSupport == 0) {
		UtilityFunctions::print(LOG_PREFIX "ServerCodecModeSupport not provided or 0, defaulting to H.264 (1)");
		server_info.serverCodecModeSupport = 1; // SCM_H264
	}

	is_streaming = true;

	// 2. 开始线程
	connection_thread.instantiate();
	connection_thread->start(callable_mp(this, &MoonlightStreamCore::_thread_func_start_connection));
}

void MoonlightStreamCore::_thread_func_start_connection() {
	UtilityFunctions::print(LOG_PREFIX "Starting connection thread...");
	int ret = LiStartConnection(&server_info, &stream_config, &cl_callbacks, &dr_callbacks, &ar_callbacks, this, 0, this, 0);
	if (ret != 0) {
		UtilityFunctions::printerr(LOG_PREFIX "LiStartConnection failed: ", ret);
		call_deferred("stop_play_stream");
	}
	is_streaming = false;
}

void MoonlightStreamCore::stop_play_stream() {
	if (is_streaming) {
		UtilityFunctions::print(LOG_PREFIX "Stopping connection...");
		LiStopConnection();
		if (connection_thread.is_valid() && connection_thread->is_alive()) {
			connection_thread->wait_to_finish();
		}
		is_streaming = false;
	}
}

void MoonlightStreamCore::set_render_target(TextureRect *target) {
	display_rect = target;
	if (display_rect) {
		// 确保存在一个 texture 供更新
		if (display_texture.is_null()) {
			display_texture.instantiate();
			// 创建初始黑色图像
			Ref<Image> img = Image::create(stream_config.width > 0 ? stream_config.width : 1280,
					stream_config.height > 0 ? stream_config.height : 720,
					false, Image::FORMAT_RGBA8);
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

void MoonlightStreamCore::reset_audio_stream() {
	if (audio_stream.is_valid()) {
		audio_stream->clear_buffer();
		audio_stream.unref();
	}
}

void MoonlightStreamCore::_process(double delta) {
	// 在主线程上更新视频纹理
	if (new_frame_available) {
		video_mutex->lock(); // 修复：使用 ->
		if (last_decoded_image.is_valid() && display_texture.is_valid()) {
			display_texture->update(last_decoded_image);
		}
		new_frame_available = false;
		video_mutex->unlock(); // 修复：使用 ->
	}
}

// ============================================================================
// Callbacks
// ============================================================================

void MoonlightStreamCore::_cl_stage_starting(int stage) {
	UtilityFunctions::print(LOG_PREFIX "Stage starting: ", stage);
}

void MoonlightStreamCore::_cl_connection_started() {
	UtilityFunctions::print(LOG_PREFIX "Connection Started");
}

void MoonlightStreamCore::_cl_connection_terminated(int error_code) {
	UtilityFunctions::print(LOG_PREFIX "Connection Terminated: ", error_code);
	if (singleton_instance) {
		singleton_instance->call_deferred("stop_play_stream");
	}
}

void MoonlightStreamCore::_cl_log_message(const char *format, ...) {
	// 简单转发，实际使用中可能需要格式化处理
	// UtilityFunctions::print(LOG_PREFIX "Log: ", String(format));
}

// --- Video Callbacks ---

int MoonlightStreamCore::_dr_setup(int video_format, int width, int height, int redraw_rate, void *context, int dr_flags) {
	return ((MoonlightStreamCore *)context)->_handle_dr_setup(video_format, width, height);
}

void MoonlightStreamCore::_dr_cleanup(void) {
	if (singleton_instance)
		singleton_instance->_cleanup_ffmpeg_video();
}

int MoonlightStreamCore::_dr_submit_decode_unit(PDECODE_UNIT decode_unit) {
	if (singleton_instance)
		return singleton_instance->_handle_dr_submit_decode_unit(decode_unit);
	return -1;
}

// --- Audio Callbacks ---

int MoonlightStreamCore::_ar_init(int audio_configuration, const POPUS_MULTISTREAM_CONFIGURATION opus_config, void *context, int ar_flags) {
	return ((MoonlightStreamCore *)context)->_handle_ar_init(audio_configuration);
}

void MoonlightStreamCore::_ar_cleanup(void) {
	if (singleton_instance)
		singleton_instance->_cleanup_ffmpeg_audio();
}

void MoonlightStreamCore::_ar_decode_and_play_sample(char *sample_data, int sample_length) {
	if (singleton_instance)
		singleton_instance->_handle_ar_decode_and_play_sample(sample_data, sample_length);
}

// ============================================================================
// FFmpeg & Logic Internals
// ============================================================================

void MoonlightStreamCore::_cleanup_ffmpeg_video() {
	if (sws_ctx) {
		sws_freeContext(sws_ctx);
		sws_ctx = nullptr;
	}
	if (v_frame) {
		av_frame_free(&v_frame);
		v_frame = nullptr;
	}
	if (v_packet) {
		av_packet_free(&v_packet);
		v_packet = nullptr;
	}
	if (v_codec_ctx) {
		avcodec_free_context(&v_codec_ctx);
		v_codec_ctx = nullptr;
	}
}

void MoonlightStreamCore::_cleanup_ffmpeg_audio() {
	if (swr_ctx) {
		swr_free(&swr_ctx);
		swr_ctx = nullptr;
	}
	if (a_frame) {
		av_frame_free(&a_frame);
		a_frame = nullptr;
	}
	if (a_packet) {
		av_packet_free(&a_packet);
		a_packet = nullptr;
	}
	if (a_codec_ctx) {
		avcodec_free_context(&a_codec_ctx);
		a_codec_ctx = nullptr;
	}
}

int MoonlightStreamCore::_handle_dr_setup(int fmt, int width, int height) {
	_cleanup_ffmpeg_video();
	UtilityFunctions::print(LOG_PREFIX "Initializing Video Decoder. Format: ", fmt, " W: ", width, " H: ", height);

	const AVCodec *codec = nullptr;
	enum AVCodecID codec_id = AV_CODEC_ID_NONE;

	if (fmt & VIDEO_FORMAT_MASK_H264)
		codec_id = AV_CODEC_ID_H264;
	else if (fmt & VIDEO_FORMAT_MASK_H265)
		codec_id = AV_CODEC_ID_HEVC;
	else if (fmt & VIDEO_FORMAT_MASK_AV1)
		codec_id = AV_CODEC_ID_AV1;

	// 首先尝试查找默认解码器
	if (codec_id != AV_CODEC_ID_NONE) {
		codec = avcodec_find_decoder(codec_id);
	}

	if (!codec) {
		UtilityFunctions::printerr(LOG_PREFIX "Codec not found for format: ", fmt);
		return -1;
	}

	UtilityFunctions::print(LOG_PREFIX "Selected Video Codec: ", codec->name);

	v_codec_ctx = avcodec_alloc_context3(codec);
	v_codec_ctx->width = width;
	v_codec_ctx->height = height;

	// 多线程设置
	v_codec_ctx->thread_count = 0; // 让 ffmpeg 决定 (通常是 cpu count)
	v_codec_ctx->thread_type = FF_THREAD_SLICE | FF_THREAD_FRAME;

	if (avcodec_open2(v_codec_ctx, codec, nullptr) < 0) {
		UtilityFunctions::printerr(LOG_PREFIX "Failed to open video codec");
		return -1;
	}

	v_frame = av_frame_alloc();
	v_packet = av_packet_alloc();

	video_width = width;
	video_height = height;
	video_format = fmt;

	return DR_OK;
}

int MoonlightStreamCore::_handle_dr_submit_decode_unit(PDECODE_UNIT du) {
	if (!v_codec_ctx)
		return DR_NEED_IDR;

	// 从缓冲链构建 AVPacket
	int total_len = du->fullLength;
	if (av_new_packet(v_packet, total_len) < 0)
		return DR_NEED_IDR;

	int offset = 0;
	PLENTRY entry = du->bufferList;
	while (entry != nullptr) {
		memcpy(v_packet->data + offset, entry->data, entry->length);
		offset += entry->length;
		entry = entry->next;
	}

	// 发送到解码器
	int ret = avcodec_send_packet(v_codec_ctx, v_packet);
	av_packet_unref(v_packet);

	if (ret < 0) {
		return DR_NEED_IDR;
	}

	while (ret >= 0) {
		ret = avcodec_receive_frame(v_codec_ctx, v_frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			break;
		} else if (ret < 0) {
			return DR_NEED_IDR;
		}

		// 帧解码成功
		// 延迟初始化或尺寸变更时重新初始化 sws_ctx
		if (!sws_ctx || video_width != v_codec_ctx->width || video_height != v_codec_ctx->height) {
			if (sws_ctx)
				sws_freeContext(sws_ctx);
			sws_ctx = sws_getContext(v_codec_ctx->width, v_codec_ctx->height, v_codec_ctx->pix_fmt,
					v_codec_ctx->width, v_codec_ctx->height, AV_PIX_FMT_RGBA,
					SWS_BILINEAR, nullptr, nullptr, nullptr);
			video_width = v_codec_ctx->width;
			video_height = v_codec_ctx->height;
		}

		// 创建图像数据
		// 优化：这仍然是一个高开销操作，理想情况下应复用缓冲区
		PackedByteArray img_data;
		img_data.resize(video_width * video_height * 4);

		uint8_t *dest[4] = { img_data.ptrw(), nullptr, nullptr, nullptr };
		int dest_linesize[4] = { video_width * 4, 0, 0, 0 };

		sws_scale(sws_ctx, v_frame->data, v_frame->linesize, 0, v_codec_ctx->height, dest, dest_linesize);

		video_mutex->lock(); // 修复：使用 ->
		last_decoded_image = Image::create_from_data(video_width, video_height, false, Image::FORMAT_RGBA8, img_data);
		new_frame_available = true;
		video_mutex->unlock(); // 修复：使用 ->
	}

	return DR_OK;
}

int MoonlightStreamCore::_handle_ar_init(int audio_cfg) {
	_cleanup_ffmpeg_audio();
	UtilityFunctions::print(LOG_PREFIX "Initializing Audio Decoder...");

	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
	if (!codec) {
		UtilityFunctions::printerr(LOG_PREFIX "Audio Codec (OPUS) not found!");
		return -1;
	}

	a_codec_ctx = avcodec_alloc_context3(codec);

	// 使用新 API 设置通道布局 (FFmpeg 5.1+)
	av_channel_layout_default(&a_codec_ctx->ch_layout, 2);
	a_codec_ctx->sample_rate = 48000;

	if (avcodec_open2(a_codec_ctx, codec, nullptr) < 0) {
		UtilityFunctions::printerr(LOG_PREFIX "Failed to open audio codec");
		return -1;
	}

	a_frame = av_frame_alloc();
	a_packet = av_packet_alloc();

	// 初始化重采样器 (swr_alloc_set_opts 已废弃，使用 av_opt 设置)
	swr_ctx = swr_alloc();
	if (!swr_ctx)
		return -1;

	AVChannelLayout stereo_layout;
	av_channel_layout_default(&stereo_layout, 2);

	// 配置 Resampler
	// 输入：Opus 输出通常为 48k, Stereo, Sample Fmt 取决于解码器 (通常 FLT 或 S16)
	// 输出：Godot 需要 48k FLT (交错)
	av_opt_set_chlayout(swr_ctx, "in_chlayout", &a_codec_ctx->ch_layout, 0);
	av_opt_set_int(swr_ctx, "in_sample_rate", a_codec_ctx->sample_rate, 0);
	av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", a_codec_ctx->sample_fmt, 0);

	av_opt_set_chlayout(swr_ctx, "out_chlayout", &stereo_layout, 0);
	av_opt_set_int(swr_ctx, "out_sample_rate", 48000, 0);
	av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

	if (swr_init(swr_ctx) < 0) {
		UtilityFunctions::printerr(LOG_PREFIX "Failed to initialize audio resampler");
		return -1;
	}

	return 0;
}

void MoonlightStreamCore::_handle_ar_decode_and_play_sample(char *data, int len) {
	if (!a_codec_ctx || !audio_stream.is_valid())
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
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		else if (ret < 0)
			break;

		// 重新采样
		int max_samples = swr_get_out_samples(swr_ctx, a_frame->nb_samples);
		if (max_samples <= 0)
			continue;

		float *buffer_ptr = (float *)av_malloc(max_samples * 2 * sizeof(float)); // Stereo

		int out_samples = swr_convert(swr_ctx, (uint8_t **)&buffer_ptr, max_samples,
				(const uint8_t **)a_frame->data, a_frame->nb_samples);

		if (out_samples > 0) {
			audio_stream->push_audio(buffer_ptr, out_samples * 2);
		}

		av_free(buffer_ptr);
	}
}

void MoonlightStreamCore::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start_play_stream", "options"), &MoonlightStreamCore::start_play_stream);
	ClassDB::bind_method(D_METHOD("stop_play_stream"), &MoonlightStreamCore::stop_play_stream);
	ClassDB::bind_method(D_METHOD("set_render_target", "texture_rect"), &MoonlightStreamCore::set_render_target);
	ClassDB::bind_method(D_METHOD("reset_render_target"), &MoonlightStreamCore::reset_render_target);
	ClassDB::bind_method(D_METHOD("get_audio_stream"), &MoonlightStreamCore::get_audio_stream);
	ClassDB::bind_method(D_METHOD("reset_audio_stream"), &MoonlightStreamCore::reset_audio_stream);
}
