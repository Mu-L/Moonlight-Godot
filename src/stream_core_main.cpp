#include "computer_manager.h"
#include "stream_core.h"
#include "stream_core_struct.h"
#include <Limelight.h>
using namespace godot;

// Moonlight 流核心：对外接口与生命周期管理

namespace godot {
MoonlightStreamCore *singleton_instance = nullptr;
}
Mutex *MoonlightStreamCore::lib_global_mutex = nullptr;

static int _video_mask_to_server_codec_mode_support(int video_mask) {
	int scm_mask = 0;

	if (video_mask & VIDEO_FORMAT_MASK_H264)
		scm_mask |= SCM_MASK_H264;
	if (video_mask & VIDEO_FORMAT_MASK_H265)
		scm_mask |= SCM_MASK_HEVC;
	if (video_mask & VIDEO_FORMAT_MASK_AV1)
		scm_mask |= SCM_MASK_AV1;
	if (video_mask & VIDEO_FORMAT_MASK_10BIT)
		scm_mask |= SCM_MASK_10BIT;
	if (video_mask & VIDEO_FORMAT_MASK_YUV444)
		scm_mask |= SCM_MASK_YUV444;

	return scm_mask;
}

static bool _apply_remote_input_keys_from_response(const Dictionary &response, STREAM_CONFIGURATION &cfg) {
	if (!response.has("rikey") || !response.has("rikeyid")) {
		return false;
	}

	String rikey_hex = response.get("rikey", "");
	PackedByteArray key_bytes = rikey_hex.hex_decode();
	if (key_bytes.size() < 16) {
		return false;
	}

	int64_t rikeyid = (int64_t)response.get("rikeyid", 0);

	memcpy(cfg.remoteInputAesKey, key_bytes.ptr(), 16);
	memset(cfg.remoteInputAesIv, 0, 16);
	cfg.remoteInputAesIv[0] = (char)((rikeyid >> 24) & 0xFF);
	cfg.remoteInputAesIv[1] = (char)((rikeyid >> 16) & 0xFF);
	cfg.remoteInputAesIv[2] = (char)((rikeyid >> 8) & 0xFF);
	cfg.remoteInputAesIv[3] = (char)(rikeyid & 0xFF);

	return true;
}

MoonlightStreamCore::MoonlightStreamCore() {
	// 默认参数设定
	singleton_instance = this;
	is_streaming.store(false);
	new_frame_available = false;
	selected_codec_config = CODEC_H264;
	disable_hw_decoding = false;
	use_shader_conversion = false;
	last_idr_time = 0;
	enable_idr_logs = false;
	is_hw_decode_active = false;
	pending_gpu_update.store(false);

	// Verbose defaults already in header; ensure fields exist (no-op here)

	texture_mutex.instantiate();
	queue_mutex.instantiate();
	decode_sem.instantiate();
	codec_mutex.instantiate();

	if (lib_global_mutex == nullptr) {
		lib_global_mutex = memnew(Mutex);
	}

	// 将所有回调初始化为零以防止垃圾指针
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
	cl_callbacks.setHdrMode = _cl_set_hdr_mode;

	dr_callbacks.setup = _dr_setup;
	dr_callbacks.cleanup = _dr_cleanup;
	dr_callbacks.submitDecodeUnit = _dr_submit_decode_unit;
	dr_callbacks.capabilities = 0; // 推送渲染器

	ar_callbacks.init = _ar_init;
	ar_callbacks.cleanup = _ar_cleanup;
	ar_callbacks.decodeAndPlaySample = _ar_decode_and_play_sample;

	get_audio_stream();

	// 一次分配可重用的框架容器。可跨分辨率更改保持。
	v_frame = av_frame_alloc();
	sw_frame = av_frame_alloc(); // 分配SW帧
}
MoonlightStreamCore::~MoonlightStreamCore() {
	stop_play_stream();
	if (v_frame) {
		av_frame_free(&v_frame);
		v_frame = nullptr;
	}
	if (sw_frame) {
		av_frame_free(&sw_frame);
		sw_frame = nullptr;
	}
	if (singleton_instance == this)
		singleton_instance = nullptr;
}

