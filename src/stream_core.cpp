#include "stream_core.h"
#include "yuvtorgb_shader.h"

// Android JNI Integration for Zero-Copy (SurfaceTexture)
#ifdef __ANDROID__
#include <android/native_window_jni.h>
#include <jni.h>

// On-demand JNIEnv retrieval as requested
static JNIEnv *GetJNIEnv() {
	JavaVM *vm;
	jsize vm_count;
	jint result = JNI_GetCreatedJavaVMs(&vm, 1, &vm_count);
	if (result != JNI_OK || vm_count == 0) {
		return nullptr;
	}
	JNIEnv *env;
	result = vm->AttachCurrentThread(&env, NULL);
	if (result != JNI_OK) {
		return nullptr;
	}
	return env;
}
#endif

using namespace godot;

static MoonlightStreamCore *singleton_instance = nullptr;
Mutex *MoonlightStreamCore::lib_global_mutex = nullptr;

// ============================================================================
// moonlight流核心
// ============================================================================
// 修复：在 _handle_ar_init 中，我们应使用 _try_open_decoder 来测试硬件解码器，而不是直接调用 avcodec_open2

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
		UtilityFunctions::print(LOG_PREFIX "RenderingDevice available. Hardware acceleration optimizations enabled.");
	} else {
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

void MoonlightStreamCore::_update_display_texture() {
	// This function was for legacy SW fallback. Since we removed legacy CPU conversion,
	// this is practically a no-op or placeholder.
	return;
}

// ============================================================================
// 线程逻辑
// ============================================================================
// 修复：在 _handle_ar_init 中，我们应使用 _try_open_decoder 来测试硬件解码器，而不是直接调用 avcodec_open2

void MoonlightStreamCore::_thread_func_connection() {
	int res = LiStartConnection(&server_info, &stream_config, &cl_callbacks, &dr_callbacks, &ar_callbacks, this, 0, this, 0);
	if (res != 0) {
		UtilityFunctions::printerr(LOG_PREFIX "Connection failed with error: ", res);
		// 如果连接无法启动，我们必须进行清理
		is_streaming.store(false);
		if (decode_sem.is_valid()) {
			decode_sem->post();
		}
	} else {
		UtilityFunctions::print(LOG_PREFIX "LiStartConnection returned 0 (Graceful Termination)");
		// 修复：不要在这里将 is_streaming 设置为 false。LiStartConnection 返回 0 在此上下文中可能是非阻塞的。我们依赖 _cl_connection_terminated 或 stop_play_stream 来清除标志。
	}
}

void MoonlightStreamCore::_thread_func_video_decode() {
	UtilityFunctions::print(LOG_PREFIX "Video Decode Thread Started");
	while (true) {
		// 等待数据包
		if (decode_sem.is_valid()) {
			decode_sem->wait();
		} else {
			break;
		}
		// 尽可能排空队列
		while (true) {
			AVPacket *pkt = nullptr;
			int queue_size = 0;
			queue_mutex->lock();
			queue_size = packet_queue.size();
			if (queue_size > 0) {
				pkt = packet_queue.front()->get();
				packet_queue.pop_front();
			}
			queue_mutex->unlock();

			// Exit condition
			if (!is_streaming.load() && pkt == nullptr) {
				UtilityFunctions::print(LOG_PREFIX "Video Decode Thread Stopping (Queue empty)");
				goto end_of_thread;
			}

			if (pkt == nullptr) {
				break;
			}

			codec_mutex->lock();
			if (v_codec_ctx) {
				int ret = avcodec_send_packet(v_codec_ctx, pkt);
				if (ret >= 0) {
					while (true) {
						ret = avcodec_receive_frame(v_codec_ctx, v_frame);
						if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
							break;
						if (ret < 0) {
							// 严重解码错误，请求新的数据流 - Throttled
							_request_idr_frame("Decode Error " + String::num_int64(ret));
							break;
						}
						// 优化：帧丢弃逻辑。
						bool is_keyframe = (v_frame->flags & AV_FRAME_FLAG_KEY);
						if (queue_size > 12 && !is_keyframe) {
							av_frame_unref(v_frame);
							continue;
						}

						AVFrame *display_frame = v_frame;

						// 1. Hardware frame transfer to system memory
						bool is_hw_frame = (v_frame->format == hw_pix_fmt && hw_device_ctx);
						if (is_hw_frame) {
							if (!sw_frame)
								sw_frame = av_frame_alloc();
							// Download frame from GPU/Device to CPU memory for upload to Godot
							int err = av_hwframe_transfer_data(sw_frame, v_frame, 0);
							if (err < 0) {
								UtilityFunctions::printerr(LOG_PREFIX "Failed to transfer hardware frame: ", err);
								continue;
							}
							av_frame_copy_props(sw_frame, v_frame);
							display_frame = sw_frame;
						}

						// 2. Upload to Godot Texture via RenderingServer (Shader Path)
						if (use_shader_conversion) {
							_update_textures_with_frame(display_frame);
						} else {
							// Should rarely happen if setup logic is correct.
							// Attempt to setup shader if we haven't yet (lazy init for SW fallback)
							if (video_width > 0 && video_height > 0) {
								call_deferred("_setup_shader_integration", video_width, video_height, (AVPixelFormat)display_frame->format,
										_resolve_frame_colorspace(display_frame), (AVColorRange)display_frame->color_range, 8);
								// We lose this frame, but next one will catch up
							}
						}

						// Unref temp frame if used
						if (is_hw_frame) {
							av_frame_unref(sw_frame);
						}
						av_frame_unref(v_frame);
					}
				} else {
					// 发送数据包失败 - Throttled
					if (ret != AVERROR(EAGAIN)) {
						_request_idr_frame("Send Packet Failed " + String::num_int64(ret));
					}
				}
			}
			codec_mutex->unlock(); // 解锁
			av_packet_free(&pkt);
		}
	}
end_of_thread:
	UtilityFunctions::print(LOG_PREFIX "Video Decode Thread Exited");
}

// ============================================================================
// FFmpeg 辅助方法+AVPacket解码
// ============================================================================
// 修复：在 _handle_ar_init 中，我们应使用 _try_open_decoder 来测试硬件解码器，而不是直接调用 avcodec_open2

void MoonlightStreamCore::_request_idr_frame(const String &reason) {
	uint64_t now = Time::get_singleton()->get_ticks_msec();
	// Throttle to once every 2000ms to prevent log spam and network congestion
	if (now - last_idr_time > 2000) {
		if (enable_idr_logs) {
			UtilityFunctions::print(LOG_PREFIX "Requesting IDR: ", reason);
		}
		LiRequestIdrFrame();
		last_idr_time = now;
	}
}

Vector<AVHWDeviceType> MoonlightStreamCore::_get_supported_hw_devices() {
	Vector<AVHWDeviceType> types;
#if defined(__ANDROID__)
	// Android: 返回空列表。
	// 这会迫使逻辑只使用 AV_HWDEVICE_TYPE_NONE。
	// 当我们使用 AV_HWDEVICE_TYPE_NONE 并通过名称 (如 "h264_mediacodec") 打开解码器时，
	// FFmpeg 会进入 MediaCodec Buffer 模式。
	// 这规避了复杂的 JNI/Surface 设置，并且是 Godot 这种自绘引擎所需要的（我们需要 YUV 数据）。
#elif defined(_WIN32)
	types.push_back(AV_HWDEVICE_TYPE_D3D11VA);
	types.push_back(AV_HWDEVICE_TYPE_DXVA2);
	types.push_back(AV_HWDEVICE_TYPE_VULKAN);
	types.push_back(AV_HWDEVICE_TYPE_CUDA);
#elif defined(__APPLE__)
	types.push_back(AV_HWDEVICE_TYPE_VIDEOTOOLBOX);
#elif defined(__linux__)
	types.push_back(AV_HWDEVICE_TYPE_VAAPI);
	types.push_back(AV_HWDEVICE_TYPE_VULKAN);
	types.push_back(AV_HWDEVICE_TYPE_QSV);
	types.push_back(AV_HWDEVICE_TYPE_CUDA);
#endif
	return types;
}

int MoonlightStreamCore::_probe_video_format(MoonlightStreamCore::VideoCodecConfig preference) {
	int supported_mask = 0;
	int test_w = 1280;
	int test_h = 720;

	Vector<AVHWDeviceType> hw_devices;
	if (!disable_hw_decoding) {
		hw_devices = _get_supported_hw_devices();
	}
	// 总是最后测试无硬件上下文模式（软件或 Android MediaCodec Buffer 模式）
	hw_devices.push_back(AV_HWDEVICE_TYPE_NONE);

	auto test_family = [&](int family) -> bool {
		Vector<String> candidates = _get_candidate_decoders(family);
		for (int i = 0; i < candidates.size(); i++) {
			// 修复手动禁用硬件解码无效的问题：在探测阶段跳过 mediacodec
			if (disable_hw_decoding && candidates[i].find("_mediacodec") != -1) {
				continue;
			}
			for (int j = 0; j < hw_devices.size(); j++) {
				if (_try_open_decoder(candidates[i], test_w, test_h, hw_devices[j]) == 0) {
					_cleanup_ffmpeg_video();
					return true;
				}
			}
		}
		return false;
	};

	bool h264_ok = test_family(CODEC_FAMILY_H264);
	bool hevc_ok = (preference == CODEC_H265 || preference == CODEC_AUTO) && test_family(CODEC_FAMILY_H265);
	bool av1_ok = (preference == CODEC_AV1 || preference == CODEC_AUTO) && test_family(CODEC_FAMILY_AV1);

	if (preference == CODEC_AV1 && av1_ok)
		supported_mask |= VIDEO_FORMAT_MASK_AV1;
	else if (preference == CODEC_H265 && hevc_ok)
		supported_mask |= VIDEO_FORMAT_MASK_H265;
	else if (h264_ok)
		supported_mask |= VIDEO_FORMAT_MASK_H264;

	if (preference == CODEC_AUTO) {
		if (h264_ok)
			supported_mask |= VIDEO_FORMAT_MASK_H264;
		if (hevc_ok)
			supported_mask |= VIDEO_FORMAT_MASK_H265;
		if (av1_ok)
			supported_mask |= VIDEO_FORMAT_MASK_AV1;
	}
	if (supported_mask == 0)
		supported_mask = VIDEO_FORMAT_MASK_H264;
	return supported_mask;
}

Vector<String> MoonlightStreamCore::_get_candidate_decoders(int codec_family) {
	Vector<String> candidates;
#if defined(__ANDROID__)
	// Android 必须优先尝试 mediacodec
	if (codec_family == CODEC_FAMILY_H264) {
		// Prefer low-latency specific MediaCodec components if available
		Vector<String> codec_names;
		// JNI helper: collect MediaCodec component names
		{
			JNIEnv *env = GetJNIEnv();
			if (env) {
				jclass cls = env->FindClass("android/media/MediaCodecList");
				if (cls) {
					jmethodID mid = env->GetStaticMethodID(cls, "getCodecInfos", "()[Landroid/media/MediaCodecInfo;");
					if (mid) {
						jobjectArray arr = (jobjectArray)env->CallStaticObjectMethod(cls, mid);
						if (arr) {
							jsize len = env->GetArrayLength(arr);
							for (jsize i = 0; i < len; i++) {
								jobject info = env->GetObjectArrayElement(arr, i);
								if (!info)
									continue;
								jclass infoCls = env->GetObjectClass(info);
								jmethodID nameMid = env->GetMethodID(infoCls, "getName", "()Ljava/lang/String;");
								if (nameMid) {
									jstring jname = (jstring)env->CallObjectMethod(info, nameMid);
									if (jname) {
										const char *cname = env->GetStringUTFChars(jname, nullptr);
										if (cname) {
											codec_names.push_back(String(cname));
											env->ReleaseStringUTFChars(jname, cname);
										}
										env->DeleteLocalRef(jname);
									}
								}
								env->DeleteLocalRef(info);
							}
						}
					}
					env->DeleteLocalRef(cls);
				}
			}
			// Search for low_latency substrings
			for (int i = 0; i < codec_names.size(); i++) {
				String kn = codec_names[i].to_lower();
				if (kn.find("low_latency") != -1 || kn.find("low-latency") != -1) {
					// Push a special candidate that encodes the component name
					candidates.push_back("h264_mediacodec_lowlat:" + codec_names[i]);
				}
			}
			// Always add generic fallback
			candidates.push_back("h264_mediacodec");
		}
		else if (codec_family == CODEC_FAMILY_H265) {
			candidates.push_back("hevc_mediacodec");
		}
		else if (codec_family == CODEC_FAMILY_AV1) {
			candidates.push_back("av1_mediacodec");
		}
#endif
	// 软件解码器作为后备
	if (codec_family == CODEC_FAMILY_H264)
		candidates.push_back("h264");
	else if (codec_family == CODEC_FAMILY_H265)
		candidates.push_back("hevc");
	else if (codec_family == CODEC_FAMILY_AV1) {
		candidates.push_back("libdav1d");
		candidates.push_back("av1");
	}
	return candidates;
}

AVPixelFormat MoonlightStreamCore::_get_hw_format_callback(AVCodecContext *ctx, const AVPixelFormat *pix_fmts) {
	if (singleton_instance && singleton_instance->hw_pix_fmt != AV_PIX_FMT_NONE) {
		for (const AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
			if (*p == singleton_instance->hw_pix_fmt)
				return *p;
		}
	}
	return avcodec_default_get_format(ctx, pix_fmts);
}

int MoonlightStreamCore::_try_open_decoder(const String &codec_name, int width, int height, AVHWDeviceType hw_type) {
	// Support special candidate names that include a ':' followed by a platform-specific component name
	// e.g. "h264_mediacodec_lowlat:c2.qti.hevc.decoder.low_latency"
	String base_name = codec_name;
	int sep = codec_name.find(":");
	if (sep != -1) {
		base_name = codec_name.substr(0, sep);
	}
	const AVCodec *codec = avcodec_find_decoder_by_name(base_name.utf8().get_data());
	if (!codec)
		return -1;
	// 检查硬件设备是否被libavutil构建支持
	if (hw_type != AV_HWDEVICE_TYPE_NONE) {
		if (av_hwdevice_find_type_by_name(av_hwdevice_get_type_name(hw_type)) == AV_HWDEVICE_TYPE_NONE) {
			// 硬件设备未编译或未找到
			return -1;
		}
	}
	AVCodecContext *ctx = avcodec_alloc_context3(codec);
	if (!ctx)
		return -1;
	ctx->width = width;
	ctx->height = height;
	ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

	// 极致低延迟选项：设置内部 delay 为 0，防止帧缓存
	ctx->delay = 0;

	// 对UDP流至关重要。
	// 注意：Android MediaCodec (Buffer Mode) 往往不支持 OUTPUT_CORRUPT 标志，会导致 avcodec_open2 失败 (Error -22 / EINVAL)
	// 或者导致解码器处于错误状态。我们在 Android 上禁用它。
	if (codec_name.find("_mediacodec") == -1) {
		ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
	}

	ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
	ctx->flags2 |= AV_CODEC_FLAG2_FAST; // 允许非规范兼容的加速
	// 报告解码错误以便我们请求关键帧
	ctx->err_recognition = AV_EF_EXPLODE;

	// 修复：移除强制 NV12 的逻辑。让 FFmpeg 自动协商 MediaCodec 的最佳输出格式。
	// 强制格式可能导致 H.264 解码器初始化失败或输出绿屏（因为实际输出并非 NV12）。

	bool enforce_sw_pix_fmt = hw_type == AV_HWDEVICE_TYPE_NONE &&
			codec_name.find("av1") == -1 && codec_name.find("dav1d") == -1 &&
			codec_name.find("_mediacodec") == -1; // 不要强制 MediaCodec 使用 YUV420P，它通常输出 NV12
	if (enforce_sw_pix_fmt) {
		// Require YUV420P for SW decoders so our shader can handle it
		ctx->pix_fmt = AV_PIX_FMT_YUV420P;
	}
	// 硬件加速设置
	hw_pix_fmt = AV_PIX_FMT_NONE;
	if (hw_type != AV_HWDEVICE_TYPE_NONE) {
		int err = av_hwdevice_ctx_create(&hw_device_ctx, hw_type, nullptr, nullptr, 0);
		if (err < 0) {
			// 硬件初始化失败
			avcodec_free_context(&ctx);
			return -1;
		}
		ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
		ctx->get_format = _get_hw_format_callback;
		// 找到此硬件类型对应的像素格式
		if (hw_type == AV_HWDEVICE_TYPE_D3D11VA)
			hw_pix_fmt = AV_PIX_FMT_D3D11;
		else if (hw_type == AV_HWDEVICE_TYPE_DXVA2)
			hw_pix_fmt = AV_PIX_FMT_DXVA2_VLD;
		else if (hw_type == AV_HWDEVICE_TYPE_VAAPI)
			hw_pix_fmt = AV_PIX_FMT_VAAPI;
		else if (hw_type == AV_HWDEVICE_TYPE_CUDA)
			hw_pix_fmt = AV_PIX_FMT_CUDA;
		else if (hw_type == AV_HWDEVICE_TYPE_QSV)
			hw_pix_fmt = AV_PIX_FMT_QSV;
		else if (hw_type == AV_HWDEVICE_TYPE_VIDEOTOOLBOX)
			hw_pix_fmt = AV_PIX_FMT_VIDEOTOOLBOX;
		else if (hw_type == AV_HWDEVICE_TYPE_MEDIACODEC)
			hw_pix_fmt = AV_PIX_FMT_MEDIACODEC;
		else if (hw_type == AV_HWDEVICE_TYPE_VULKAN)
			hw_pix_fmt = AV_PIX_FMT_VULKAN;
		UtilityFunctions::print(LOG_PREFIX "Attempting HW Decoder: ", codec_name, " Type: ", av_hwdevice_get_type_name(hw_type));
	} else {
		// Android MediaCodec buffer mode 也会走进这里 (hw_type == NONE)
		UtilityFunctions::print(LOG_PREFIX "Attempting SW/Buffer Decoder: ", codec_name);
	}
	int thread_count = OS::get_singleton()->get_processor_count() - 1;
	if (thread_count < 1)
		thread_count = 1;

	// 优化：线程延迟处理。
	// 对于所有的硬件加速模式，强制使用 1 个线程解码。
	// 多线程硬解不仅不能减小单帧延迟，反而经常因为线程管理和同步机制引入 16-33ms 的额外缓冲。
	if (hw_type != AV_HWDEVICE_TYPE_NONE) {
		ctx->thread_count = 1;
		ctx->thread_type = 0; // 禁用帧并行解码（会引入延迟）
	} else {
		// 软解仅使用 Slice 线程以保持低延迟
		if (codec->capabilities & AV_CODEC_CAP_SLICE_THREADS) {
			ctx->thread_type = FF_THREAD_SLICE;
			ctx->thread_count = thread_count;
		} else {
			ctx->thread_count = 1;
		}
	}

	AVDictionary *opts = nullptr;
	// If codec_name encodes a specific mediacodec component (format: name_lowlat:ComponentName), pass it to FFmpeg
	String special_component;
	// reuse earlier `sep` and `base_name` variables to avoid redeclaration
	if (sep != -1) {
		special_component = codec_name.substr(sep + 1, codec_name.length() - (sep + 1));
	}
	if (special_component != String()) {
		// We map to an AVDictionary option for mediacodec component selection. Key name may vary by FFmpeg build;
		// common option used in builds exposing MediaCodec choice is "mediacodec_name".
		av_dict_set(&opts, "mediacodec_name", special_component.utf8().get_data(), 0);
	}

	if (avcodec_open2(ctx, codec, &opts) < 0) {
		// 如果已打开，则清理硬件上下文
		if (hw_device_ctx) {
			av_buffer_unref(&hw_device_ctx);
			hw_device_ctx = nullptr;
		}
		if (opts)
			av_dict_free(&opts);
		avcodec_free_context(&ctx);
		return -1;
	}
	if (opts)
		av_dict_free(&opts);
	v_codec = codec;
	v_codec_ctx = ctx;
	return 0;
}

int MoonlightStreamCore::_handle_dr_setup(int video_fmt, int width, int height) {
	codec_mutex->lock(); // 锁定
	_cleanup_ffmpeg_video();
	UtilityFunctions::print(LOG_PREFIX "Setup Video: Fmt=0x", String::num_int64(video_fmt, 16), " Size=", width, "x", height);
	int family = -1;
	if (video_fmt & VIDEO_FORMAT_MASK_H264)
		family = CODEC_FAMILY_H264;
	else if (video_fmt & VIDEO_FORMAT_MASK_H265)
		family = CODEC_FAMILY_H265;
	else if (video_fmt & VIDEO_FORMAT_MASK_AV1)
		family = CODEC_FAMILY_AV1;
	if (family == -1) {
		codec_mutex->unlock();
		return -1;
	}
	Vector<String> candidates = _get_candidate_decoders(family);
	Vector<AVHWDeviceType> hw_devices;
	if (!disable_hw_decoding) {
		hw_devices = _get_supported_hw_devices();
	}
	hw_devices.push_back(AV_HWDEVICE_TYPE_NONE); // 回退到SW
	bool opened = false;
	String opened_name = "";
	String opened_hw = "Software";
	for (int i = 0; i < candidates.size(); i++) {
		// 修复手动禁用硬件解码无效的问题：在 setup 阶段跳过 mediacodec
		if (disable_hw_decoding && candidates[i].find("_mediacodec") != -1) {
			continue;
		}
		for (int j = 0; j < hw_devices.size(); j++) {
			if (_try_open_decoder(candidates[i], width, height, hw_devices[j]) == 0) {
				opened_name = candidates[i];
				if (hw_devices[j] != AV_HWDEVICE_TYPE_NONE) {
					opened_hw = String(av_hwdevice_get_type_name(hw_devices[j]));
				} else if (candidates[i].find("_mediacodec") != -1) {
					opened_hw = "MediaCodec (Buffer)";
				}
				opened = true;
				break;
			}
		}
		if (opened)
			break;
	}
	if (!opened) {
		UtilityFunctions::printerr(LOG_PREFIX "No usable decoder found!");
		call_deferred("emit_signal", "warning_message", "INIT_ERROR", "Failed to initialize any decoder");
		codec_mutex->unlock(); // 失败时解锁
		return -1;
	}
	UtilityFunctions::print(LOG_PREFIX "Initialized FFmpeg Decoder: ", opened_name, " (", opened_hw, ")");
	call_deferred("emit_signal", "log_message", "Decoder initialized: " + opened_name + " (" + opened_hw + ")");
	if (v_codec_ctx) {
		AVPixelFormat fmt = v_codec_ctx->pix_fmt;
		// Determine bit depth and HDR
		// Note: v_codec_ctx->pix_fmt might be default here until first frame for HW decoders
		// But for SW decoders it should be set.
		// We optimistically setup shader. If invalid format, _setup_shader_integration will log and fail gracefully.
		int bit_depth = 8;
		// Use negotiate color range. If UNSPECIFIED, usually Limited (0) for video.
		AVColorRange range = v_codec_ctx->color_range;
		_setup_shader_integration(width, height, fmt, AVCOL_SPC_BT709, range, bit_depth);
	}
	video_width = width;
	video_height = height;
	// video_format = -1; // Removed legacy SWS format tracking
	codec_mutex->unlock(); // 成功时解锁
	return DR_OK;
}

int MoonlightStreamCore::_handle_dr_submit_decode_unit(PDECODE_UNIT decode_unit) {
	if (!is_streaming.load())
		return DR_OK;
	// 创建数据包
	AVPacket *pkt = av_packet_alloc();
	bool packet_ready = false;
	if (pkt && av_new_packet(pkt, decode_unit->fullLength) >= 0) {
		int offset = 0;
		PLENTRY entry = decode_unit->bufferList;
		while (entry != nullptr) {
			if (entry->length > 0 && entry->data) {
				memcpy(pkt->data + offset, entry->data, entry->length);
				offset += entry->length;
			}
			entry = entry->next;
		}
		packet_ready = true;
	}
	if (packet_ready) {
		queue_mutex->lock();
		int qsize = packet_queue.size();
		// 软解压力过大时清理队列并请求 IDR
		if (!is_hw_decode_active && qsize > 128) {
			while (packet_queue.size() > 0) {
				AVPacket *old = packet_queue.front()->get();
				packet_queue.pop_front();
				av_packet_free(&old);
			}
			queue_mutex->unlock();
			av_packet_free(&pkt);
			_request_idr_frame("SW Queue Flush");
			return DR_OK;
		}
		// 增大队列允许的最大长度至 512，防止在稍微的网络抖动或CPU瞬时高负载时立即硬件级丢包造成花屏
		if (qsize < 512) {
			packet_queue.push_back(pkt);
			queue_mutex->unlock();
			decode_sem->post();
			return DR_OK;
		} else {
			queue_mutex->unlock();
			// 限制因溢出导致的日志泛滥
			static uint64_t last_log = 0;
			uint64_t now = Time::get_singleton()->get_ticks_msec();
			if (now - last_log > 1000) {
				UtilityFunctions::printerr(LOG_PREFIX "Dropping frame due to slow decoder (Queue > 512)");
				last_log = now;
			}
			av_packet_free(&pkt);
			// 丢帧但不请求 IDR，避免循环请求
			return DR_OK;
		}
	} else {
		if (pkt)
			av_packet_free(&pkt);
		return DR_OK;
	}
}

void MoonlightStreamCore::_cleanup_ffmpeg_video() {
	if (hw_device_ctx) {
		av_buffer_unref(&hw_device_ctx);
		hw_device_ctx = nullptr;
	}
	hw_pix_fmt = AV_PIX_FMT_NONE;

	// Removed sws_ctx cleanup as it is removed
	if (v_codec_ctx) {
		avcodec_free_context(&v_codec_ctx);
		v_codec_ctx = nullptr;
	}
}

// New helper to fill image with color (used for black initialization)
static void fill_image_color(Ref<Image> img, Color color) {
	if (img.is_null())
		return;
	// Simple fill for L8/RG8 using fill pattern
	// Note: Image::fill(Color) works but maps mapping might be tricky for L8.
	// L8 maps R channel. RG8 maps RG.
	// For performance and correctness:
	// Y (L8) Black: 0.0 -> 0 byte
	// UV (RG8) Grey: 0.5, 0.5 -> 128, 128 bytes

	// Since fill(Color) is available in Godot 4, we use it.
	img->fill(color);
}

void MoonlightStreamCore::_setup_shader_integration(int width, int height, AVPixelFormat format, AVColorSpace colorspace, AVColorRange color_range, int bit_depth) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs) {
		rs->call_on_render_thread(callable_mp(this, &MoonlightStreamCore::_render_thread_setup_shader).bind(width, height, (int)format, (int)colorspace, (int)color_range, bit_depth));
	}
}

