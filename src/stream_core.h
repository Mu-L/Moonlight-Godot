#pragma once

// Godot 头文件
#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_playback_resampled.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/mutex.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/semaphore.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/thread.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/list.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

// C++ 标准库
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

// Limelight SDK 头文件
#include "Limelight.h"
// Stream configuration resources
#include "stream_core_struct.h"

// FFmpeg 头文件
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

// YUV 转 RGB 的 shader 资源
#include "yuvtorgb_shader.h"

// Android 平台相关头文件
#ifdef __ANDROID__
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <jni.h>
// Provide JNI helper declaration for other translation units
JNIEnv *GetJNIEnv();
#endif

// 日志前缀
#define LOG_PREFIX "[Moonlight-StreamCore] "

// 编解码器系列
#define CODEC_FAMILY_H264 0
#define CODEC_FAMILY_H265 1
#define CODEC_FAMILY_AV1 2

// Forward declarations for miniaudio types used by the native bypass callback.
#if !defined(NO_MINIAUDIO)
struct ma_device;
typedef unsigned int ma_uint32;
#else
// When miniaudio is disabled, provide opaque typedefs to allow compilation.
typedef struct ma_device ma_device;
typedef unsigned int ma_uint32;
#endif

namespace godot {

class MoonlightStreamCore;
extern std::vector<MoonlightStreamCore *> active_instances;
extern std::mutex instances_mutex;

class MoonlightStreamConfigurationResource;
class MoonlightAdditionalStreamOptions;
class ComputerManager;

// 自定义音频播放
class AudioStreamMoonlight;
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

// 自定义音频流
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
	int channel_count = 2; // 每实例的声道数（1=单声道，2=立体声）
	AudioStreamMoonlight();

	// 可用于更改此 AudioStreamMoonlight 期望的声道数（仅影响内部缓冲/读取行为）
	void set_channel_count(int c) { channel_count = c; }
	void push_audio(const float *samples, int count);
	void clear_buffer();
	int read_samples(AudioFrame *dst_buffer, int frame_count);
	virtual Ref<AudioStreamPlayback> _instantiate_playback() const override;
	virtual String _get_stream_name() const override;
	virtual double _get_length() const override { return 0; }

protected:
	static void _bind_methods();
};

// moonlight核心节点
class MoonlightStreamCore : public Node {
	GDCLASS(MoonlightStreamCore, Node);

public:
	enum VideoCodecConfig {
		CODEC_AUTO = 0,
		CODEC_H264 = 1,
		CODEC_H265 = 2,
		CODEC_AV1 = 3
	};

	// Limelight-aligned enums (C++ wrappers for plugin clarity)
	enum StreamCfg {
		StreamCfgLocal = STREAM_CFG_LOCAL,
		StreamCfgRemote = STREAM_CFG_REMOTE,
		StreamCfgAuto = STREAM_CFG_AUTO
	};

	enum ColorSpace {
		ColorSpaceRec601 = COLORSPACE_REC_601,
		ColorSpaceRec709 = COLORSPACE_REC_709,
		ColorSpaceRec2020 = COLORSPACE_REC_2020
	};

	enum ColorRange {
		ColorRangeLimited = COLOR_RANGE_LIMITED,
		ColorRangeFull = COLOR_RANGE_FULL
	};

	enum EncryptionFlags {
		EncryptNone = ENCFLG_NONE,
		EncryptAudio = ENCFLG_AUDIO,
		EncryptVideo = ENCFLG_VIDEO,
		EncryptAll = ENCFLG_ALL
	};

	MoonlightStreamCore();
	~MoonlightStreamCore();

	void set_config_manager(Object *cm);

	void start_play_stream(int host_id, int app_id, Ref<MoonlightStreamConfigurationResource> stream_config_res, Ref<MoonlightAdditionalStreamOptions> additional_options = Ref<MoonlightAdditionalStreamOptions>());
	void stop_play_stream();
	void set_render_target(TextureRect *target);
	void reset_render_target();
	Ref<AudioStream> get_audio_stream();
	Array get_audio_streams();
	void reset_audio_stream(bool free_stream = false);

	// 原生旁路音频控制：启动/停止直接输出到系统默认设备（可选）
	bool start_native_audio_bypass();
	void stop_native_audio_bypass();
	bool is_native_audio_bypass_running() const;

	// Pause/resume native bypass without stopping device (silence while paused)
	void pause_native_audio_bypass();
	void resume_native_audio_bypass();
	bool is_native_audio_bypass_paused() const;

