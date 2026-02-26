#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

namespace godot {

class MoonlightStreamConfigurationResource : public Resource {
	GDCLASS(MoonlightStreamConfigurationResource, Resource);

protected:
	static void _bind_methods();

private:
	int width = 0;
	int height = 0;
	int fps = 0;
	int bitrate = 0;
	int packet_size = 0;
	int streaming_remotely = 0;
	int audio_configuration = 0;
	int supported_video_formats = 0;
	int client_refresh_rate_x100 = 0;
	int color_space = 0;
	int color_range = 0;
	int encryption_flags = 0;
	PackedByteArray remote_input_aes_key; // expected 16 bytes
	PackedByteArray remote_input_aes_iv; // expected 16 bytes

public:
	MoonlightStreamConfigurationResource() {}

	// setters/getters
	void set_width(int v) { width = v; }
	int get_width() const { return width; }
	void set_height(int v) { height = v; }
	int get_height() const { return height; }
	void set_fps(int v) { fps = v; }
	int get_fps() const { return fps; }
	void set_bitrate(int v) { bitrate = v; }
	int get_bitrate() const { return bitrate; }
	void set_packet_size(int v) { packet_size = v; }
	int get_packet_size() const { return packet_size; }
	void set_streaming_remotely(int v) { streaming_remotely = v; }
	int get_streaming_remotely() const { return streaming_remotely; }
	void set_audio_configuration(int v) { audio_configuration = v; }
	int get_audio_configuration() const { return audio_configuration; }
	void set_supported_video_formats(int v) { supported_video_formats = v; }
	int get_supported_video_formats() const { return supported_video_formats; }
	void set_client_refresh_rate_x100(int v) { client_refresh_rate_x100 = v; }
	int get_client_refresh_rate_x100() const { return client_refresh_rate_x100; }
	void set_color_space(int v) { color_space = v; }
	int get_color_space() const { return color_space; }
	void set_color_range(int v) { color_range = v; }
	int get_color_range() const { return color_range; }
	void set_encryption_flags(int v) { encryption_flags = v; }
	int get_encryption_flags() const { return encryption_flags; }
	void set_remote_input_aes_key(const PackedByteArray &b) { remote_input_aes_key = b; }
	PackedByteArray get_remote_input_aes_key() const { return remote_input_aes_key; }
	void set_remote_input_aes_iv(const PackedByteArray &b) { remote_input_aes_iv = b; }
	PackedByteArray get_remote_input_aes_iv() const { return remote_input_aes_iv; }
};

class MoonlightAdditionalStreamOptions : public Resource {
	GDCLASS(MoonlightAdditionalStreamOptions, Resource);

protected:
	static void _bind_methods();

private:
	bool disable_hw_acceleration = false;
	bool prefer_hw_decoder = false;
	bool verbose = false;
	int video_codec = 1; // default to H.264 (matches existing CODEC_H264 == 1)

public:
	MoonlightAdditionalStreamOptions() {}
	void set_disable_hw_acceleration(bool v) { disable_hw_acceleration = v; }
	bool get_disable_hw_acceleration() const { return disable_hw_acceleration; }
	void set_prefer_hw_decoder(bool v) { prefer_hw_decoder = v; }
	bool get_prefer_hw_decoder() const { return prefer_hw_decoder; }
	void set_verbose(bool v) { verbose = v; }
	bool get_verbose() const { return verbose; }
	void set_video_codec(int v) { video_codec = v; }
	int get_video_codec() const { return video_codec; }
};

} // namespace godot