void MoonlightStreamCore::_render_thread_setup_shader(int width, int height, int format, int colorspace, int color_range, int bit_depth) {
	texture_mutex->lock();
	AVPixelFormat av_format = (AVPixelFormat)format;
	AVColorSpace av_colorspace = (AVColorSpace)colorspace;
	AVColorRange av_color_range = (AVColorRange)color_range;

	// 1. Cleanup existing resources if any (for resolution/format changes)
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rd) {
		for (int i = 0; i < 3; i++) {
			rd_texture_wrappers[i].unref();
			if (rs_texture_rid[i].is_valid()) {
				rs->free_rid(rs_texture_rid[i]);
				rs_texture_rid[i] = RID();
			}
			if (rd_texture_rid[i].is_valid()) {
				rd->free_rid(rd_texture_rid[i]);
				rd_texture_rid[i] = RID();
			}
		}
	}

	// 2. Format Detection
	bool is_nv12 = (av_format == AV_PIX_FMT_NV12);
	bool is_yuv420p = (av_format == AV_PIX_FMT_YUV420P);

	if (!is_nv12 && !is_yuv420p) {
		if (av_format == AV_PIX_FMT_NONE) {
			if (hw_device_ctx)
				is_nv12 = true;
			else
				is_yuv420p = true;
		} else {
			use_shader_conversion = false;
			UtilityFunctions::print(LOG_PREFIX "Format not supported by internal shader (", av_get_pix_fmt_name(av_format), "), shader path disabled.");
			texture_mutex->unlock();
			return;
		}
	}

	UtilityFunctions::print(LOG_PREFIX "Initializing Godot Shader Video Pipeline. Format: ", is_nv12 ? "NV12" : "YUV420P");
	use_shader_conversion = true;

	int y_w = width;
	int y_h = height;
	int uv_w = width / 2;
	int uv_h = height / 2;

	// Check for RenderingDevice availability (Fast Path)
	rd = rs->get_rendering_device();
	if (rd) {
		UtilityFunctions::print(LOG_PREFIX "Using RenderingDevice for video textures acceleration.");

		auto create_rd_texture = [&](int idx, int w, int h, RenderingDevice::DataFormat fmt) {
			Ref<RDTextureFormat> tf;
			tf.instantiate();
			tf->set_width(w);
			tf->set_height(h);
			tf->set_depth(1);
			tf->set_array_layers(1);
			tf->set_format(fmt);
			tf->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
			tf->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);

			Ref<RDTextureView> tv;
			tv.instantiate();

			PackedByteArray data;
			data.resize(w * h * (fmt == RenderingDevice::DATA_FORMAT_R8G8_UNORM ? 2 : 1));

			// Neutral YUV
			if (fmt == RenderingDevice::DATA_FORMAT_R8G8_UNORM || idx > 0) {
				data.fill(128);
			} else {
				data.fill(0);
			}

			TypedArray<PackedByteArray> data_array;
			data_array.push_back(data);

			rd_texture_rid[idx] = rd->texture_create(tf, tv, data_array);

			// Create a High-level Texture RID from the RD RID (More stable for Sampling)
			rs_texture_rid[idx] = rs->texture_rd_create(rd_texture_rid[idx]);

			// Create wrapper
			if (rd_texture_wrappers[idx].is_null()) {
				rd_texture_wrappers[idx].instantiate();
			}
			rd_texture_wrappers[idx]->set_texture_rd_rid(rd_texture_rid[idx]);
		};

		// Planar init
		create_rd_texture(0, y_w, y_h, RenderingDevice::DATA_FORMAT_R8_UNORM);
		if (is_nv12) {
			// Some drivers interpret interleaved RG differently; to avoid sampling ambiguity
			// we deinterleave NV12 into two R8 planes (U and V) and upload them separately.
			create_rd_texture(1, uv_w, uv_h, RenderingDevice::DATA_FORMAT_R8_UNORM); // U
			create_rd_texture(2, uv_w, uv_h, RenderingDevice::DATA_FORMAT_R8_UNORM); // V
		} else {
			create_rd_texture(1, uv_w, uv_h, RenderingDevice::DATA_FORMAT_R8_UNORM);
			create_rd_texture(2, uv_w, uv_h, RenderingDevice::DATA_FORMAT_R8_UNORM);
		}
	} else {
		// Fallback to ImageTexture
		plane_images[0] = Image::create(y_w, y_h, false, Image::FORMAT_L8);
		fill_image_color(plane_images[0], Color(0, 0, 0));
		plane_textures[0] = ImageTexture::create_from_image(plane_images[0]);

		if (is_nv12) {
			plane_images[1] = Image::create(uv_w, uv_h, false, Image::FORMAT_RG8);
			fill_image_color(plane_images[1], Color(0.5, 0.5, 0.5));
			plane_textures[1] = ImageTexture::create_from_image(plane_images[1]);
			// Also init plane 2 to avoid null
			plane_images[2] = Image::create(uv_w, uv_h, false, Image::FORMAT_L8);
			fill_image_color(plane_images[2], Color(0.5, 0.5, 0.5));
			plane_textures[2] = ImageTexture::create_from_image(plane_images[2]);
		} else {
			plane_images[1] = Image::create(uv_w, uv_h, false, Image::FORMAT_L8);
			fill_image_color(plane_images[1], Color(0.5, 0.5, 0.5));
			plane_textures[1] = ImageTexture::create_from_image(plane_images[1]);
			plane_images[2] = Image::create(uv_w, uv_h, false, Image::FORMAT_L8);
			fill_image_color(plane_images[2], Color(0.5, 0.5, 0.5));
			plane_textures[2] = ImageTexture::create_from_image(plane_images[2]);
		}
	}

	// Create Material
	if (yuv_shader.is_null()) {
		yuv_shader.instantiate();
		yuv_shader->set_code(YUV_SHADER_CODE);
	}
	if (shader_material.is_null()) {
		shader_material.instantiate();
		shader_material->set_shader(yuv_shader);
	}

	// Infer matrix
	int matrix_type = 1; // Default BT.709
	if (av_colorspace == AVCOL_SPC_BT470BG || av_colorspace == AVCOL_SPC_SMPTE170M) {
		matrix_type = 0;
	} else if (av_colorspace == AVCOL_SPC_BT2020_NCL || av_colorspace == AVCOL_SPC_BT2020_CL) {
		matrix_type = 2;
	} else if (width < 1280 && height < 720) {
		matrix_type = 0;
	}

	// Determine Color Range
	int range_val = (av_color_range == AVCOL_RANGE_JPEG) ? 1 : 0;

	// Assign parameters
	RID mat_rid = shader_material->get_rid();
	// If we are using RD fast path and NV12, we deinterleave into planar U/V, so shader should treat as planar
	bool shader_semi = is_nv12;
	if (rd && is_nv12)
		shader_semi = false;
	rs->material_set_param(mat_rid, "is_semi_planar", shader_semi);
	rs->material_set_param(mat_rid, "color_matrix_type", matrix_type);
	rs->material_set_param(mat_rid, "color_range", range_val);
	// Fix for some hardware (AMD/Intel) presenting NV12 as NV21 or vice versa causing Red->Green artifacts
	rs->material_set_param(mat_rid, "swap_uv", false);
	rs->material_set_param(mat_rid, "channel_order", 0);

	if (rd) {
		// Use set_shader_parameter on the Resource object to ensure high-level Inspector visibility
		// and prevent overwrites. Texture2D types (rd_texture_wrappers) must be passed, not RIDs.
		if (shader_material.is_valid()) {
			shader_material->set_shader_parameter("is_semi_planar", shader_semi);
			shader_material->set_shader_parameter("color_matrix_type", matrix_type);
			shader_material->set_shader_parameter("color_range", range_val);
			shader_material->set_shader_parameter("swap_uv", false);

			shader_material->set_shader_parameter("tex_y", rd_texture_wrappers[0]);
			shader_material->set_shader_parameter("tex_u", rd_texture_wrappers[1]);
			shader_material->set_shader_parameter("tex_v", rd_texture_wrappers[2]);
		} else {
			rs->material_set_param(mat_rid, "tex_y", rs_texture_rid[0]);
			rs->material_set_param(mat_rid, "tex_u", rs_texture_rid[1]);
			rs->material_set_param(mat_rid, "tex_v", rs_texture_rid[2]);
		}
	} else {
		if (shader_material.is_valid()) {
			shader_material->set_shader_parameter("is_semi_planar", is_nv12);
			shader_material->set_shader_parameter("color_matrix_type", matrix_type);
			shader_material->set_shader_parameter("color_range", range_val);
			shader_material->set_shader_parameter("swap_uv", false);

			shader_material->set_shader_parameter("tex_y", plane_textures[0]);
			shader_material->set_shader_parameter("tex_u", plane_textures[1]);
			shader_material->set_shader_parameter("tex_v", plane_textures[2]);
		} else {
			rs->material_set_param(mat_rid, "tex_y", plane_textures[0]->get_rid());
			rs->material_set_param(mat_rid, "tex_u", plane_textures[1]->get_rid());
			rs->material_set_param(mat_rid, "tex_v", plane_textures[2]->get_rid());
		}
	}

	// Apply to Target
	if (display_rect) {
		display_rect->call_deferred("set_material", shader_material);
		if (rd) {
			display_rect->call_deferred("set_texture", rd_texture_wrappers[0]);
		} else if (plane_textures[0].is_valid()) {
			display_rect->call_deferred("set_texture", plane_textures[0]);
		}
	}

	texture_mutex->unlock();
}