void MoonlightStreamCore::start_play_stream(int host_id, int app_id, Ref<MoonlightStreamConfigurationResource> stream_config_res, Ref<MoonlightAdditionalStreamOptions> additional_options) {
	// 1.彻底清理上一次会话
	if (is_streaming.load() || (connection_thread.is_valid() && connection_thread->is_started())) {
		UtilityFunctions::print(LOG_PREFIX "Stream already running, stopping first...");
		stop_play_stream();
		// 等待：给底层C库时间重置全局状态（winsock等）
		OS::get_singleton()->delay_usec(1000000);
	}
	decode_sem.instantiate();
	queue_mutex.instantiate();
	texture_mutex.instantiate();
	codec_mutex.instantiate();
	packet_queue.clear();

	// Initialize RenderingDevice checks (Main Thread)
	// We do this here as it's safe on main thread before decoder threads start
	rd = RenderingServer::get_singleton()->get_rendering_device();
	if (rd) {
		if (verbose_plugin)
			UtilityFunctions::print(LOG_PREFIX "RenderingDevice available. Hardware acceleration optimizations enabled.");
	} else {
		if (verbose_plugin)
			UtilityFunctions::print(LOG_PREFIX "RenderingDevice NOT available. Falling back to compatibility mode.");
	}

	// 确保音频流存在并已清空
	get_audio_stream();
	if (audio_stream.is_valid()) {
		audio_stream->clear_buffer();
	}
	// 重置帧状态
	if (v_frame)
		av_frame_unref(v_frame);
	else
		v_frame = av_frame_alloc();

	if (sw_frame)
		av_frame_unref(sw_frame);
	else
		sw_frame = av_frame_alloc();
	memset(&dr_callbacks, 0, sizeof(dr_callbacks));
	LiInitializeVideoCallbacks(&dr_callbacks);
	dr_callbacks.setup = _dr_setup;
	dr_callbacks.cleanup = _dr_cleanup;
	dr_callbacks.submitDecodeUnit = _dr_submit_decode_unit;
	dr_callbacks.capabilities = 0; // 推送渲染器

	// 2. 编解码器选择与控制选项（来自 AdditionalStreamOptions）
	Ref<MoonlightAdditionalStreamOptions> add_opts = Ref<MoonlightAdditionalStreamOptions>();
	if (additional_options.is_valid())
		add_opts = additional_options;

	if (add_opts.is_valid()) {
		disable_hw_decoding = add_opts->get_disable_hw_acceleration();
		verbose_plugin = add_opts->get_verbose();
		int codec_val = add_opts->get_video_codec();
		selected_codec_config = (VideoCodecConfig)codec_val;
	} else {
		disable_hw_decoding = false;
		enable_idr_logs = false;
		selected_codec_config = CODEC_H264;
	}
	String codec_name;
	switch (selected_codec_config) {
		case CODEC_AUTO:
			codec_name = "Auto";
			break;
		case CODEC_H264:
			codec_name = "H.264";
			break;
		case CODEC_H265:
			codec_name = "HEVC";
			break;
		case CODEC_AV1:
			codec_name = "AV1";
			break;
		default:
			codec_name = "Unknown";
			break;
	}
	int supported_formats = _probe_video_format(selected_codec_config);
	UtilityFunctions::print(LOG_PREFIX "Codec Selection: ", codec_name, " | Mask: 0x", String::num_int64(supported_formats, 16));

	// 3. 配置流（来自 StreamConfigurationResource）
	LiInitializeStreamConfiguration(&stream_config);
	Ref<MoonlightStreamConfigurationResource> cfg = Ref<MoonlightStreamConfigurationResource>();
	if (stream_config_res.is_valid())
		cfg = stream_config_res;

	// Fill values with provided Resource or defaults
	stream_config.width = cfg.is_valid() && cfg->get_width() ? cfg->get_width() : 1280;
	stream_config.height = cfg.is_valid() && cfg->get_height() ? cfg->get_height() : 720;
	stream_config.fps = cfg.is_valid() && cfg->get_fps() ? cfg->get_fps() : 60;
	stream_config.bitrate = cfg.is_valid() && cfg->get_bitrate() ? cfg->get_bitrate() : 10000;
	stream_config.packetSize = cfg.is_valid() && cfg->get_packet_size() ? cfg->get_packet_size() : 1392;
	stream_config.streamingRemotely = cfg.is_valid() ? cfg->get_streaming_remotely() : STREAM_CFG_AUTO;
	stream_config.audioConfiguration = cfg.is_valid() && cfg->get_audio_configuration() ? cfg->get_audio_configuration() : AUDIO_CONFIGURATION_STEREO;
	stream_config.supportedVideoFormats = cfg.is_valid() && cfg->get_supported_video_formats() ? cfg->get_supported_video_formats() : supported_formats;
	// HDR / 10-bit enable if requested in resource
	if (cfg.is_valid() && cfg->get_color_space() == COLORSPACE_REC_2020) {
		if (stream_config.supportedVideoFormats & (VIDEO_FORMAT_MASK_H265 | VIDEO_FORMAT_MASK_AV1)) {
			stream_config.supportedVideoFormats |= VIDEO_FORMAT_MASK_10BIT;
			UtilityFunctions::print(LOG_PREFIX "HDR (10-bit) capability enabled");
		}
	}
	stream_config.colorSpace = cfg.is_valid() ? cfg->get_color_space() : COLORSPACE_REC_709;
	stream_config.colorRange = cfg.is_valid() ? cfg->get_color_range() : COLOR_RANGE_LIMITED;
	stream_config.clientRefreshRateX100 = cfg.is_valid() ? cfg->get_client_refresh_rate_x100() : 0;

	// Surround audio detection via audioConfiguration value if provided
	// (Resource already encodes channel count via audioConfiguration macros)

	// Configure encryption keys if provided
	if (cfg.is_valid()) {
		PackedByteArray key_bytes = cfg->get_remote_input_aes_key();
		PackedByteArray iv_bytes = cfg->get_remote_input_aes_iv();
		if (key_bytes.size() >= 16) {
			memcpy(stream_config.remoteInputAesKey, key_bytes.ptr(), 16);
			if (iv_bytes.size() >= 16) {
				memcpy(stream_config.remoteInputAesIv, iv_bytes.ptr(), 16);
			} else {
				memset(stream_config.remoteInputAesIv, 0, 16);
			}
			stream_config.encryptionFlags = ENCFLG_NONE;
		}
	}

	// 4. 服务器信息
	// Server info: leave empty for now (can be filled from resources in future)
	ip_storage = std::string();
	session_url_storage = std::string();
	app_version_storage = std::string("0.0.0.0");
	gfe_version_storage = std::string("");
	// For now, keep server_info fields empty unless caller sets them via additional options in future
	server_info.address = ip_storage.c_str();
	server_info.rtspSessionUrl = session_url_storage.c_str();
	server_info.serverInfoAppVersion = app_version_storage.c_str();
	server_info.serverInfoGfeVersion = gfe_version_storage.c_str();
	server_info.serverCodecModeSupport = _video_mask_to_server_codec_mode_support(stream_config.supportedVideoFormats);
	if (server_info.serverCodecModeSupport == 0) {
		server_info.serverCodecModeSupport = SCM_MASK_H264;
	}
	is_streaming.store(true);

	if (verbose_requests) {
		UtilityFunctions::print(LOG_PREFIX "Connection Request Info: ip=", ip_storage.c_str(), " session_url=", session_url_storage.c_str());
	}

	// 5. 获取 Limelight 附加查询参数并委托 ComputerManager 建立流
	const char *extra_q = LiGetLaunchUrlQueryParameters();

	Dictionary opts;
	// 基本参数，供 ComputerManager 构建 /launch 或 /resume 请求
	opts["width"] = stream_config.width;
	opts["height"] = stream_config.height;
	opts["fps"] = stream_config.fps;
	opts["sops"] = stream_config.packetSize;
	opts["server_codec_mode_support"] = server_info.serverCodecModeSupport;
	if (extra_q && strlen(extra_q) > 0) {
		opts["limelight_query_parameters"] = String(extra_q);
	}

	// 将 surroundAudioInfo 添加到 options（由服务器在 /launch 或 /resume 请求中使用）
	if (cfg.is_valid()) {
		opts["surround_audio_info"] = cfg->get_surround_audio_info();
	} else {
		int x = stream_config.audioConfiguration;
		int channelCount = (x >> 8) & 0xFF;
		int channelMask = (x >> 16) & 0xFFFF;
		opts["surround_audio_info"] = (channelMask << 16) | channelCount;
	}

	// 保存 pending 配置以便在回调中使用
	pending_cfg = stream_config_res;
	pending_add_opts = additional_options;

	if (!internal_cm) {
		internal_cm = memnew(ComputerManager);
	}
	internal_cm->establish_stream(host_id, app_id, opts, callable_mp(this, &MoonlightStreamCore::_on_establish_stream_completed));
}

