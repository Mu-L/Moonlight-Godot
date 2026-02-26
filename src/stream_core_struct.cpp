#include "stream_core_struct.h"
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

void MoonlightStreamConfigurationResource::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_width", "v"), &MoonlightStreamConfigurationResource::set_width);
	ClassDB::bind_method(D_METHOD("get_width"), &MoonlightStreamConfigurationResource::get_width);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "width"), "set_width", "get_width");

	ClassDB::bind_method(D_METHOD("set_height", "v"), &MoonlightStreamConfigurationResource::set_height);
	ClassDB::bind_method(D_METHOD("get_height"), &MoonlightStreamConfigurationResource::get_height);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "height"), "set_height", "get_height");

	ClassDB::bind_method(D_METHOD("set_fps", "v"), &MoonlightStreamConfigurationResource::set_fps);
	ClassDB::bind_method(D_METHOD("get_fps"), &MoonlightStreamConfigurationResource::get_fps);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "fps"), "set_fps", "get_fps");

	ClassDB::bind_method(D_METHOD("set_bitrate", "v"), &MoonlightStreamConfigurationResource::set_bitrate);
	ClassDB::bind_method(D_METHOD("get_bitrate"), &MoonlightStreamConfigurationResource::get_bitrate);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bitrate"), "set_bitrate", "get_bitrate");

	ClassDB::bind_method(D_METHOD("set_packet_size", "v"), &MoonlightStreamConfigurationResource::set_packet_size);
	ClassDB::bind_method(D_METHOD("get_packet_size"), &MoonlightStreamConfigurationResource::get_packet_size);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "packet_size"), "set_packet_size", "get_packet_size");

	ClassDB::bind_method(D_METHOD("set_streaming_remotely", "v"), &MoonlightStreamConfigurationResource::set_streaming_remotely);
	ClassDB::bind_method(D_METHOD("get_streaming_remotely"), &MoonlightStreamConfigurationResource::get_streaming_remotely);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "streaming_remotely"), "set_streaming_remotely", "get_streaming_remotely");

	ClassDB::bind_method(D_METHOD("set_audio_configuration", "v"), &MoonlightStreamConfigurationResource::set_audio_configuration);
	ClassDB::bind_method(D_METHOD("get_audio_configuration"), &MoonlightStreamConfigurationResource::get_audio_configuration);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "audio_configuration"), "set_audio_configuration", "get_audio_configuration");

	ClassDB::bind_method(D_METHOD("set_supported_video_formats", "v"), &MoonlightStreamConfigurationResource::set_supported_video_formats);
	ClassDB::bind_method(D_METHOD("get_supported_video_formats"), &MoonlightStreamConfigurationResource::get_supported_video_formats);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "supported_video_formats"), "set_supported_video_formats", "get_supported_video_formats");

	ClassDB::bind_method(D_METHOD("set_client_refresh_rate_x100", "v"), &MoonlightStreamConfigurationResource::set_client_refresh_rate_x100);
	ClassDB::bind_method(D_METHOD("get_client_refresh_rate_x100"), &MoonlightStreamConfigurationResource::get_client_refresh_rate_x100);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "client_refresh_rate_x100"), "set_client_refresh_rate_x100", "get_client_refresh_rate_x100");

	ClassDB::bind_method(D_METHOD("set_color_space", "v"), &MoonlightStreamConfigurationResource::set_color_space);
	ClassDB::bind_method(D_METHOD("get_color_space"), &MoonlightStreamConfigurationResource::get_color_space);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "color_space"), "set_color_space", "get_color_space");

	ClassDB::bind_method(D_METHOD("set_color_range", "v"), &MoonlightStreamConfigurationResource::set_color_range);
	ClassDB::bind_method(D_METHOD("get_color_range"), &MoonlightStreamConfigurationResource::get_color_range);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "color_range"), "set_color_range", "get_color_range");

	ClassDB::bind_method(D_METHOD("set_encryption_flags", "v"), &MoonlightStreamConfigurationResource::set_encryption_flags);
	ClassDB::bind_method(D_METHOD("get_encryption_flags"), &MoonlightStreamConfigurationResource::get_encryption_flags);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "encryption_flags"), "set_encryption_flags", "get_encryption_flags");

	ClassDB::bind_method(D_METHOD("set_remote_input_aes_key", "b"), &MoonlightStreamConfigurationResource::set_remote_input_aes_key);
	ClassDB::bind_method(D_METHOD("get_remote_input_aes_key"), &MoonlightStreamConfigurationResource::get_remote_input_aes_key);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "remote_input_aes_key"), "set_remote_input_aes_key", "get_remote_input_aes_key");

	ClassDB::bind_method(D_METHOD("set_remote_input_aes_iv", "b"), &MoonlightStreamConfigurationResource::set_remote_input_aes_iv);
	ClassDB::bind_method(D_METHOD("get_remote_input_aes_iv"), &MoonlightStreamConfigurationResource::get_remote_input_aes_iv);
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "remote_input_aes_iv"), "set_remote_input_aes_iv", "get_remote_input_aes_iv");
}

void MoonlightAdditionalStreamOptions::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_disable_hw_acceleration", "v"), &MoonlightAdditionalStreamOptions::set_disable_hw_acceleration);
	ClassDB::bind_method(D_METHOD("get_disable_hw_acceleration"), &MoonlightAdditionalStreamOptions::get_disable_hw_acceleration);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "disable_hw_acceleration"), "set_disable_hw_acceleration", "get_disable_hw_acceleration");

	ClassDB::bind_method(D_METHOD("set_prefer_hw_decoder", "v"), &MoonlightAdditionalStreamOptions::set_prefer_hw_decoder);
	ClassDB::bind_method(D_METHOD("get_prefer_hw_decoder"), &MoonlightAdditionalStreamOptions::get_prefer_hw_decoder);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "prefer_hw_decoder"), "set_prefer_hw_decoder", "get_prefer_hw_decoder");

	ClassDB::bind_method(D_METHOD("set_verbose", "v"), &MoonlightAdditionalStreamOptions::set_verbose);
	ClassDB::bind_method(D_METHOD("get_verbose"), &MoonlightAdditionalStreamOptions::get_verbose);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "verbose"), "set_verbose", "get_verbose");

	ClassDB::bind_method(D_METHOD("set_video_codec", "v"), &MoonlightAdditionalStreamOptions::set_video_codec);
	ClassDB::bind_method(D_METHOD("get_video_codec"), &MoonlightAdditionalStreamOptions::get_video_codec);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "video_codec"), "set_video_codec", "get_video_codec");
}

} // namespace godot