void MoonlightStreamCore::_render_thread_cleanup_resources() {
	texture_mutex->lock();
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rd) {
		for (int i = 0; i < 3; i++) {
			rd_texture_wrappers[i].unref();
			if (rs_texture_rid[i].is_valid()) {
				rs->free_rid(rs_texture_rid[i]);
				rs_texture_rid[i] = RID();
			}
			if (rd_texture_rid[i].is_valid()) {
				rd->free_rid(rd_texture_rid[i]);
				rd_texture_rid[i] = RID();
			}
		}
		rd = nullptr;
	}
	texture_mutex->unlock();
}

void MoonlightStreamCore::_update_textures_with_frame(AVFrame *frame) {
	if (!frame)
		return;

	// Use RenderingServer for thread-safe updates without memory allocation overhead.
	RenderingServer *rs = RenderingServer::get_singleton();

	if (rd) {
		// RenderingDevice High Performance Path
		texture_mutex->lock();
		auto upload_rd = [&](int idx, int av_idx, int w, int h, int bpp) {
			int src_stride = frame->linesize[av_idx];
			int dst_stride = w * bpp;
			int required_size = dst_stride * h;

			// Resize intermediate buffer (reuse vector)
			if (rd_texture_buffers[idx].size() != required_size) {
				rd_texture_buffers[idx].resize(required_size);
			}

			uint8_t *dst = rd_texture_buffers[idx].ptrw();
			uint8_t *src = frame->data[av_idx];

			if (src_stride == dst_stride) {
				memcpy(dst, src, required_size);
			} else {
				for (int i = 0; i < h; i++) {
					memcpy(dst + i * dst_stride, src + i * src_stride, dst_stride);
				}
			}
		};

		bool is_nv12 = (frame->format == AV_PIX_FMT_NV12);

		// Y Plane
		upload_rd(0, 0, frame->width, frame->height, 1); // R8

		if (is_nv12) {
			// NV12: interleaved UV plane in frame->data[1]. Deinterleave into two R8 buffers (U and V)
			int uv_w = frame->width / 2;
			int uv_h = frame->height / 2;

			int src_stride = frame->linesize[1];
			int dst_stride = uv_w; // 1 byte per pixel per plane
			int required_size = dst_stride * uv_h;

			if (rd_texture_buffers[1].size() != required_size)
				rd_texture_buffers[1].resize(required_size);
			if (rd_texture_buffers[2].size() != required_size)
				rd_texture_buffers[2].resize(required_size);

			uint8_t *dst_u = rd_texture_buffers[1].ptrw();
			uint8_t *dst_v = rd_texture_buffers[2].ptrw();
			uint8_t *src = frame->data[1];

			for (int row = 0; row < uv_h; row++) {
				uint8_t *srow = src + row * src_stride;
				uint8_t *drow_u = dst_u + row * dst_stride;
				uint8_t *drow_v = dst_v + row * dst_stride;
				for (int x = 0; x < uv_w; x++) {
					drow_u[x] = srow[x * 2 + 0];
					drow_v[x] = srow[x * 2 + 1];
				}
			}
		} else {
			// U + V Planes (YUV420P -> R8 each)
			upload_rd(1, 1, frame->width / 2, frame->height / 2, 1);
			upload_rd(2, 2, frame->width / 2, frame->height / 2, 1);
		}

		pending_gpu_update.store(true);
		texture_mutex->unlock();

		// Request update on the render thread to avoid "only be called from render thread" error
		rs->call_on_render_thread(callable_mp(this, &MoonlightStreamCore::_perform_gpu_update));

		return; // RD path complete
	}

	auto upload_plane = [&](int gl_idx, int av_idx, int w, int h, int bpp) {
		// CRITICAL FIX: Ensure image resources still exist and validity check for _texture_2d_update
		// Capture Refs locally to avoid race conditions if stop_play_stream logic changes
		Ref<Image> img = plane_images[gl_idx];
		Ref<ImageTexture> tex = plane_textures[gl_idx];

		if (img.is_null() || img->is_empty())
			return;
		if (tex.is_null())
			return;

		int src_stride = frame->linesize[av_idx];
		int dst_stride = w * bpp;
		int required_size = dst_stride * h;

		// Resize local buffer if needed
		if (plane_buffers[gl_idx].size() != required_size) {
			plane_buffers[gl_idx].resize(required_size);
		}

		uint8_t *dst = plane_buffers[gl_idx].ptrw();
		uint8_t *src = frame->data[av_idx];

		if (src_stride == dst_stride) {
			memcpy(dst, src, required_size);
		} else {
			for (int i = 0; i < h; i++) {
				memcpy(dst + i * dst_stride, src + i * src_stride, dst_stride);
			}
		}

		// Push data to Godot
		img->set_data(w, h, false, (bpp == 2) ? Image::FORMAT_RG8 : Image::FORMAT_L8, plane_buffers[gl_idx]);

		// Double check before sending to RS
		if (img->is_empty())
			return;

		rs->texture_2d_update(tex->get_rid(), img, 0);
	};

	bool is_nv12 = (frame->format == AV_PIX_FMT_NV12);

	// Upload layout based on format detected in setup
	// If frame format mismatches setup (e.g. dynamic format change), this might look weird,
	// but usually format is constant or stream resets.

	// Y Plane
	upload_plane(0, 0, frame->width, frame->height, 1);

	if (is_nv12) {
		// UV Plane (NV12)
		upload_plane(1, 1, frame->width / 2, frame->height / 2, 2);
	} else {
		// U + V Planes (YUV420P)
		upload_plane(1, 1, frame->width / 2, frame->height / 2, 1);
		upload_plane(2, 2, frame->width / 2, frame->height / 2, 1);
	}
}