void MoonlightStreamCore::_on_establish_stream_completed(Dictionary response) {
	if (!is_streaming.load())
		return;

	String status = response.get("status", "error");
	if (status != "success") {
		UtilityFunctions::print(LOG_PREFIX "Establish stream failed: ", response.get("message", "unknown"));
		is_streaming.store(false);
		return;
	}

	// 成功：设置 session_url / ip 等并启动线程
	String session_url = response.get("session_url", "");
	String ip = response.get("ip", "");
	String app_version = response.get("app_version", "");
	String gfe_version = response.get("gfe_version", "");
	int server_codec_mode_support = (int)response.get("server_codec_mode_support", server_info.serverCodecModeSupport);
	bool keys_applied = _apply_remote_input_keys_from_response(response, stream_config);
	if (session_url.is_empty()) {
		UtilityFunctions::print(LOG_PREFIX "No session_url returned by ComputerManager");
		is_streaming.store(false);
		return;
	}

	session_url_storage = std::string(session_url.utf8().get_data());
	ip_storage = std::string(ip.utf8().get_data());
	if (!app_version.is_empty()) {
		app_version_storage = std::string(app_version.utf8().get_data());
	}
	if (!gfe_version.is_empty()) {
		gfe_version_storage = std::string(gfe_version.utf8().get_data());
	}
	if (server_codec_mode_support != 0) {
		server_info.serverCodecModeSupport = server_codec_mode_support;
	}
	server_info.rtspSessionUrl = session_url_storage.c_str();
	server_info.address = ip_storage.c_str();
	server_info.serverInfoAppVersion = app_version_storage.c_str();
	server_info.serverInfoGfeVersion = gfe_version_storage.c_str();
	if (strstr(server_info.rtspSessionUrl, "rtspenc://") != nullptr && !keys_applied) {
		UtilityFunctions::printerr(LOG_PREFIX "Warning: encrypted RTSP session detected but rikey/rikeyid missing; handshake may fail.");
	}

	// 启动连接与解码线程（同之前逻辑）
	if (connection_thread.is_valid()) {
		connection_thread->wait_to_finish();
		connection_thread.unref();
	}
	connection_thread.instantiate();
	connection_thread->start(callable_mp(this, &MoonlightStreamCore::_thread_func_connection));
	if (video_decode_thread.is_valid()) {
		video_decode_thread->wait_to_finish();
		video_decode_thread.unref();
	}
	video_decode_thread.instantiate();
	video_decode_thread->start(callable_mp(this, &MoonlightStreamCore::_thread_func_video_decode));
}
void MoonlightStreamCore::stop_play_stream() {
	// 0. 停止流
	UtilityFunctions::print(LOG_PREFIX "Stopping stream...");

	// 1. 设置标志
	is_streaming.store(false);

	// 2. 解锁解码器线程
	if (decode_sem.is_valid()) {
		decode_sem->post();
	}

	// 3. 打破阻塞的网络调用
	LiInterruptConnection();

	// 4. 停止库（对重置内部状态至关重要）
	LiStopConnection();

	// 5. 等待线程停止 (MUST wait before cleaning up shader resources to avoid '_texture_2d_update' crash)
	if (connection_thread.is_valid()) {
		connection_thread->wait_to_finish();
		connection_thread.unref();
	}
	if (video_decode_thread.is_valid()) {
		video_decode_thread->wait_to_finish();
		video_decode_thread.unref();
	}

	// 6. 清理FFmpeg和队列
	if (queue_mutex.is_valid()) {
		queue_mutex->lock();
		while (packet_queue.size() > 0) {
			AVPacket *pkt = packet_queue.front()->get();
			packet_queue.pop_front();
			av_packet_free(&pkt);
		}
		queue_mutex->unlock();
	}
	_cleanup_ffmpeg_video();
	_cleanup_ffmpeg_audio();

	// Clean up shader resources on the render thread to avoid thread context errors
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs) {
		rs->call_on_render_thread(callable_mp(this, &MoonlightStreamCore::_render_thread_cleanup_resources));
	}

	for (int i = 0; i < 3; i++) {
		if (plane_textures[i].is_valid())
			plane_textures[i].unref();
		if (plane_images[i].is_valid())
			plane_images[i].unref();
		plane_buffers[i].resize(0);
	}
	if (shader_material.is_valid()) {
		shader_material.unref();
	}
	use_shader_conversion = false;
	is_hw_decode_active = false;

	// 修复：不要在这里调用 reset_render_target()，否则下一次 start 时 display_rect 为 null 导致黑屏
	// 我们只需要清理视觉残留
	if (display_rect) {
		display_rect->set_material(Ref<Material>());
		display_rect->set_texture(Ref<Texture2D>());
	}
}

