#pragma once

#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_playback_resampled.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/mutex.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/semaphore.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/thread.hpp>
#include <godot_cpp/templates/list.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <atomic>
#include <string>
#include <vector>
#include <cstdarg>
#include <cstdio>

// 包含C API
#include "Limelight.h"

// 前向声明 FFmpeg 结构体
extern "C" {
struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct SwrContext;
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#define LOG_PREFIX "[Moonlight-StreamCore] "

// 编解码器系列
#define CODEC_FAMILY_H264 0
#define CODEC_FAMILY_H265 1
#define CODEC_FAMILY_AV1 2

namespace godot {

class AudioStreamMoonlight;

// ============================================================================
// 自定义音频播放
// ============================================================================
class AudioStreamPlaybackMoonlight : public AudioStreamPlaybackResampled {
	GDCLASS(AudioStreamPlaybackMoonlight, AudioStreamPlaybackResampled);
	friend class AudioStreamMoonlight;

private:
	Ref<AudioStreamMoonlight> base;
	bool active;

public:
	AudioStreamPlaybackMoonlight();
	~AudioStreamPlaybackMoonlight();

	virtual void _start(double p_from_pos = 0.0) override;
	virtual void _stop() override;
	virtual bool _is_playing() const override;
	virtual int32_t _get_loop_count() const override;
	virtual double _get_playback_position() const override;
	virtual void _seek(double p_time) override;
	virtual int32_t _mix_resampled(AudioFrame *dst_buffer, int32_t frame_count) override;
	virtual float _get_stream_sampling_rate() const override;

protected:
	static void _bind_methods();
};

// ============================================================================
// 自定义音频流
// ============================================================================
class AudioStreamMoonlight : public AudioStream {
	GDCLASS(AudioStreamMoonlight, AudioStream);
	friend class AudioStreamPlaybackMoonlight;

private:
	Vector<float> ring_buffer;
	int rb_write_pos = 0;
	int rb_read_pos = 0;
	int rb_capacity = 0;
	int rb_used = 0;
	mutable Ref<Mutex> buffer_mutex;

public:
	int mix_rate;

	AudioStreamMoonlight();

	void push_audio(const float *samples, int count);
	void clear_buffer();
	int read_samples(AudioFrame *dst_buffer, int frame_count);

	virtual Ref<AudioStreamPlayback> _instantiate_playback() const override;
	virtual String _get_stream_name() const override;
	virtual double _get_length() const override { return 0; }

protected:
	static void _bind_methods();
};

// ============================================================================
// moonlight核心节点
// ============================================================================
class MoonlightStreamCore : public Node {
	GDCLASS(MoonlightStreamCore, Node);

public:
	enum VideoCodecConfig {
		CODEC_AUTO = 0,
		CODEC_H264 = 1,
		CODEC_H265 = 2,
		CODEC_AV1 = 3
	};

	MoonlightStreamCore();
	~MoonlightStreamCore();

	void start_play_stream(Dictionary options);
	void stop_play_stream();

	void set_render_target(TextureRect *target);
	void reset_render_target();
	Ref<AudioStream> get_audio_stream();
	void reset_audio_stream(bool free_stream = false);

protected:
	static void _bind_methods();

private:
	// --- 线程与状态 ---
	Ref<Thread> connection_thread; // 运行 LiStartConnection
	Ref<Thread> video_decode_thread; // 运行 FFmpeg 解码（处理器）
	std::atomic<bool> is_streaming;

	// 用于Limelight的全局互斥锁
	static Mutex *lib_global_mutex;

	// --- 配置数据 ---
	std::string ip_storage;
	std::string session_url_storage;
	std::string app_version_storage;
	std::string gfe_version_storage;

	STREAM_CONFIGURATION stream_config;
	SERVER_INFORMATION server_info;

	VideoCodecConfig selected_codec_config;
	bool disable_hw_decoding;

	// 回调
	CONNECTION_LISTENER_CALLBACKS cl_callbacks;
	DECODER_RENDERER_CALLBACKS dr_callbacks;
	AUDIO_RENDERER_CALLBACKS ar_callbacks;

	// --- 视频渲染状态 ---
	TextureRect *display_rect = nullptr;
	Ref<ImageTexture> display_texture;
	Ref<Image> last_decoded_image;
	Ref<Mutex> texture_mutex;
	bool new_frame_available;

	// --- 数据包队列（Pull 与 Decode 之间的缓冲区） ---
	List<AVPacket *> packet_queue;
	Ref<Mutex> queue_mutex;
	Ref<Semaphore> decode_sem;
	Ref<Mutex> codec_mutex;

	// --- 音频状态 ---
	Ref<AudioStreamMoonlight> audio_stream;

	// --- FFmpeg 视频上下文 ---
	const AVCodec *v_codec = nullptr;
	AVCodecContext *v_codec_ctx = nullptr;
	AVFrame *v_frame = nullptr;
	AVFrame *sw_frame = nullptr; // 用于硬件下载的中间帧
	SwsContext *sws_ctx = nullptr;
	int video_width = 0;
	int video_height = 0;
	int video_format = 0;

	// 硬件加速
	AVBufferRef *hw_device_ctx = nullptr;
	AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;

	// 优化：使用可重用缓冲区存储解码数据以避免每帧重新分配
	PackedByteArray decode_buffer;

	// --- FFmpeg 音频上下文 ---
	AVCodecContext *a_codec_ctx = nullptr;
	AVFrame *a_frame = nullptr;
	AVPacket *a_packet = nullptr;
	SwrContext *swr_ctx = nullptr;

	// --- 内部方法 ---
	int _probe_video_format(VideoCodecConfig preference);
	Vector<String> _get_candidate_decoders(int codec_family);

	// 帮助获取特定平台的硬件优先级
	Vector<AVHWDeviceType> _get_supported_hw_devices();
	int _try_open_decoder(const String &codec_name, int width, int height, AVHWDeviceType hw_type);

	void _cleanup_ffmpeg_video();
	void _cleanup_ffmpeg_audio();
	String _get_error_string(int error_code);
	void _apply_sws_colorspace(struct SwsContext *ctx, AVFrame *frame);
	AVColorSpace _resolve_frame_colorspace(AVFrame *frame) const;

	// limelight回调静态封装器
	static void _cl_stage_starting(int stage);
	static void _cl_connection_started();
	static void _cl_connection_terminated(int error_code);
	static void _cl_log_message(const char *format, ...);
	static void _cl_set_hdr_mode(bool enabled);

	static int _dr_setup(int video_format, int width, int height, int redraw_rate, void *context, int dr_flags);
	static void _dr_cleanup(void);
	static int _dr_submit_decode_unit(PDECODE_UNIT decode_unit);

	static int _ar_init(int audio_configuration, const POPUS_MULTISTREAM_CONFIGURATION opus_config, void *context, int ar_flags);
	static void _ar_cleanup(void);
	static void _ar_decode_and_play_sample(char *sample_data, int sample_length);

	// FFmpeg 回调
	static enum AVPixelFormat _get_hw_format_callback(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts);

	// 实例处理器
	int _handle_dr_setup(int video_format, int width, int height);
	int _handle_dr_submit_decode_unit(PDECODE_UNIT decode_unit);
	int _handle_ar_init(int audio_configuration);
	void _handle_ar_decode_and_play_sample(char *sample_data, int sample_length);
	void _handle_set_hdr_mode(bool enabled);

	// 内部更新方法
	void _update_display_texture();

	// 线程循环
	void _thread_func_connection();
	void _thread_func_video_decode();
};

} //namespace godot

VARIANT_ENUM_CAST(godot::MoonlightStreamCore::VideoCodecConfig);