void MoonlightStreamCore::_perform_gpu_update() {
	if (!rd)
		return;

	texture_mutex->lock();
	if (pending_gpu_update.exchange(false)) {
		// Update Y
		if (rd_texture_rid[0].is_valid()) {
			rd->texture_update(rd_texture_rid[0], 0, rd_texture_buffers[0]);
		}
		// Update U (or UV)
		if (rd_texture_rid[1].is_valid()) {
			rd->texture_update(rd_texture_rid[1], 0, rd_texture_buffers[1]);
		}
		// Update V
		if (rd_texture_rid[2].is_valid()) {
			rd->texture_update(rd_texture_rid[2], 0, rd_texture_buffers[2]);
		}

		if (display_rect) {
			display_rect->call_deferred("queue_redraw");
		}
	}
	texture_mutex->unlock();
}

AVColorSpace MoonlightStreamCore::_resolve_frame_colorspace(AVFrame *frame) const {
	if (!frame)
		return AVCOL_SPC_BT709;

	AVColorSpace declared = (AVColorSpace)frame->colorspace;
	bool hdr_trc = frame->color_trc == AVCOL_TRC_SMPTE2084 || frame->color_trc == AVCOL_TRC_ARIB_STD_B67;
	bool hdr_primaries = frame->color_primaries == AVCOL_PRI_BT2020;
	if (hdr_trc || hdr_primaries) {
		return AVCOL_SPC_BT2020_NCL;
	}

#if defined(_WIN32)
	bool hw_active = hw_device_ctx != nullptr &&
			(hw_pix_fmt == AV_PIX_FMT_D3D11 || hw_pix_fmt == AV_PIX_FMT_DXVA2_VLD || hw_pix_fmt == AV_PIX_FMT_VULKAN);
	if (hw_active && !hdr_trc && !hdr_primaries) {
		return AVCOL_SPC_BT709;
	}
#endif

	if (declared == AVCOL_SPC_UNSPECIFIED || declared == AVCOL_SPC_RGB) {
#if defined(__ANDROID__)
		// Android 强推测：HD/FHD 使用 BT.709，SD 使用 BT.601
		return (frame->width >= 1280 || frame->height >= 720) ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
#else
		return (frame->width <= 1024 && frame->height <= 576) ? AVCOL_SPC_BT470BG : AVCOL_SPC_BT709;
#endif
	}

	return declared;
}