void MoonlightStreamCore::set_render_target(TextureRect *target) {
	display_rect = target;
	if (display_rect) {
		// If using shaders, ensure the material is re-applied when target changes
		if (use_shader_conversion && shader_material.is_valid()) {
			display_rect->set_material(shader_material);

			// Set the main texture to fix "Missing Texture" checkerboard and provide size info
			if (rd) {
				display_rect->set_texture(rd_texture_wrappers[0]);
			} else if (plane_textures[0].is_valid()) {
				display_rect->set_texture(plane_textures[0]);
			}
		} else {
			// Fallback / Initial State
			display_rect->set_material(Ref<Material>()); // Clear material
			display_rect->set_texture(Ref<Texture2D>());
		}
	}
}
void MoonlightStreamCore::reset_render_target() {
	if (display_rect) {
		display_rect->set_material(Ref<Material>());
	}
	display_rect = nullptr;
}
Ref<AudioStream> MoonlightStreamCore::get_audio_stream() {
	if (audio_stream.is_null())
		audio_stream.instantiate();
	return audio_stream;
}

// 返回一个包含每个声道单独 AudioStream 的数组（最多 AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT 个）
// 顺序按照 Limelight 约定：0=L,1=R,2=C,3=LFE,4=Ls,5=Rs,6=SideL,7=SideR
Array MoonlightStreamCore::get_audio_streams() {
	// 从当前 stream_config 中读取协商的音频配置以获取声道数。
	int ac = stream_config.audioConfiguration;
	int channel_count = (ac >> 8) & 0xFF;
	if (channel_count <= 0)
		channel_count = 2; // 回退到立体声
	if (channel_count > AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT)
		channel_count = AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT;

	// 确保 vector 大小
	if ((int)audio_streams.size() < channel_count) {
		audio_streams.resize(channel_count);
	}

	// 为每个声道实例化 AudioStreamMoonlight（如果尚未实例化）
	Array out;
	Ref<AudioStreamMoonlight> *arr = audio_streams.ptrw();
	for (int i = 0; i < channel_count; i++) {
		Ref<AudioStreamMoonlight> &r = arr[i];
		if (r.is_null()) {
			r.instantiate();
			r->set_channel_count(1); // 每个单声道实例期望单声道样本
		}
		out.push_back(r);
	}
	return out;
}
void MoonlightStreamCore::reset_audio_stream(bool free_stream) {
	if (audio_stream.is_valid()) {
		audio_stream->clear_buffer();
		if (free_stream)
			audio_stream.unref();
	}
	// 清理按声道分配的单声道流（如果存在）
	if (audio_streams.size() > 0) {
		Ref<AudioStreamMoonlight> *arr = audio_streams.ptrw();
		for (int i = 0; i < (int)audio_streams.size(); i++) {
			Ref<AudioStreamMoonlight> &r = arr[i];
			if (r.is_valid()) {
				r->clear_buffer();
				if (free_stream)
					r.unref();
			}
		}
		if (free_stream) {
			audio_streams.clear();
		}
	}
}

