#include "stream_core.h"

#ifdef __ANDROID__
#include <godot_cpp/core/jni_helper.hpp>
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

#ifdef __ANDROID__
	// 针对 Android 的关键修复：必须向 FFmpeg 提供 JavaVM 才能正常启动 MediaCodec 硬件加速组件并发挥性能
	JavaVM *jvm = godot::JNIRuntime::get_java_vm();
	if (jvm) {
		av_jni_set_java_vm(jvm, nullptr);
		UtilityFunctions::print(LOG_PREFIX "Android JNI JavaVM attached to FFmpeg");
	}
#endif

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
	// 即使 is_streaming 已经为 false，也必须允许清理。当服务器远程取消流时会发生这种情况（错误 - 100）。
	// 如果我们在这里提前返回，LiStopConnection() 永远不会被调用，而 C 库的内部状态（CurrentStage）将保持脏状态，导致下一次启动时出现断言。
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

	// 5. 等待线程停止
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
	reset_render_target();
}

void MoonlightStreamCore::set_render_target(TextureRect *target) {
	display_rect = target;
	if (display_rect) {
		if (display_texture.is_null()) {
			Ref<Image> img = Image::create(1280, 720, false, Image::FORMAT_RGBA8);
			img->fill(Color(0, 0, 0, 1));
			display_texture = ImageTexture::create_from_image(img);
		}
		display_rect->set_texture(display_texture);
	}
}
void MoonlightStreamCore::reset_render_target() { display_rect = nullptr; }
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
	if (!is_streaming.load())
		return;
	if (new_frame_available && texture_mutex.is_valid()) {
		texture_mutex->lock();
		if (last_decoded_image.is_valid() && display_texture.is_valid()) {
			// ImageTexture::update() 需要精确的尺寸/格式匹配。
			int tex_w = display_texture->get_width();
			int tex_h = display_texture->get_height();
			int img_w = last_decoded_image->get_width();
			int img_h = last_decoded_image->get_height();
			if (tex_w != img_w || tex_h != img_h || display_texture->get_format() != last_decoded_image->get_format()) {
				// 如果纹理分辨率有变动，重新设置纹理
				display_texture->set_image(last_decoded_image);
			} else {
				// 否则直接快速更新
				display_texture->update(last_decoded_image);
			}
		}
		new_frame_available = false;
		texture_mutex->unlock();
	}
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
		// 尽可能排空队列，处理信号量发布合并或队列填充速度快于唤醒的情况
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
			// 退出条件：没有正在正常运行的流媒体连接且队列中没有数据
			if (!is_streaming.load() && pkt == nullptr) {
				UtilityFunctions::print(LOG_PREFIX "Video Decode Thread Stopping (Queue empty)");
				goto end_of_thread;
			}
			// 如果没有数据包但仍在流式传输，跳出内循环以再次等待信号量
			if (pkt == nullptr) {
				break;
			}
			codec_mutex->lock(); // 锁：保护 v_codec_ctx 访问
			if (v_codec_ctx) {
				// 极致低延迟：如果队列中积压了超过 2 帧，且当前包是 IDR，则清空解码器并直接从这一帧开始
				if (pkt->flags & AV_PKT_FLAG_KEY && queue_size > 2) {
					avcodec_flush_buffers(v_codec_ctx);
				}

				int ret = avcodec_send_packet(v_codec_ctx, pkt);
				if (ret >= 0) {
					while (true) {
						ret = avcodec_receive_frame(v_codec_ctx, v_frame);
						if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
							break;
						if (ret < 0) {
							UtilityFunctions::printerr(LOG_PREFIX "Decode error: ", ret);
							LiRequestIdrFrame();
							break;
						}

						// Android 16 优化：如果解码后的积压依然超过 1 帧，立即丢弃当前帧以赶上进度
						// 这样可以解决“PPT”感，因为它强制画面同步到最新。
						if (queue_size > 1) {
							continue;
						}

						AVFrame *display_frame = v_frame;

						// 1. 硬件帧下载到 CPU
						if (v_frame->format == hw_pix_fmt && hw_device_ctx) {
							if (!sw_frame)
								sw_frame = av_frame_alloc();
							int err = av_hwframe_transfer_data(sw_frame, v_frame, 0);
							if (err < 0) {
								continue;
							}
							av_frame_copy_props(sw_frame, v_frame);
							display_frame = sw_frame;
						}

						// 2. 确保 SWS 上下文正确初始化
						int w = display_frame->width;
						int h = display_frame->height;
						AVPixelFormat src_fmt = (AVPixelFormat)display_frame->format;

						if (!sws_ctx || video_width != w || video_height != h || video_format != src_fmt) {
							if (sws_ctx)
								sws_freeContext(sws_ctx);

							// 关键优化：Android 上 SWS_BICUBIC 进行彩色空间转换极其吃 CPU，这是造成 1080p“PPT”感和高延迟的核心瓶颈。
							// 针对 Android 强制使用 SWS_FAST_BILINEAR，将宝贵的 CPU 资源留给解码和逻辑处理，确保画面能同步。
							int flags = SWS_BICUBIC | SWS_ACCURATE_RND;
#ifdef __ANDROID__
							flags = SWS_FAST_BILINEAR;
#endif

							sws_ctx = sws_getContext(w, h, src_fmt, w, h, AV_PIX_FMT_RGBA, flags, nullptr, nullptr, nullptr);

							// 设置色彩空间细节（防止偏绿的核心步骤）
							_apply_sws_colorspace(sws_ctx, display_frame);

							video_width = w;
							video_height = h;
							video_format = src_fmt;
						}

						if (sws_ctx) {
							int required_size = av_image_get_buffer_size(AV_PIX_FMT_RGBA, w, h, 1);
							if (decode_buffer.size() != required_size) {
								decode_buffer.resize(required_size);
							}

							// 关键修复：使用 av_image_fill_arrays 来自动计算正确的 linesize 和指针偏移，解决重影问题
							uint8_t *dest_data[4];
							int dest_linesizes[4];
							av_image_fill_arrays(dest_data, dest_linesizes, decode_buffer.ptrw(), AV_PIX_FMT_RGBA, w, h, 1);

							// 执行转换
							sws_scale(sws_ctx, display_frame->data, display_frame->linesize, 0, h, dest_data, dest_linesizes);

							if (texture_mutex.is_valid()) {
								texture_mutex->lock();
								if (last_decoded_image.is_null() || last_decoded_image->get_width() != w || last_decoded_image->get_height() != h) {
									last_decoded_image = Image::create_from_data(w, h, false, Image::FORMAT_RGBA8, decode_buffer);
								} else {
									last_decoded_image->set_data(w, h, false, Image::FORMAT_RGBA8, decode_buffer);
								}
								new_frame_available = true;
								texture_mutex->unlock();
								call_deferred("_update_display_texture");
							}
						}
						// 如果我们使用了中间的SW帧，取消引用它
						if (display_frame == sw_frame) {
							av_frame_unref(sw_frame);
						}
					}
				} else {
					// 发送数据包失败，可能是数据流损坏
					UtilityFunctions::printerr(LOG_PREFIX "Send packet failed: ", ret);
					call_deferred("emit_signal", "warning_message", "PACKET_ERROR", "Failed to send packet to decoder");
					LiRequestIdrFrame();
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

Vector<AVHWDeviceType> MoonlightStreamCore::_get_supported_hw_devices() {
	Vector<AVHWDeviceType> types;
#if defined(__ANDROID__)
	// Android MediaCodec 优化：不提供 HW Context 以进入 Buffer 模式输出 NV12
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
	types.push_back(AV_HWDEVICE_TYPE_CUDA);
#endif
	return types;
}

int MoonlightStreamCore::_probe_video_format(VideoCodecConfig preference) {
	int supported_mask = 0;
	int test_w = 1280;
	int test_h = 720;

	Vector<AVHWDeviceType> hw_devices;
	if (!disable_hw_decoding) {
		hw_devices = _get_supported_hw_devices();
	}
	hw_devices.push_back(AV_HWDEVICE_TYPE_NONE);

	auto test_family = [&](int family) -> bool {
		Vector<String> candidates = _get_candidate_decoders(family);
		for (int i = 0; i < candidates.size(); i++) {
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
	if (codec_family == CODEC_FAMILY_H264)
		candidates.push_back("h264_mediacodec");
	else if (codec_family == CODEC_FAMILY_H265)
		candidates.push_back("hevc_mediacodec");
	else if (codec_family == CODEC_FAMILY_AV1)
		candidates.push_back("av1_mediacodec");
#endif
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
	const AVCodec *codec = avcodec_find_decoder_by_name(codec_name.utf8().get_data());
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
	ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; // 帮助某些驱动减少解析时间

	ctx->delay = 0;

	// Android 16 MediaCodec 专用调试标志
#if defined(__ANDROID__)
	if (codec_name.find("mediacodec") != -1) {
		// 某些版本的 FFmpeg 允许通过 private_data 设置特定的 mediacodec 标志
		av_opt_set_int(ctx->priv_data, "low_delay", 1, 0);
	}
#endif

	// 对UDP流至关重要。
	if (codec_name.find("_mediacodec") == -1) {
		ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
	}

	ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
	ctx->flags2 |= AV_CODEC_FLAG2_FAST; // 允许非规范兼容的加速
	// 报告解码错误以便我们请求关键帧
	ctx->err_recognition = AV_EF_EXPLODE;
	bool enforce_sw_pix_fmt = hw_type == AV_HWDEVICE_TYPE_NONE &&
			codec_name.find("av1") == -1 && codec_name.find("dav1d") == -1 &&
			codec_name.find("_mediacodec") == -1; // 不要强制 MediaCodec 使用 YUV420P，它通常输出 NV12
	if (enforce_sw_pix_fmt) {
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
		// 修复日志混淆：如果是 mediacodec，虽然 hw_type 为 NONE，但它依然是硬件加速
		if (codec_name.find("mediacodec") != -1) {
			UtilityFunctions::print(LOG_PREFIX "Attempting HW Wrapper Decoder: ", codec_name);
		} else {
			UtilityFunctions::print(LOG_PREFIX "Attempting SW Decoder: ", codec_name);
		}
	}
	int thread_count = OS::get_singleton()->get_processor_count() - 1;
	if (thread_count < 1)
		thread_count = 1;

	// 核心低延时修复：强制禁用帧并行解码 (FF_THREAD_FRAME)
	// 帧并行解码会为了利用多核而缓存 N 帧，每核一帧，直接导致 N/FPS 的毫秒延迟。
	if (hw_type != AV_HWDEVICE_TYPE_NONE) {
		ctx->thread_count = 1;
		ctx->thread_type = 0;
	} else {
		if (codec->capabilities & AV_CODEC_CAP_SLICE_THREADS) {
			ctx->thread_type = FF_THREAD_SLICE; // 切片并行不会导致帧级延迟
			ctx->thread_count = thread_count;
		} else {
			ctx->thread_count = 1;
		}
	}

	if (avcodec_open2(ctx, codec, nullptr) < 0) {
		// 如果已打开，则清理硬件上下文
		if (hw_device_ctx) {
			av_buffer_unref(&hw_device_ctx);
			hw_device_ctx = nullptr;
		}
		avcodec_free_context(&ctx);
		return -1;
	}
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
		for (int j = 0; j < hw_devices.size(); j++) {
			if (_try_open_decoder(candidates[i], width, height, hw_devices[j]) == 0) {
				opened_name = candidates[i];
				if (hw_devices[j] != AV_HWDEVICE_TYPE_NONE) {
					opened_hw = String(av_hwdevice_get_type_name(hw_devices[j]));
				} else if (opened_name.find("mediacodec") != -1) {
					// 修正显示：即便没有关联独立 HW Device Context，mediacodec 本质仍是硬件加速路径。
					opened_hw = "MediaCodec";
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
	video_width = width;
	video_height = height;
	video_format = -1; // 在第一帧强制初始化SWS
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
		// 记录帧类型信息
		if (decode_unit->frameType == FRAME_TYPE_IDR) {
			pkt->flags |= AV_PKT_FLAG_KEY;
		}
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
		// 实时流极致丢包逻辑：如果队列中已经有超过 2 个包，丢弃旧包，推送新包。
		// 这样可以移除网络波动导致的帧积压（PPT 效应的主要来源）。
		while (packet_queue.size() >= 2) {
			AVPacket *old_pkt = packet_queue.front()->get();
			packet_queue.pop_front();
			av_packet_free(&old_pkt);
		}

		packet_queue.push_back(pkt);
		queue_mutex->unlock();
		decode_sem->post();
		return DR_OK;
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

	if (sws_ctx) {
		sws_freeContext(sws_ctx);
		sws_ctx = nullptr;
	}
	if (v_codec_ctx) {
		avcodec_free_context(&v_codec_ctx);
		v_codec_ctx = nullptr;
	}
	// 不要在这里释放 v_frame。它在核心实例的整个生命周期中都会存在
}

// 新增：根据帧信息推断色彩空间，供 SWS 配置使用
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
		return (frame->width <= 1024 && frame->height <= 576) ? AVCOL_SPC_BT470BG : AVCOL_SPC_BT709;
	}

	return declared;
}

void MoonlightStreamCore::_apply_sws_colorspace(SwsContext *ctx, AVFrame *frame) {
	if (!ctx || !frame)
		return;

	AVColorSpace src_csp = _resolve_frame_colorspace(frame);
	int src_full_range = (frame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
	int dst_full_range = 1;

#if defined(_WIN32)
	bool hw_dx = hw_device_ctx != nullptr &&
			(hw_pix_fmt == AV_PIX_FMT_D3D11 || hw_pix_fmt == AV_PIX_FMT_DXVA2_VLD);
	if (hw_dx && (frame->format == AV_PIX_FMT_NV12 || frame->format == AV_PIX_FMT_P010LE)) {
		// 强制使用 BT.709 + 限制级范围，避免偏绿
		frame->color_primaries = AVCOL_PRI_BT709;
		frame->color_trc = AVCOL_TRC_BT709;
		frame->color_range = AVCOL_RANGE_MPEG;
		src_full_range = 0;
		src_csp = AVCOL_SPC_BT709;
	}
#endif

	frame->colorspace = src_csp;

	const int *src_mat = sws_getCoefficients(src_csp);
	const int *dst_mat = sws_getCoefficients(AVCOL_SPC_RGB);
	if (!src_mat || !dst_mat)
		return;

	if (sws_setColorspaceDetails(ctx,
				const_cast<int *>(src_mat), src_full_range,
				const_cast<int *>(dst_mat), dst_full_range,
				0, 1 << 16, 1 << 16) < 0) {
		UtilityFunctions::printerr(LOG_PREFIX "Failed to configure SWS colorspace, using defaults");
	}
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