// ============================================================================
// moonlight音频流playback
// ============================================================================
// 修复：在 _handle_ar_init 中，我们应使用 _try_open_decoder 来测试硬件解码器，而不是直接调用 avcodec_open2

AudioStreamPlaybackMoonlight::AudioStreamPlaybackMoonlight() : active(false) {}
AudioStreamPlaybackMoonlight::~AudioStreamPlaybackMoonlight() {}

void AudioStreamPlaybackMoonlight::_start(double p_from_pos) { active = true; }
void AudioStreamPlaybackMoonlight::_stop() { active = false; }
bool AudioStreamPlaybackMoonlight::_is_playing() const { return active; }
int32_t AudioStreamPlaybackMoonlight::_get_loop_count() const { return 0; }
double AudioStreamPlaybackMoonlight::_get_playback_position() const { return 0.0; }
void AudioStreamPlaybackMoonlight::_seek(double p_time) {}

int32_t AudioStreamPlaybackMoonlight::_mix_resampled(AudioFrame *p_buffer, int32_t p_frames) {
	if (!active || base.is_null())
		return 0;
	return base->read_samples(p_buffer, p_frames);
}

float AudioStreamPlaybackMoonlight::_get_stream_sampling_rate() const {
	if (base.is_valid())
		return (float)base->mix_rate;
	return 48000.0f;
}