// 原生旁路音频：对外 API（在此文件实现为外部可调用接口，内部实现委托给 stream_core_audio.cpp）
bool MoonlightStreamCore::start_native_audio_bypass() {
	int ac = stream_config.audioConfiguration;
	int channel_count = (ac >> 8) & 0xFF;
	if (channel_count <= 0)
		channel_count = 2;
	int res = _native_audio_start(channel_count);
	if (res == 0) {
		native_audio_bypass_enabled = true;
		native_audio_channel_count = channel_count;
		return true;
	}
	return false;
}

void MoonlightStreamCore::stop_native_audio_bypass() {
	_native_audio_stop();
	native_audio_bypass_enabled = false;
}

bool MoonlightStreamCore::is_native_audio_bypass_running() const {
	return native_audio_bypass_enabled;
}

void MoonlightStreamCore::pause_native_audio_bypass() {
	if (!native_audio_bypass_enabled)
		return;
	native_audio_paused = true;
}

void MoonlightStreamCore::resume_native_audio_bypass() {
	if (!native_audio_bypass_enabled)
		return;
	native_audio_paused = false;
}

bool MoonlightStreamCore::is_native_audio_bypass_paused() const {
	return native_audio_paused;
}

// 线程逻辑
void MoonlightStreamCore::_thread_func_connection() {
	PDECODER_RENDERER_CALLBACKS p_dr_callbacks = &dr_callbacks;
	PAUDIO_RENDERER_CALLBACKS p_ar_callbacks = &ar_callbacks;

	if (pending_add_opts.is_valid()) {
		if (pending_add_opts->get_disable_video()) {
			p_dr_callbacks = nullptr;
		}
		if (pending_add_opts->get_disable_audio()) {
			p_ar_callbacks = nullptr;
		}
	}

	int res = LiStartConnection(&server_info, &stream_config, &cl_callbacks, p_dr_callbacks, p_ar_callbacks, this, 0, this, 0);
	if (res != 0) {
		if (verbose_requests)
			UtilityFunctions::printerr(LOG_PREFIX "Connection failed with error: ", res);
		// 如果连接无法启动，我们必须进行清理
		is_streaming.store(false);
		if (decode_sem.is_valid()) {
			decode_sem->post();
		}
	} else {
		if (verbose_requests)
			UtilityFunctions::print(LOG_PREFIX "LiStartConnection returned 0 (Graceful Termination)");
		// 修复：不要在这里将 is_streaming 设置为 false。LiStartConnection 返回 0 在此上下文中可能是非阻塞的。我们依赖 _cl_connection_terminated 或 stop_play_stream 来清除标志。
	}
}

