#include "stream_core.h"
using namespace godot;

// Moonlight 流核心：对外接口与生命周期管理

namespace godot {
MoonlightStreamCore *singleton_instance = nullptr;
}
Mutex *MoonlightStreamCore::lib_global_mutex = nullptr;

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

void MoonlightStreamCore::start_play_stream(Dictionary options) {
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

	// 2. 编解码器选择
	disable_hw_decoding = options.get("disable_hw_acceleration", false);
	enable_idr_logs = options.get("debug_idr_log", false); // 默认关闭

	// Verbose options (override defaults in header)
	if (options.has("verbose_decoders"))
		verbose_decoders = options.get("verbose_decoders", verbose_decoders);
	if (options.has("verbose_requests"))
		verbose_requests = options.get("verbose_requests", verbose_requests);
	if (options.has("verbose_limelight"))
		verbose_limelight = options.get("verbose_limelight", verbose_limelight);
	if (options.has("verbose_plugin"))
		verbose_plugin = options.get("verbose_plugin", verbose_plugin);

	int codec_val = options.get("video_codec", (int)CODEC_H264);
	selected_codec_config = (VideoCodecConfig)codec_val;
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

	// 3. 配置流
	LiInitializeStreamConfiguration(&stream_config);
	stream_config.width = options.get("width", 1280);
	stream_config.height = options.get("height", 720);
	stream_config.fps = options.get("fps", 60);
	stream_config.bitrate = options.get("bitrate", 10000);
	stream_config.packetSize = options.get("packet_size", 1392);
	stream_config.streamingRemotely = STREAM_CFG_AUTO;
	stream_config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
	stream_config.supportedVideoFormats = supported_formats;
	// 如果有请求且编解码器支持，则启用HDR（10bit）支持
	if (options.get("enable_hdr", false)) {
		// 检查我们是否使用支持10bit的编解码器（HEVC或AV1）
		if (supported_formats & (VIDEO_FORMAT_MASK_H265 | VIDEO_FORMAT_MASK_AV1)) {
			stream_config.supportedVideoFormats |= VIDEO_FORMAT_MASK_10BIT;
			UtilityFunctions::print(LOG_PREFIX "HDR (10-bit) capability enabled");
		}
	}
	if (options.has("color_space"))
		stream_config.colorSpace = options["color_space"];
	if (options.has("color_range"))
		stream_config.colorRange = options["color_range"];

	if (options.has("surround_audio_info")) {
		int surround_info = options["surround_audio_info"];
		int count = (surround_info >> 8) & 0xFF;
		if (count == 6)
			stream_config.audioConfiguration = AUDIO_CONFIGURATION_51_SURROUND;
		if (count == 8)
			stream_config.audioConfiguration = AUDIO_CONFIGURATION_71_SURROUND;
	}

	// 配置流密钥
	String rikey = options.get("rikey", "");
	if (!rikey.is_empty()) {
		PackedByteArray key_bytes = rikey.hex_decode();
		if (key_bytes.size() >= 16) {
			memcpy(stream_config.remoteInputAesKey, key_bytes.ptr(), 16);
		}
		memset(stream_config.remoteInputAesIv, 0, 16);
		stream_config.encryptionFlags = ENCFLG_NONE;
	}

	// 4. 服务器信息
	ip_storage = String(options.get("ip", "")).utf8().get_data();
	session_url_storage = String(options.get("session_url", "")).utf8().get_data();
	app_version_storage = String(options.get("app_version", "0.0.0.0")).utf8().get_data();
	gfe_version_storage = String(options.get("gfe_version", "")).utf8().get_data();
	server_info.address = ip_storage.c_str();
	server_info.rtspSessionUrl = session_url_storage.c_str();
	server_info.serverInfoAppVersion = app_version_storage.c_str();
	server_info.serverInfoGfeVersion = gfe_version_storage.c_str();
	server_info.serverCodecModeSupport = options.get("server_codec_mode_support", 0);
	is_streaming.store(true);

	if (verbose_requests) {
		UtilityFunctions::print(LOG_PREFIX "Connection Request Info: ip=", ip_storage.c_str(), " session_url=", session_url_storage.c_str());
	}

	// 5. 启动线程
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
void MoonlightStreamCore::reset_audio_stream(bool free_stream) {
	if (audio_stream.is_valid()) {
		audio_stream->clear_buffer();
		if (free_stream)
			audio_stream.unref();
	}
}

// 线程逻辑
void MoonlightStreamCore::_thread_func_connection() {
	int res = LiStartConnection(&server_info, &stream_config, &cl_callbacks, &dr_callbacks, &ar_callbacks, this, 0, this, 0);
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
	ClassDB::bind_method(D_METHOD("start_play_stream", "options"), &MoonlightStreamCore::start_play_stream);
	ClassDB::bind_method(D_METHOD("stop_play_stream"), &MoonlightStreamCore::stop_play_stream);
	ClassDB::bind_method(D_METHOD("set_render_target", "texture_rect"), &MoonlightStreamCore::set_render_target);
	ClassDB::bind_method(D_METHOD("reset_render_target"), &MoonlightStreamCore::reset_render_target);
	ClassDB::bind_method(D_METHOD("get_audio_stream"), &MoonlightStreamCore::get_audio_stream);
	ClassDB::bind_method(D_METHOD("reset_audio_stream", "free_stream"), &MoonlightStreamCore::reset_audio_stream, DEFVAL(false));

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

	// 鼠标相关
	BIND_ENUM_CONSTANT(MOUSE_BUTTON_LEFT);
	BIND_ENUM_CONSTANT(MOUSE_BUTTON_MIDDLE);
	BIND_ENUM_CONSTANT(MOUSE_BUTTON_RIGHT);
	BIND_ENUM_CONSTANT(MOUSE_BUTTON_X1);
	BIND_ENUM_CONSTANT(MOUSE_BUTTON_X2);
	BIND_ENUM_CONSTANT(MOUSE_BUTTON_ACTION_PRESS);
	BIND_ENUM_CONSTANT(MOUSE_BUTTON_ACTION_RELEASE);

	// 键盘相关
	BIND_ENUM_CONSTANT(KEY_ACTION_DOWN_LIMIT);
	BIND_ENUM_CONSTANT(KEY_ACTION_UP_LIMIT);
	BIND_ENUM_CONSTANT(MODIFIER_NONE);
	BIND_ENUM_CONSTANT(MODIFIER_SHIFT_BIT);
	BIND_ENUM_CONSTANT(MODIFIER_CTRL_BIT);
	BIND_ENUM_CONSTANT(MODIFIER_ALT_BIT);
	BIND_ENUM_CONSTANT(MODIFIER_META_BIT);

	// 触摸与笔相关
	BIND_ENUM_CONSTANT(TOUCH_EVENT_HOVER);
	BIND_ENUM_CONSTANT(TOUCH_EVENT_DOWN);
	BIND_ENUM_CONSTANT(TOUCH_EVENT_UP);
	BIND_ENUM_CONSTANT(TOUCH_EVENT_MOVE);
	BIND_ENUM_CONSTANT(TOUCH_EVENT_CANCEL);
	BIND_ENUM_CONSTANT(TOUCH_EVENT_BUTTON_ONLY);
	BIND_ENUM_CONSTANT(TOUCH_EVENT_HOVER_LEAVE);
	BIND_ENUM_CONSTANT(TOUCH_EVENT_CANCEL_ALL);
	BIND_ENUM_CONSTANT(TOOL_TYPE_UNKNOWN);
	BIND_ENUM_CONSTANT(TOOL_TYPE_PEN);
	BIND_ENUM_CONSTANT(TOOL_TYPE_ERASER);
	BIND_ENUM_CONSTANT(PEN_BUTTON_PRIMARY);
	BIND_ENUM_CONSTANT(PEN_BUTTON_SECONDARY);
	BIND_ENUM_CONSTANT(PEN_BUTTON_TERTIARY);

	// 控制器相关
	BIND_ENUM_CONSTANT(CONTROLLER_A);
	BIND_ENUM_CONSTANT(CONTROLLER_B);
	BIND_ENUM_CONSTANT(CONTROLLER_X);
	BIND_ENUM_CONSTANT(CONTROLLER_Y);
	BIND_ENUM_CONSTANT(CONTROLLER_UP);
	BIND_ENUM_CONSTANT(CONTROLLER_DOWN);
	BIND_ENUM_CONSTANT(CONTROLLER_LEFT);
	BIND_ENUM_CONSTANT(CONTROLLER_RIGHT);
	BIND_ENUM_CONSTANT(CONTROLLER_LB);
	BIND_ENUM_CONSTANT(CONTROLLER_RB);
	BIND_ENUM_CONSTANT(CONTROLLER_PLAY);
	BIND_ENUM_CONSTANT(CONTROLLER_BACK);
	BIND_ENUM_CONSTANT(CONTROLLER_LS_CLK);
	BIND_ENUM_CONSTANT(CONTROLLER_RS_CLK);
	BIND_ENUM_CONSTANT(CONTROLLER_SPECIAL);
	BIND_ENUM_CONSTANT(CONTROLLER_PADDLE1);
	BIND_ENUM_CONSTANT(CONTROLLER_PADDLE2);
	BIND_ENUM_CONSTANT(CONTROLLER_PADDLE3);
	BIND_ENUM_CONSTANT(CONTROLLER_PADDLE4);
	BIND_ENUM_CONSTANT(CONTROLLER_TOUCHPAD);
	BIND_ENUM_CONSTANT(CONTROLLER_MISC);

	BIND_ENUM_CONSTANT(CONTROLLER_TYPE_UNKNOWN);
	BIND_ENUM_CONSTANT(CONTROLLER_TYPE_XBOX);
	BIND_ENUM_CONSTANT(CONTROLLER_TYPE_PS);
	BIND_ENUM_CONSTANT(CONTROLLER_TYPE_NINTENDO);

	BIND_ENUM_CONSTANT(CONTROLLER_CAP_ANALOG_TRIGGERS);
	BIND_ENUM_CONSTANT(CONTROLLER_CAP_RUMBLE);
	BIND_ENUM_CONSTANT(CONTROLLER_CAP_TRIGGER_RUMBLE);
	BIND_ENUM_CONSTANT(CONTROLLER_CAP_TOUCHPAD);
	BIND_ENUM_CONSTANT(CONTROLLER_CAP_ACCEL);
	BIND_ENUM_CONSTANT(CONTROLLER_CAP_GYRO);
	BIND_ENUM_CONSTANT(CONTROLLER_CAP_BATTERY_STATE);
	BIND_ENUM_CONSTANT(CONTROLLER_CAP_RGB_LED);

	// 运动与电池
	BIND_ENUM_CONSTANT(MOTION_TYPE_ACCEL);
	BIND_ENUM_CONSTANT(MOTION_TYPE_GYRO);

	BIND_ENUM_CONSTANT(BATTERY_STATE_UNKNOWN);
	BIND_ENUM_CONSTANT(BATTERY_STATE_NOT_PRESENT);
	BIND_ENUM_CONSTANT(BATTERY_STATE_DISCHARGING);
	BIND_ENUM_CONSTANT(BATTERY_STATE_CHARGING);
	BIND_ENUM_CONSTANT(BATTERY_STATE_NOT_CHARGING);
	BIND_ENUM_CONSTANT(BATTERY_STATE_FULL);

	// 特殊输入/错误常量作为枚举绑定
	BIND_ENUM_CONSTANT(INPUT_ROT_UNKNOWN);
	BIND_ENUM_CONSTANT(INPUT_TILT_UNKNOWN);
	BIND_ENUM_CONSTANT(INPUT_BATTERY_PERCENTAGE_UNKNOWN);
	BIND_ENUM_CONSTANT(INPUT_ERR_UNSUPPORTED);
}