void AudioStreamPlaybackMoonlight::_bind_methods() {}

// ============================================================================
// moonlight音频流
// ============================================================================
// 修复：在 _handle_ar_init 中，我们应使用 _try_open_decoder 来测试硬件解码器，而不是直接调用 avcodec_open2

AudioStreamMoonlight::AudioStreamMoonlight() : mix_rate(48000) {
	buffer_mutex.instantiate();
	rb_capacity = 48000 * 2 * 0.2; // 200毫秒缓冲
	ring_buffer.resize(rb_capacity);
	ring_buffer.fill(0);
}

Ref<AudioStreamPlayback> AudioStreamMoonlight::_instantiate_playback() const {
	Ref<AudioStreamPlaybackMoonlight> playback;
	playback.instantiate();
	playback->base = Ref<AudioStreamMoonlight>(this);
	return playback;
}

String AudioStreamMoonlight::_get_stream_name() const { return "Moonlight Audio"; }

void AudioStreamMoonlight::push_audio(const float *samples, int count) {
	buffer_mutex->lock();
	int free_space = rb_capacity - rb_used;
	if (count > free_space) {
		int overflow = count - free_space;
		rb_read_pos = (rb_read_pos + overflow) % rb_capacity;
		rb_used -= overflow;
	}
	int first_chunk = MIN(count, rb_capacity - rb_write_pos);
	int second_chunk = count - first_chunk;
	float *ptr = ring_buffer.ptrw();
	memcpy(ptr + rb_write_pos, samples, first_chunk * sizeof(float));
	if (second_chunk > 0)
		memcpy(ptr, samples + first_chunk, second_chunk * sizeof(float));
	rb_write_pos = (rb_write_pos + count) % rb_capacity;
	rb_used += count;
	buffer_mutex->unlock();
}