	/* 输入相关封装：将 Limelight 的输入 API 暴露给 Godot */
	int send_mouse_move_event(short delta_x, short delta_y);
	int send_mouse_position_event(short x, short y, int reference_width, int reference_height);
	int send_mouse_move_as_mouse_position_event(short delta_x, short delta_y, int reference_width, int reference_height);
	int send_touch_event(int event_type, int pointer_id, float x, float y, float pressure_or_distance,
			float contact_area_major, float contact_area_minor, int rotation);
	int send_pen_event(int event_type, int tool_type, int pen_buttons,
			float x, float y, float pressure_or_distance,
			float contact_area_major, float contact_area_minor,
			int rotation, int tilt);
	int send_mouse_button_event(int action, int button);
	int send_keyboard_event(short key_code, int key_action, int modifiers);
	int send_keyboard_event2(short key_code, int key_action, int modifiers, int flags);
	int send_utf8_text_event(const String &text);
	int send_controller_event(int button_flags, int left_trigger, int right_trigger,
			short left_stick_x, short left_stick_y, short right_stick_x, short right_stick_y);
	int send_multi_controller_event(int controller_number, int active_gamepad_mask,
			int button_flags, int left_trigger, int right_trigger,
			short left_stick_x, short left_stick_y, short right_stick_x, short right_stick_y);
	int send_controller_arrival_event(int controller_number, int active_gamepad_mask, int type,
			uint32_t supported_button_flags, int capabilities);
	int send_controller_touch_event(int controller_number, int event_type, int pointer_id, float x, float y, float pressure);
	int send_controller_motion_event(int controller_number, int motion_type, float x, float y, float z);
	int send_controller_battery_event(int controller_number, int battery_state, int battery_percentage);
	int send_scroll_event(int scroll_clicks);
	int send_high_res_scroll_event(short scroll_amount);
	int send_hscroll_event(int scroll_clicks);
	int send_high_res_hscroll_event(short scroll_amount);
	uint32_t get_host_feature_flags();

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
	bool disable_hw_decoding; // 为 true 时将跳过 mediacodec 与硬件设备，仅使用纯软件解码
	// 回调
	CONNECTION_LISTENER_CALLBACKS cl_callbacks;
	DECODER_RENDERER_CALLBACKS dr_callbacks;
	AUDIO_RENDERER_CALLBACKS ar_callbacks;
	// --- 视频渲染状态 ---
	TextureRect *display_rect = nullptr;
	Ref<ImageTexture> display_texture; // Used for legacy fallback (RGBA) or placeholder
	// RenderingDevice Acceleration
	RenderingDevice *rd = nullptr;
	RID rd_texture_rid[3]; // Low-level RD RIDs
	RID rs_texture_rid[3]; // High-level RS RIDs (linked to RD)
	Ref<Texture2DRD> rd_texture_wrappers[3];
	PackedByteArray rd_texture_buffers[3];
	std::atomic<bool> pending_gpu_update;
	// Shader Pipeline Resources
	bool use_shader_conversion = false;
	Ref<ShaderMaterial> shader_material;
	Ref<Shader> yuv_shader;
	// Internal textures for planes (Y, U, V or Y, UV)
	// We use max 3 planes (Y, U, V). For NV12, we use 0 (Y) and 1 (UV).
	Ref<Image> plane_images[3];
	Ref<ImageTexture> plane_textures[3];
	PackedByteArray plane_buffers[3]; // Reusable intermediate buffers
	Ref<Image> last_decoded_image;
	Ref<Mutex> texture_mutex;
	bool new_frame_available;
	// Throttling
	uint64_t last_idr_time = 0;
	// --- 数据包队列（Pull 与 Decode 之间的缓冲区） ---
	List<AVPacket *> packet_queue;
	Ref<Mutex> queue_mutex;
	Ref<Semaphore> decode_sem;
	Ref<Mutex> codec_mutex;

	// --- Internal helpers for unified start flow ---
	Object *config_manager = nullptr;
	ComputerManager *internal_cm = nullptr;
	Ref<MoonlightStreamConfigurationResource> pending_cfg;
	Ref<MoonlightAdditionalStreamOptions> pending_add_opts;

	void _on_establish_stream_completed(Dictionary response);
	// --- Debug Options ---
	bool enable_idr_logs = false;
	// Verbose toggles (can be overridden via options dict in start_play_stream)
	bool verbose_decoders =
#if defined(NDEBUG)
			false
#else
			true
#endif
			;
	bool verbose_requests =
#if defined(NDEBUG)
			false
#else
			true
#endif
			;
	bool verbose_limelight =
#if defined(NDEBUG)
			false
#else
			true
#endif
			;
	bool verbose_plugin =
#if defined(NDEBUG)
			false
#else
			true
#endif
			;
	// --- Decoder State ---
	bool is_hw_decode_active = false;
	// --- 音频状态 ---
	Ref<AudioStreamMoonlight> audio_stream;

