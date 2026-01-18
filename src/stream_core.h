#pragma once

#include <godot_cpp/classes/audio_stream.hpp>
#include <godot_cpp/classes/audio_stream_playback_resampled.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/mutex.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/thread.hpp>
#include <godot_cpp/templates/list.hpp>
#include <godot_cpp/templates/vector.hpp>

#include <string>

#include "Limelight.h"

// FFmpeg forward declarations
extern "C" {
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct SwrContext;
}

namespace godot {

class AudioStreamMoonlight;

class AudioStreamPlaybackMoonlight : public AudioStreamPlaybackResampled {
	GDCLASS(AudioStreamPlaybackMoonlight, AudioStreamPlaybackResampled);
	friend class AudioStreamMoonlight;

private:
	Ref<AudioStreamMoonlight> base;
	bool active;

protected:
	static void _bind_methods();

public:
	AudioStreamPlaybackMoonlight();
	~AudioStreamPlaybackMoonlight();

	// 将虚函数移至 public 以解决 godot-cpp register_virtuals 的访问权限问题 (C2248)
	virtual void _start(double p_from_pos = 0.0) override;
	virtual void _stop() override;
	virtual bool _is_playing() const override;
	virtual int32_t _get_loop_count() const override;
	virtual double _get_playback_position() const override;
	virtual void _seek(double p_time) override;

	virtual int32_t _mix_resampled(AudioFrame *dst_buffer, int32_t frame_count) override;
	virtual float _get_stream_sampling_rate() const override;
};

class AudioStreamMoonlight : public AudioStream {
	GDCLASS(AudioStreamMoonlight, AudioStream);
	friend class AudioStreamPlaybackMoonlight;

private:
	List<float> audio_buffer;
	mutable Ref<Mutex> buffer_mutex; // 修复：使用 Ref<Mutex> 而非直接成员对象
	int mix_rate;
	int channels;

public:
	AudioStreamMoonlight();

	void push_audio(const float *samples, int count);
	void clear_buffer();

	virtual Ref<AudioStreamPlayback> _instantiate_playback() const override;
	virtual String _get_stream_name() const override;
	virtual double _get_length() const override { return 0; }

protected:
	static void _bind_methods();
};

class MoonlightStreamCore : public Node {
	GDCLASS(MoonlightStreamCore, Node);

private:
	// Connection state
	Ref<Thread> connection_thread;
	bool is_streaming;

	// String storage for C pointers in server_info
	// 使用成员变量存储字符串数据，确保在连接线程运行期间指针有效
	std::string ip_storage;
	std::string session_url_storage;
	std::string app_version_storage;
	std::string gfe_version_storage;

	// Moonlight structs
	STREAM_CONFIGURATION stream_config;
	SERVER_INFORMATION server_info;
	CONNECTION_LISTENER_CALLBACKS cl_callbacks;
	DECODER_RENDERER_CALLBACKS dr_callbacks;
	AUDIO_RENDERER_CALLBACKS ar_callbacks;

	// Video rendering
	TextureRect *display_rect = nullptr; // 修改为 TextureRect
	Ref<ImageTexture> display_texture;
	Ref<Image> last_decoded_image;
	Ref<Mutex> video_mutex; // 修复：使用 Ref<Mutex> 而非直接成员对象
	bool new_frame_available;

	// Audio handling
	Ref<AudioStreamMoonlight> audio_stream;

	// FFmpeg Video Context
	AVCodecContext *v_codec_ctx = nullptr;
	AVFrame *v_frame = nullptr;
	AVPacket *v_packet = nullptr;
	SwsContext *sws_ctx = nullptr;
	int video_width = 0;
	int video_height = 0;
	int video_format = 0;

	// FFmpeg Audio Context
	AVCodecContext *a_codec_ctx = nullptr;
	AVFrame *a_frame = nullptr;
	AVPacket *a_packet = nullptr;
	SwrContext *swr_ctx = nullptr;

	// Callbacks handlers
	static void _cl_stage_starting(int stage);
	static void _cl_connection_started();
	static void _cl_connection_terminated(int error_code);
	static void _cl_log_message(const char *format, ...);

	static int _dr_setup(int video_format, int width, int height, int redraw_rate, void *context, int dr_flags);
	static void _dr_cleanup(void);
	static int _dr_submit_decode_unit(PDECODE_UNIT decode_unit);

	static int _ar_init(int audio_configuration, const POPUS_MULTISTREAM_CONFIGURATION opus_config, void *context, int ar_flags);
	static void _ar_cleanup(void);
	static void _ar_decode_and_play_sample(char *sample_data, int sample_length);

	// Instance implementations
	void _thread_func_start_connection();
	int _handle_dr_setup(int video_format, int width, int height);
	int _handle_dr_submit_decode_unit(PDECODE_UNIT decode_unit);
	int _handle_ar_init(int audio_configuration);
	void _handle_ar_decode_and_play_sample(char *sample_data, int sample_length);

	void _cleanup_ffmpeg_video();
	void _cleanup_ffmpeg_audio();

public:
	MoonlightStreamCore();
	~MoonlightStreamCore();

	void start_play_stream(Dictionary options);
	void stop_play_stream();

	void set_render_target(TextureRect *target); // 参数类型变更
	void reset_render_target();

	Ref<AudioStream> get_audio_stream();
	void reset_audio_stream();

	virtual void _process(double delta) override;

protected:
	static void _bind_methods();
};

} // namespace godot