int AudioStreamMoonlight::read_samples(AudioFrame *dst_buffer, int frame_count) {
	buffer_mutex->lock();
	int available_frames = rb_used / 2;
	int frames_to_read = MIN(frame_count, available_frames);
	const float *ptr = ring_buffer.ptr();
	int current_pos = rb_read_pos;
	for (int i = 0; i < frames_to_read; i++) {
		float l = ptr[current_pos];
		current_pos = (current_pos + 1) % rb_capacity;
		float r = ptr[current_pos];
		current_pos = (current_pos + 1) % rb_capacity;
		dst_buffer[i].left = l;
		dst_buffer[i].right = r;
	}
	rb_read_pos = current_pos;
	rb_used -= (frames_to_read * 2);
	buffer_mutex->unlock();
	// 如不足则填充静音
	if (frames_to_read < frame_count) {
		for (int i = frames_to_read; i < frame_count; i++) {
			dst_buffer[i].left = 0.0f;
			dst_buffer[i].right = 0.0f;
		}
	}
	return frame_count;
}

void AudioStreamMoonlight::clear_buffer() {
	buffer_mutex->lock();
	rb_write_pos = 0;
	rb_read_pos = 0;
	rb_used = 0;
	buffer_mutex->unlock();
}

void AudioStreamMoonlight::_bind_methods() {}

// ============================================================================
// 音频流核心
// ============================================================================
// 修复：在 _handle_ar_init 中，我们应使用 _try_open_decoder 来测试硬件解码器，而不是直接调用 avcodec_open2

int MoonlightStreamCore::_handle_ar_init(int audio_cfg) {
	_cleanup_ffmpeg_audio();
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
	if (!codec) {
		UtilityFunctions::printerr(LOG_PREFIX "Opus decoder not found");
		return -1;
	}

	a_codec_ctx = avcodec_alloc_context3(codec);
	if (!a_codec_ctx)
		return -1;

	// 优化：音频解码延迟设置
	a_codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	// Opus 解码极快，单线程足以应付且抖动更小
	a_codec_ctx->thread_count = 1;

	// 根据协商的 audio_cfg 设置输入声道布局，避免强制立体声
	int in_channels = CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(audio_cfg);
	int in_mask = CHANNEL_MASK_FROM_AUDIO_CONFIGURATION(audio_cfg);
	if (in_channels <= 0)
		in_channels = 2; // 回退立体声
	if (in_mask != 0) {
		if (av_channel_layout_from_mask(&a_codec_ctx->ch_layout, in_mask) < 0) {
			av_channel_layout_default(&a_codec_ctx->ch_layout, in_channels);
		}
	} else {
		av_channel_layout_default(&a_codec_ctx->ch_layout, in_channels);
	}
	a_codec_ctx->sample_rate = 48000;

	if (avcodec_open2(a_codec_ctx, codec, nullptr) < 0) {
		UtilityFunctions::printerr(LOG_PREFIX "Failed to open Opus codec");
		return -1;
	}

	a_frame = av_frame_alloc();
	a_packet = av_packet_alloc();
	swr_ctx = swr_alloc();

	// 输出仍下混为立体声以兼容现有音频管线
	AVChannelLayout out_layout;
	av_channel_layout_default(&out_layout, 2);
	if (in_channels > 2) {
		UtilityFunctions::print(LOG_PREFIX "Downmixing multichannel audio to stereo");
	}

	// 配置重采样：输入（协商声道）-> 输出（Godot 浮点立体声）
	av_opt_set_chlayout(swr_ctx, "in_chlayout", &a_codec_ctx->ch_layout, 0);
	av_opt_set_int(swr_ctx, "in_sample_rate", 48000, 0);
	av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", a_codec_ctx->sample_fmt, 0);

	av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_layout, 0);
	av_opt_set_int(swr_ctx, "out_sample_rate", 48000, 0);
	av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0); // Godot uses Float

	if (swr_init(swr_ctx) < 0) {
		UtilityFunctions::printerr(LOG_PREFIX "Failed to init Audio Resampler");
		return -1;
	}

	UtilityFunctions::print(LOG_PREFIX "Audio Initialized: Opus 48kHz, input channels=", in_channels, " (downmix to stereo)");
	return 0;
}