// 静态回调与绑定
void MoonlightStreamCore::_cl_stage_starting(int stage) { UtilityFunctions::print(LOG_PREFIX "Stage Starting: ", LiGetStageName(stage)); }
void MoonlightStreamCore::_cl_connection_started() {
	UtilityFunctions::print(LOG_PREFIX "Connection Started");
	if (singleton_instance) {
		singleton_instance->call_deferred("emit_signal", "connection_started");
	}
}
void MoonlightStreamCore::_cl_connection_terminated(int error_code) {
	UtilityFunctions::print(LOG_PREFIX "Connection Terminated: ", error_code);
	// 修复：在此处处理终止以确保解码器仅在连接真正断开时停止
	if (singleton_instance) {
		singleton_instance->is_streaming.store(false);
		if (singleton_instance->decode_sem.is_valid()) {
			singleton_instance->decode_sem->post();
		}
		// 向调用者发送包含错误详细信息的信号
		String msg = singleton_instance->_get_error_string(error_code);
		singleton_instance->call_deferred("emit_signal", "connection_terminated", error_code, msg);
	}
}
void MoonlightStreamCore::_cl_log_message(const char *format, ...) {
	va_list args;
	va_start(args, format);
	char buffer[2048];
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	String msg = String(buffer);
	if (!singleton_instance || singleton_instance->verbose_limelight) {
		UtilityFunctions::print(LOG_PREFIX "Log from lib: ", msg);
	}
	if (singleton_instance) {
		singleton_instance->call_deferred("emit_signal", "log_message", msg);
	}
}
void MoonlightStreamCore::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start_play_stream", "host_id", "app_id", "stream_config_res", "additional_options"), &MoonlightStreamCore::start_play_stream, DEFVAL(Ref<MoonlightAdditionalStreamOptions>()));
	ClassDB::bind_method(D_METHOD("stop_play_stream"), &MoonlightStreamCore::stop_play_stream);
	ClassDB::bind_method(D_METHOD("set_render_target", "texture_rect"), &MoonlightStreamCore::set_render_target);
	ClassDB::bind_method(D_METHOD("reset_render_target"), &MoonlightStreamCore::reset_render_target);
	ClassDB::bind_method(D_METHOD("get_audio_stream"), &MoonlightStreamCore::get_audio_stream);
	ClassDB::bind_method(D_METHOD("get_audio_streams"), &MoonlightStreamCore::get_audio_streams);
	ClassDB::bind_method(D_METHOD("reset_audio_stream", "free_stream"), &MoonlightStreamCore::reset_audio_stream, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("start_native_audio_bypass"), &MoonlightStreamCore::start_native_audio_bypass);
	ClassDB::bind_method(D_METHOD("stop_native_audio_bypass"), &MoonlightStreamCore::stop_native_audio_bypass);
	ClassDB::bind_method(D_METHOD("is_native_audio_bypass_running"), &MoonlightStreamCore::is_native_audio_bypass_running);
	ClassDB::bind_method(D_METHOD("pause_native_audio_bypass"), &MoonlightStreamCore::pause_native_audio_bypass);
	ClassDB::bind_method(D_METHOD("resume_native_audio_bypass"), &MoonlightStreamCore::resume_native_audio_bypass);
	ClassDB::bind_method(D_METHOD("is_native_audio_bypass_paused"), &MoonlightStreamCore::is_native_audio_bypass_paused);

	// 输入 API 绑定
	ClassDB::bind_method(D_METHOD("send_mouse_move_event", "delta_x", "delta_y"), &MoonlightStreamCore::send_mouse_move_event);
	ClassDB::bind_method(D_METHOD("send_mouse_position_event", "x", "y", "reference_width", "reference_height"), &MoonlightStreamCore::send_mouse_position_event);
	ClassDB::bind_method(D_METHOD("send_mouse_move_as_mouse_position_event", "delta_x", "delta_y", "reference_width", "reference_height"), &MoonlightStreamCore::send_mouse_move_as_mouse_position_event);
	ClassDB::bind_method(D_METHOD("send_touch_event", "event_type", "pointer_id", "x", "y", "pressure", "contact_major", "contact_minor", "rotation"), &MoonlightStreamCore::send_touch_event);
	ClassDB::bind_method(D_METHOD("send_pen_event", "event_type", "tool_type", "pen_buttons", "x", "y", "pressure", "contact_major", "contact_minor", "rotation", "tilt"), &MoonlightStreamCore::send_pen_event);
	ClassDB::bind_method(D_METHOD("send_mouse_button_event", "action", "button"), &MoonlightStreamCore::send_mouse_button_event);
	ClassDB::bind_method(D_METHOD("send_keyboard_event", "key_code", "action", "modifiers"), &MoonlightStreamCore::send_keyboard_event);
	ClassDB::bind_method(D_METHOD("send_keyboard_event2", "key_code", "action", "modifiers", "flags"), &MoonlightStreamCore::send_keyboard_event2);
	ClassDB::bind_method(D_METHOD("send_utf8_text_event", "text"), &MoonlightStreamCore::send_utf8_text_event);
	ClassDB::bind_method(D_METHOD("send_controller_event", "button_flags", "left_trigger", "right_trigger", "lx", "ly", "rx", "ry"), &MoonlightStreamCore::send_controller_event);
	ClassDB::bind_method(D_METHOD("send_multi_controller_event", "controller_number", "active_gamepad_mask", "button_flags", "left_trigger", "right_trigger", "lx", "ly", "rx", "ry"), &MoonlightStreamCore::send_multi_controller_event);
	ClassDB::bind_method(D_METHOD("send_controller_arrival_event", "controller_number", "active_gamepad_mask", "type", "supported_button_flags", "capabilities"), &MoonlightStreamCore::send_controller_arrival_event);
	ClassDB::bind_method(D_METHOD("send_controller_touch_event", "controller_number", "event_type", "pointer_id", "x", "y", "pressure"), &MoonlightStreamCore::send_controller_touch_event);
	ClassDB::bind_method(D_METHOD("send_controller_motion_event", "controller_number", "motion_type", "x", "y", "z"), &MoonlightStreamCore::send_controller_motion_event);
	ClassDB::bind_method(D_METHOD("send_controller_battery_event", "controller_number", "battery_state", "battery_percentage"), &MoonlightStreamCore::send_controller_battery_event);
	ClassDB::bind_method(D_METHOD("send_scroll_event", "scroll_clicks"), &MoonlightStreamCore::send_scroll_event);
	ClassDB::bind_method(D_METHOD("send_high_res_scroll_event", "scroll_amount"), &MoonlightStreamCore::send_high_res_scroll_event);
	ClassDB::bind_method(D_METHOD("send_hscroll_event", "scroll_clicks"), &MoonlightStreamCore::send_hscroll_event);
	ClassDB::bind_method(D_METHOD("send_high_res_hscroll_event", "scroll_amount"), &MoonlightStreamCore::send_high_res_hscroll_event);
	ClassDB::bind_method(D_METHOD("get_host_feature_flags"), &MoonlightStreamCore::get_host_feature_flags);

	// 绑定内部更新方法以进行延迟调用
	ClassDB::bind_method(D_METHOD("_perform_gpu_update"), &MoonlightStreamCore::_perform_gpu_update);
	ClassDB::bind_method(D_METHOD("_render_thread_setup_shader", "width", "height", "format", "colorspace", "color_range", "bit_depth"), &MoonlightStreamCore::_render_thread_setup_shader);
	ClassDB::bind_method(D_METHOD("_render_thread_cleanup_resources"), &MoonlightStreamCore::_render_thread_cleanup_resources);

	ADD_SIGNAL(MethodInfo("connection_started"));
	ADD_SIGNAL(MethodInfo("connection_terminated", PropertyInfo(Variant::INT, "error_code"), PropertyInfo(Variant::STRING, "message")));
	ADD_SIGNAL(MethodInfo("hdr_mode_changed", PropertyInfo(Variant::BOOL, "enabled"), PropertyInfo(Variant::DICTIONARY, "metadata")));
	ADD_SIGNAL(MethodInfo("log_message", PropertyInfo(Variant::STRING, "message")));
	ADD_SIGNAL(MethodInfo("warning_message", PropertyInfo(Variant::STRING, "type"), PropertyInfo(Variant::STRING, "message")));

	BIND_ENUM_CONSTANT(CODEC_AUTO);
	BIND_ENUM_CONSTANT(CODEC_H264);
	BIND_ENUM_CONSTANT(CODEC_H265);
	BIND_ENUM_CONSTANT(CODEC_AV1);
}