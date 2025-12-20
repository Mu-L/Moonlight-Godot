#include "moonlight_stream_core.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/audio_server.hpp>

// 静态成员变量定义
std::map<void *, MoonlightStreamCore *> MoonlightStreamCore::instance_map;

MoonlightStreamCore::MoonlightStreamCore() {
    // 初始化FFmpeg相关变量
    video_codec_ctx = nullptr;
    video_frame = nullptr;
    video_packet = nullptr;
    sws_ctx = nullptr;
    
    audio_codec_ctx = nullptr;
    audio_frame = nullptr;
    audio_packet = nullptr;
    
    current_width = 0;
    current_height = 0;
    
    // 注册实例到映射表
    instance_map[this] = this;
}

MoonlightStreamCore::~MoonlightStreamCore() {
    _cleanup_ffmpeg();
    
    // 从实例映射表中移除
    auto it = instance_map.find(this);
    if (it != instance_map.end()) {
        instance_map.erase(it);
    }
}

void MoonlightStreamCore::_bind_methods() {
    // 绑定方法到Godot
    ClassDB::bind_method(D_METHOD("start_connection", "address", "config"), &MoonlightStreamCore::start_connection);
    ClassDB::bind_method(D_METHOD("stop_connection"), &MoonlightStreamCore::stop_connection);
    ClassDB::bind_method(D_METHOD("get_video_viewport"), &MoonlightStreamCore::get_video_viewport);
    ClassDB::bind_method(D_METHOD("get_audio_generators"), &MoonlightStreamCore::get_audio_generators);
    ClassDB::bind_method(D_METHOD("set_audio_playback", "channel_idx", "playback"), &MoonlightStreamCore::set_audio_playback);
    
    // 绑定枚举
    BIND_ENUM_CONSTANT(REMOTE_LOCAL);
    BIND_ENUM_CONSTANT(REMOTE_REMOTE);
    BIND_ENUM_CONSTANT(REMOTE_AUTO);
}

void MoonlightStreamCore::_notification(int p_what) {
    switch (p_what) {
        case NOTIFICATION_READY:
            // 节点准备就绪时的初始化
            break;
        case NOTIFICATION_PROCESS:
            // 每帧处理
            break;
        case NOTIFICATION_EXIT_TREE:
            // 节点从场景树移除时的清理
            stop_connection();
            break;
    }
}

void MoonlightStreamCore::start_connection(const String &address, const Dictionary &config) {
    if (is_streaming) {
        Godot::print("Connection already started");
        return;
    }
    
    Godot::print("Starting connection to: " + address);
    is_streaming = true;
    
    // TODO: 实现实际的连接逻辑
    // 这里应该初始化Moonlight连接
}

void MoonlightStreamCore::stop_connection() {
    if (!is_streaming) {
        return;
    }
    
    Godot::print("Stopping connection");
    is_streaming = false;
    
    _cleanup_ffmpeg();
    
    // TODO: 实现实际的断开连接逻辑
}

SubViewport *MoonlightStreamCore::get_video_viewport() const {
    return sub_viewport;
}

Array MoonlightStreamCore::get_audio_generators() const {
    Array generators;
    std::lock_guard<std::mutex> lock(audio_mutex);
    
    for (const auto &channel : audio_channels) {
        generators.push_back(channel.generator);
    }
    
    return generators;
}

void MoonlightStreamCore::set_audio_playback(int channel_idx, const Ref<AudioStreamGeneratorPlayback> &playback) {
    std::lock_guard<std::mutex> lock(audio_mutex);
    
    if (channel_idx >= 0 && channel_idx < static_cast<int>(audio_channels.size())) {
        audio_channels[channel_idx].playback = playback;
    }
}

void MoonlightStreamCore::_cleanup_ffmpeg() {
    // 清理视频解码器
    if (video_codec_ctx) {
        avcodec_free_context(&video_codec_ctx);
        video_codec_ctx = nullptr;
    }
    
    if (video_frame) {
        av_frame_free(&video_frame);
        video_frame = nullptr;
    }
    
    if (video_packet) {
        av_packet_free(&video_packet);
        video_packet = nullptr;
    }
    
    if (sws_ctx) {
        sws_freeContext(sws_ctx);
        sws_ctx = nullptr;
    }
    
    // 清理音频解码器
    if (audio_codec_ctx) {
        avcodec_free_context(&audio_codec_ctx);
        audio_codec_ctx = nullptr;
    }
    
    if (audio_frame) {
        av_frame_free(&audio_frame);
        audio_frame = nullptr;
    }
    
    if (audio_packet) {
        av_packet_free(&audio_packet);
        audio_packet = nullptr;
    }
}

void MoonlightStreamCore::_setup_video_resources(int width, int height) {
    if (sub_viewport == nullptr) {
        // 创建子视口
        sub_viewport = memnew(SubViewport);
        sub_viewport->set_size(Vector2i(width, height));
        sub_viewport->set_transparent_background(true);
        add_child(sub_viewport);
        
        // 创建纹理矩形
        video_display_rect = memnew(TextureRect);
        video_display_rect->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
        sub_viewport->add_child(video_display_rect);
        
        // 创建图像纹理
        video_texture.instantiate();
        video_display_rect->set_texture(video_texture);
    } else {
        sub_viewport->set_size(Vector2i(width, height));
    }
    
    current_width = width;
    current_height = height;
}

bool MoonlightStreamCore::_init_video_decoder(PDECODE_UNIT du) {
    // TODO: 实现视频解码器初始化
    return false;
}

bool MoonlightStreamCore::_init_audio_decoder(const OPUS_MULTISTREAM_CONFIGURATION *config) {
    // TODO: 实现音频解码器初始化
    return false;
}

void MoonlightStreamCore::_setup_audio_generators_deferred(int channel_count, int sample_rate) {
    // TODO: 实现音频生成器设置
}

// C风格回调函数的包装器实现
int MoonlightStreamCore::_on_video_setup(int videoFormat, int width, int height, int redrawRate) {
    _setup_video_resources(width, height);
    return 0;
}

void MoonlightStreamCore::_on_video_cleanup() {
    _cleanup_ffmpeg();
}

int MoonlightStreamCore::_on_submit_decode_unit(PDECODE_UNIT decodeUnit) {
    // TODO: 实现解码单元提交
    return 0;
}

int MoonlightStreamCore::_on_audio_init(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig) {
    // TODO: 实现音频初始化
    return 0;
}

void MoonlightStreamCore::_on_decode_and_play_sample(char *sampleData, int sampleLength) {
    // TODO: 实现音频样本解码和播放
}

void MoonlightStreamCore::_on_connection_started() {
    Godot::print("Connection started successfully");
}

void MoonlightStreamCore::_on_connection_terminated(int errorCode) {
    Godot::print("Connection terminated with error code: " + String::num_int64(errorCode));
    is_streaming = false;
    _cleanup_ffmpeg();
}

void MoonlightStreamCore::_on_connection_status_update(int connectionStatus) {
    // TODO: 实现连接状态更新处理
}