void MoonlightStreamCore::_handle_ar_decode_and_play_sample(char *data, int len) {
	if (!a_codec_ctx || audio_stream.is_null())
		return;
	// 准备包裹
	av_packet_unref(a_packet);
	if (av_new_packet(a_packet, len) < 0)
		return;
	memcpy(a_packet->data, data, len);

	int ret = avcodec_send_packet(a_codec_ctx, a_packet);
	av_packet_unref(a_packet); // 数据已复制到解码器，取消引用包
	if (ret < 0)
		return;
	while (ret >= 0) {
		ret = avcodec_receive_frame(a_codec_ctx, a_frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0)
			break;
		int max_out_samples = swr_get_out_samples(swr_ctx, a_frame->nb_samples);
		if (max_out_samples > 0) {
			// 对于常见的小帧（约10毫秒 opus = 480 个样本）使用堆栈缓冲区
			// 480 个样本 * 2 通道 * 4 字节 = 约 3.8KB。16KB 已经绰绰有余。
			uint8_t stack_buf[16384];
			float *out_buf = (float *)stack_buf;
			// 2个通道 * float大小 = 每个样本8字节
			bool huge_frame = (size_t)(max_out_samples * 8) > sizeof(stack_buf);
			if (huge_frame)
				out_buf = (float *)av_malloc(max_out_samples * 8);
			int out_samples = swr_convert(swr_ctx, (uint8_t **)&out_buf, max_out_samples, (const uint8_t **)a_frame->data, a_frame->nb_samples);
			if (out_samples > 0) {
				// 推送交错的浮点立体声样本
				audio_stream->push_audio(out_buf, out_samples * 2);
			}
			if (huge_frame)
				av_free(out_buf);
		}
	}
}

void MoonlightStreamCore::_cleanup_ffmpeg_audio() {
	if (swr_ctx) {
		swr_free(&swr_ctx);
		swr_ctx = nullptr;
	}
	if (a_codec_ctx) {
		avcodec_free_context(&a_codec_ctx);
		a_codec_ctx = nullptr;
	}
	if (a_frame) {
		av_frame_free(&a_frame);
		a_frame = nullptr;
	}
	if (a_packet) {
		av_packet_free(&a_packet);
		a_packet = nullptr;
	}
}

// ============================================================================
// 静态回调与绑定
// ============================================================================

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
	UtilityFunctions::print(LOG_PREFIX "Log from lib: ", msg);
	if (singleton_instance) {
		singleton_instance->call_deferred("emit_signal", "log_message", msg);
	}
}

void MoonlightStreamCore::_cl_set_hdr_mode(bool enabled) {
	if (singleton_instance) {
		singleton_instance->_handle_set_hdr_mode(enabled);
	}
}

void MoonlightStreamCore::_handle_set_hdr_mode(bool enabled) {
	Dictionary metadata;
	if (enabled) {
		SS_HDR_METADATA hdr_data;
		if (LiGetHdrMetadata(&hdr_data)) {
			Array primaries_x;
			primaries_x.push_back(hdr_data.displayPrimaries[0].x);
			primaries_x.push_back(hdr_data.displayPrimaries[1].x);
			primaries_x.push_back(hdr_data.displayPrimaries[2].x);

			Array primaries_y;
			primaries_y.push_back(hdr_data.displayPrimaries[0].y);
			primaries_y.push_back(hdr_data.displayPrimaries[1].y);
			primaries_y.push_back(hdr_data.displayPrimaries[2].y);

			metadata["display_primaries_x"] = primaries_x;
			metadata["display_primaries_y"] = primaries_y;
			metadata["white_point_x"] = hdr_data.whitePoint.x;
			metadata["white_point_y"] = hdr_data.whitePoint.y;
			metadata["min_display_luminance"] = hdr_data.minDisplayLuminance;
			metadata["max_display_luminance"] = hdr_data.maxDisplayLuminance;
			metadata["max_content_light_level"] = hdr_data.maxContentLightLevel;
			metadata["max_frame_average_light_level"] = hdr_data.maxFrameAverageLightLevel;
		}
	}
	call_deferred("emit_signal", "hdr_mode_changed", enabled, metadata);
}

String MoonlightStreamCore::_get_error_string(int error_code) {
	switch (error_code) {
		case ML_ERROR_GRACEFUL_TERMINATION:
			return "Connection terminated gracefully";
		case ML_ERROR_NO_VIDEO_TRAFFIC:
			return "Terminating connection due to lack of video traffic";
		case ML_ERROR_NO_VIDEO_FRAME:
			return "No video frame received";
		case ML_ERROR_UNEXPECTED_EARLY_TERMINATION:
			return "Unexpected early termination";
		case ML_ERROR_PROTECTED_CONTENT:
			return "Protected content detected";
		case ML_ERROR_FRAME_CONVERSION:
			return "Frame conversion error";
		default:
			if (error_code > 0)
				return "Connection error: " + String::num_int64(error_code);
			return "Unknown error (" + String::num_int64(error_code) + ")";
	}
}

int MoonlightStreamCore::_dr_setup(int fmt, int w, int h, int rate, void *ctx, int flags) { return ((MoonlightStreamCore *)ctx)->_handle_dr_setup(fmt, w, h); }
void MoonlightStreamCore::_dr_cleanup(void) {
	if (singleton_instance) {
		// 修复：锁定互斥锁以防止与使用该上下文的解码线程发生竞争
		if (singleton_instance->codec_mutex.is_valid()) {
			singleton_instance->codec_mutex->lock();
			singleton_instance->_cleanup_ffmpeg_video();
			singleton_instance->codec_mutex->unlock();
		}
	}
}
int MoonlightStreamCore::_dr_submit_decode_unit(PDECODE_UNIT du) {
	// 修复：为推送模型路由到实例处理程序
	if (singleton_instance)
		return singleton_instance->_handle_dr_submit_decode_unit(du);
	return DR_OK;
}
int MoonlightStreamCore::_ar_init(int cfg, const POPUS_MULTISTREAM_CONFIGURATION opus, void *ctx, int flags) { return ((MoonlightStreamCore *)ctx)->_handle_ar_init(cfg); }
void MoonlightStreamCore::_ar_cleanup(void) {
	if (singleton_instance)
		singleton_instance->_cleanup_ffmpeg_audio();
}
void MoonlightStreamCore::_ar_decode_and_play_sample(char *data, int len) {
	if (singleton_instance)
		singleton_instance->_handle_ar_decode_and_play_sample(data, len);
}

void MoonlightStreamCore::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start_play_stream", "options"), &MoonlightStreamCore::start_play_stream);
	ClassDB::bind_method(D_METHOD("stop_play_stream"), &MoonlightStreamCore::stop_play_stream);
	ClassDB::bind_method(D_METHOD("set_render_target", "texture_rect"), &MoonlightStreamCore::set_render_target);
	ClassDB::bind_method(D_METHOD("reset_render_target"), &MoonlightStreamCore::reset_render_target);
	ClassDB::bind_method(D_METHOD("get_audio_stream"), &MoonlightStreamCore::get_audio_stream);
	ClassDB::bind_method(D_METHOD("reset_audio_stream", "free_stream"), &MoonlightStreamCore::reset_audio_stream, DEFVAL(false));

	// 绑定内部更新方法以进行延迟调用
	ClassDB::bind_method(D_METHOD("_update_display_texture"), &MoonlightStreamCore::_update_display_texture);
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