	// 多声道支持：按 Limelight 约定将每个输出声道单独分离为一个 `AudioStreamMoonlight` 实例。
	// 声道顺序（索引->声道）参考 Limelight 文档/头文件：
	// 0 - 左前, 1 - 右前, 2 - 中置, 3 - 低音炮, 4 - 左后, 5 - 右后, 6 - 左侧, 7 - 右侧
	// 注意：最多支持 AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT 个声道（通常为 8）。
	// 性能说明：调用 `get_audio_streams()` 会为每个声道创建/保留单独的 `AudioStreamMoonlight` 实例，
	// 这可能增加解码/内存/调度开销；在不需要每通道单独处理时请使用原有的 `get_audio_stream()`。
	Vector<Ref<AudioStreamMoonlight>> audio_streams;

	// --- 原生旁路音频（miniaudio）支持 ---
	// 当启用时，音频流会写入一个独立的环形缓冲并由 miniaudio 直接输出到默认设备，
	// 以尽可能降低延迟。此功能为可选且与 Godot 的音频流并存。
	bool native_audio_bypass_enabled = false;
	int native_audio_channel_count = 2;
	void *native_ma_context = nullptr; // opaque pointer to ma_context
	void *native_ma_device = nullptr; // opaque pointer to ma_device
	Vector<float> native_audio_ring; // interleaved float samples
	int native_rb_write_pos = 0;
	int native_rb_read_pos = 0;
	int native_rb_capacity = 0;
	int native_rb_used = 0;
	mutable Ref<Mutex> native_audio_mutex;

	// Pause state for native bypass (true -> silently output zeros until resumed)
	bool native_audio_paused = false;

	// --- FFmpeg 视频上下文 ---
	const AVCodec *v_codec = nullptr;
	AVCodecContext *v_codec_ctx = nullptr;
	AVFrame *v_frame = nullptr;
	AVFrame *sw_frame = nullptr; // 用于硬件下载的中间帧
	int video_width = 0;
	int video_height = 0;
	int video_format = 0;
	// 硬件加速
	AVBufferRef *hw_device_ctx = nullptr;
	AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
	// --- FFmpeg 音频上下文 ---
	AVCodecContext *a_codec_ctx = nullptr;
	AVFrame *a_frame = nullptr;
	AVPacket *a_packet = nullptr;
	SwrContext *swr_ctx = nullptr;
	SwrContext *swr_ctx_multi = nullptr; // for producing multichannel float output
	// --- 内部方法 ---
	int _probe_video_format(VideoCodecConfig preference);
	Vector<String> _get_candidate_decoders(int codec_family);
	void _request_idr_frame(const String &reason);
	// 帮助获取特定平台的硬件优先级
	Vector<AVHWDeviceType> _get_supported_hw_devices();
	int _try_open_decoder(const String &codec_name, int width, int height, AVHWDeviceType hw_type);
	void _cleanup_ffmpeg_video();
	void _cleanup_ffmpeg_audio();
	String _get_error_string(int error_code);
	AVColorSpace _resolve_frame_colorspace(AVFrame *frame) const;
	// limelight回调静态封装器
	static void _cl_stage_starting(int stage);
	static void _cl_connection_started();
	static void _cl_connection_terminated(int error_code);
	static void _cl_log_message(const char *format, ...);
	static void _cl_set_hdr_mode(bool enabled);

	static void _mab_device_callback(::ma_device *pDevice, void *pOutput, const void *pInput, ::ma_uint32 frameCount);
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
	int _handle_ar_init(int audio_configuration, const POPUS_MULTISTREAM_CONFIGURATION opus_config);
	void _handle_ar_decode_and_play_sample(char *sample_data, int sample_length);
	int _native_audio_start(int channels);
	void _native_audio_stop();
	bool _native_audio_is_running() const;
	void _handle_set_hdr_mode(bool enabled);
	// 内部更新方法
	void _setup_shader_integration(int width, int height, AVPixelFormat format, AVColorSpace colorspace, AVColorRange color_range, int bit_depth);
	void _update_textures_with_frame(AVFrame *frame);
	// 线程循环
	void _thread_func_connection();
	void _thread_func_video_decode();
	// 渲染辅助
	void _perform_gpu_update();
	void _render_thread_setup_shader(int width, int height, int format, int colorspace, int color_range, int bit_depth);
	void _render_thread_cleanup_resources();
};

} //namespace godot

// Active instances (defined in stream_core_main.cpp)
namespace godot {
extern std::vector<MoonlightStreamCore *> active_instances;
extern std::mutex instances_mutex;
}

VARIANT_ENUM_CAST(godot::MoonlightStreamCore::VideoCodecConfig);
VARIANT_ENUM_CAST(godot::MoonlightStreamCore::StreamCfg);
VARIANT_ENUM_CAST(godot::MoonlightStreamCore::ColorSpace);
VARIANT_ENUM_CAST(godot::MoonlightStreamCore::ColorRange);
VARIANT_ENUM_CAST(godot::MoonlightStreamCore::EncryptionFlags);