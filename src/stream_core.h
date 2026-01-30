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

#include <atomic>
#include <string>
#include <vector>

// Include the C API
#include "Limelight.h"

// Forward declare FFmpeg structs
extern "C" {
struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct SwrContext;
}

namespace godot {

class AudioStreamMoonlight;

// ============================================================================
// Custom Audio Playback
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
// Custom Audio Stream
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
// Moonlight Core Node
// ============================================================================
class MoonlightStreamCore : public Node {
	GDCLASS(MoonlightStreamCore, Node);

private:
	// --- Threading & State ---
	Ref<Thread> connection_thread; // Runs LiStartConnection
	Ref<Thread> video_decode_thread; // Runs FFmpeg Decode (Processor)
	std::atomic<bool> is_streaming;

	// Global mutex for Limelight
	static Mutex *lib_global_mutex;

	// --- Configuration Data ---
	std::string ip_storage;
	std::string session_url_storage;
	std::string app_version_storage;
	std::string gfe_version_storage;

	STREAM_CONFIGURATION stream_config;
	SERVER_INFORMATION server_info;

	enum VideoCodecConfig {
		CODEC_AUTO = 0,
		CODEC_H264 = 1,
		CODEC_H265 = 2,
		CODEC_AV1 = 3
	};
	VideoCodecConfig selected_codec_config;

	// Callbacks
	CONNECTION_LISTENER_CALLBACKS cl_callbacks;
	DECODER_RENDERER_CALLBACKS dr_callbacks;
	AUDIO_RENDERER_CALLBACKS ar_callbacks;

	// --- Video Rendering State ---
	TextureRect *display_rect = nullptr;
	Ref<ImageTexture> display_texture;
	Ref<Image> last_decoded_image;
	Ref<Mutex> texture_mutex;
	bool new_frame_available;

	// --- Packet Queue (Buffer between Pull and Decode) ---
	List<AVPacket *> packet_queue;
	Ref<Mutex> queue_mutex;
	Ref<Semaphore> decode_sem;
	Ref<Mutex> codec_mutex;

	// --- Audio State ---
	Ref<AudioStreamMoonlight> audio_stream;

	// --- FFmpeg Video Context ---
	const AVCodec *v_codec = nullptr;
	AVCodecContext *v_codec_ctx = nullptr;
	AVFrame *v_frame = nullptr;
	SwsContext *sws_ctx = nullptr;
	int video_width = 0;
	int video_height = 0;
	int video_format = 0;

	// Optimization: Reusable buffer for decoded data to avoid reallocation every frame
	PackedByteArray decode_buffer;

	// --- FFmpeg Audio Context ---
	AVCodecContext *a_codec_ctx = nullptr;
	AVFrame *a_frame = nullptr;
	AVPacket *a_packet = nullptr;
	SwrContext *swr_ctx = nullptr;

	// --- Internal Methods ---
	int _probe_video_format(VideoCodecConfig preference);
	Vector<String> _get_candidate_decoders(int codec_family);
	int _try_open_decoder(const String &codec_name, int width, int height);
	void _cleanup_ffmpeg_video();
	void _cleanup_ffmpeg_audio();

	// Limelight Static Wrappers
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

	// Instance Handlers
	int _handle_dr_setup(int video_format, int width, int height);
	int _handle_dr_submit_decode_unit(PDECODE_UNIT decode_unit);
	int _handle_ar_init(int audio_configuration);
	void _handle_ar_decode_and_play_sample(char *sample_data, int sample_length);

	// Internal update method
	void _update_display_texture();

	// Thread Loops
	void _thread_func_connection();
	void _thread_func_video_decode();

public:
	MoonlightStreamCore();
	~MoonlightStreamCore();

	void start_play_stream(Dictionary options);
	void stop_play_stream();

	void set_render_target(TextureRect *target);
	void reset_render_target();
	Ref<AudioStream> get_audio_stream();
	void reset_audio_stream(bool free_stream = false);

	virtual void _process(double delta) override;

protected:
	static void _bind_methods();
};

} // namespace godot