#include "stream_core.h"
using namespace godot;

// Moonlight 流核心：视频处理
// FFmpeg 辅助方法和 AVPacket 解码
void MoonlightStreamCore::_request_idr_frame(const String &reason) {
	uint64_t now = Time::get_singleton()->get_ticks_msec();
	// Throttle to once every 2000ms to prevent log spam and network congestion
	if (now - last_idr_time > 2000) {
		if (enable_idr_logs) {
			UtilityFunctions::print(LOG_PREFIX "Requesting IDR: ", reason);
		}
		if (verbose_requests) {
			UtilityFunctions::print(LOG_PREFIX "IDR request reason (verbose): ", reason);
		}
		LiRequestIdrFrame();
		last_idr_time = now;
	}
}

Vector<AVHWDeviceType> MoonlightStreamCore::_get_supported_hw_devices() {
	Vector<AVHWDeviceType> types;
#if defined(__ANDROID__)
	// Android: 返回空列表以优先使用 MediaCodec Buffer 模式
#elif defined(_WIN32)
	types.push_back(AV_HWDEVICE_TYPE_VULKAN);
	types.push_back(AV_HWDEVICE_TYPE_D3D11VA);
	types.push_back(AV_HWDEVICE_TYPE_DXVA2);
	types.push_back(AV_HWDEVICE_TYPE_CUDA);
#elif defined(__APPLE__)
	types.push_back(AV_HWDEVICE_TYPE_VIDEOTOOLBOX);
#elif defined(__linux__)
	types.push_back(AV_HWDEVICE_TYPE_VULKAN);
	types.push_back(AV_HWDEVICE_TYPE_VAAPI);
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
	// Android: prefer MediaCodec variants per codec family
	Vector<String> codec_names;
	// JNI helper: collect MediaCodec component names
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

	String base_codec_name;
	if (codec_family == CODEC_FAMILY_H264) {
		base_codec_name = "h264";
	} else if (codec_family == CODEC_FAMILY_H265) {
		base_codec_name = "hevc";
	} else if (codec_family == CODEC_FAMILY_AV1) {
		base_codec_name = "av1";
	}

	if (!base_codec_name.is_empty()) {
		// Search for low_latency substrings and add candidates prioritizing them
		for (int i = 0; i < codec_names.size(); i++) {
			String kn = codec_names[i].to_lower();
			bool matches_family = false;
			if (codec_family == CODEC_FAMILY_H264 && (kn.find("avc") != -1 || kn.find("h264") != -1)) {
				matches_family = true;
			} else if (codec_family == CODEC_FAMILY_H265 && (kn.find("hevc") != -1 || kn.find("h265") != -1)) {
				matches_family = true;
			} else if (codec_family == CODEC_FAMILY_AV1 && kn.find("av1") != -1) {
				matches_family = true;
			}

			if (matches_family && (kn.find("low_latency") != -1 || kn.find("low-latency") != -1)) {
				candidates.push_back(base_codec_name + "_mediacodec_lowlat:" + codec_names[i]);
			}
		}

		// Generic MediaCodec fallback
		candidates.push_back(base_codec_name + "_mediacodec");

		// Verbose: list discovered MediaCodec component names and generated candidates
		if (this->verbose_decoders) {
			UtilityFunctions::print(LOG_PREFIX "Android MediaCodec components discovered:");
			for (int i = 0; i < codec_names.size(); i++)
				UtilityFunctions::print("  - ", codec_names[i]);
			UtilityFunctions::print(LOG_PREFIX "Generated decoder candidates:");
			for (int i = 0; i < candidates.size(); i++)
				UtilityFunctions::print("  * ", candidates[i]);
		}
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
	MoonlightStreamCore *instance = (MoonlightStreamCore *)ctx->opaque;
	if (instance && instance->hw_pix_fmt != AV_PIX_FMT_NONE) {
		for (const AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
			if (*p == instance->hw_pix_fmt)
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

	// Strip the "_lowlat" suffix if it exists so we can find the base FFmpeg decoder
	if (base_name.ends_with("_lowlat")) {
		base_name = base_name.substr(0, base_name.length() - 7);
	}

	if (this->verbose_decoders) {
		UtilityFunctions::print(LOG_PREFIX "_try_open_decoder base_name: ", base_name, " Original codec_name: ", codec_name);
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
	ctx->opaque = this;
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
		if (this->verbose_decoders) {
			UtilityFunctions::print(LOG_PREFIX "Setting av_dict mediacodec_name=", special_component, " for decoder ", base_name);
		}
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
	std::lock_guard<godot::Mutex> lock(*(codec_mutex.ptr())); // RAII style lock
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
		return -1;
	}
	Vector<String> candidates = _get_candidate_decoders(family);

	if (OS::get_singleton()->is_debug_build() || verbose_decoders) {
		String family_name = (family == CODEC_FAMILY_H264) ? "H.264" : (family == CODEC_FAMILY_H265) ? "H.265 (HEVC)"
																									 : "AV1";
		UtilityFunctions::print(LOG_PREFIX "Available decoders for ", family_name, ":");
		for (int i = 0; i < candidates.size(); i++) {
			UtilityFunctions::print("  - ", candidates[i]);
		}
	}

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
		return -1;
	}

	if (OS::get_singleton()->is_debug_build() || verbose_decoders) {
		UtilityFunctions::print(LOG_PREFIX "Initialized / Selected Decoder: ", opened_name, " (", opened_hw, ")");
	} else if (verbose_decoders) {
		UtilityFunctions::print(LOG_PREFIX "Initialized FFmpeg Decoder: ", opened_name, " (", opened_hw, ")");
	}

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
		{
			std::lock_guard<godot::Mutex> lock(*(queue_mutex.ptr()));
			int qsize = packet_queue.size();
			// 软解压力过大时清理队列并请求 IDR
			if (!is_hw_decode_active && qsize > 128) {
				while (packet_queue.size() > 0) {
					AVPacket *old = packet_queue.front()->get();
					packet_queue.pop_front();
					av_packet_free(&old);
				}
				av_packet_free(&pkt);
				_request_idr_frame("SW Queue Flush");
				return DR_OK;
			}
			// 增大队列允许的最大长度至 512，防止在稍微的网络抖动或CPU瞬时高负载时立即硬件级丢包造成花屏
			if (qsize < 512) {
				packet_queue.push_back(pkt);
				decode_sem->post();
				return DR_OK;
			}
		}
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

// helper to fill image with color (used for black initialization)
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
	std::lock_guard<godot::Mutex> lock(*(texture_mutex.ptr()));
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
			if (verbose_plugin)
				UtilityFunctions::print(LOG_PREFIX "Format not supported by internal shader (", av_get_pix_fmt_name(av_format), "), shader path disabled.");
			return;
		}
	}

	if (verbose_plugin)
		UtilityFunctions::print(LOG_PREFIX "Initializing Godot Shader Video Pipeline. Format: ", is_nv12 ? "NV12" : "YUV420P");
	use_shader_conversion = true;

	int y_w = width;
	int y_h = height;
	int uv_w = width / 2;
	int uv_h = height / 2;

	// Check for RenderingDevice availability (Fast Path)
	rd = rs->get_rendering_device();
	if (rd) {
		if (verbose_plugin)
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
}

void MoonlightStreamCore::_render_thread_cleanup_resources() {
	std::lock_guard<godot::Mutex> lock(*(texture_mutex.ptr()));
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
}

void MoonlightStreamCore::_update_textures_with_frame(AVFrame *frame) {
	if (!frame)
		return;

	// Use RenderingServer for thread-safe updates without memory allocation overhead.
	RenderingServer *rs = RenderingServer::get_singleton();

	if (rd) {
		// RenderingDevice High Performance Path
		{
			std::lock_guard<godot::Mutex> lock(*(texture_mutex.ptr()));
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
		} // end lock_guard

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

	std::lock_guard<godot::Mutex> lock(*(texture_mutex.ptr()));
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
// 线程逻辑
void MoonlightStreamCore::_thread_func_video_decode() {
	if (pending_add_opts.is_valid() && pending_add_opts->get_disable_video()) {
		UtilityFunctions::print(LOG_PREFIX "Video Decode Thread Stopping (Video disabled)");
		goto end_of_thread;
	}
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
			{
				std::lock_guard<godot::Mutex> lock(*(queue_mutex.ptr()));
				queue_size = packet_queue.size();
				if (queue_size > 0) {
					pkt = packet_queue.front()->get();
					packet_queue.pop_front();
				}
			}

			// Exit condition
			if (!is_streaming.load() && pkt == nullptr) {
				UtilityFunctions::print(LOG_PREFIX "Video Decode Thread Stopping (Queue empty)");
				goto end_of_thread;
			}

			if (pkt == nullptr) {
				break;
			}

			{
				std::lock_guard<godot::Mutex> lock(*(codec_mutex.ptr()));
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
				} // end if (v_codec_ctx)
			} // end lock_guard
			av_packet_free(&pkt);
		}
	}
end_of_thread:
	UtilityFunctions::print(LOG_PREFIX "Video Decode Thread Exited");
}

// 静态回调与绑定

void MoonlightStreamCore::_cl_set_hdr_mode(bool enabled) {
	for (MoonlightStreamCore *instance : active_instances) {
		instance->_handle_set_hdr_mode(enabled);
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

int MoonlightStreamCore::_dr_setup(int fmt, int w, int h, int rate, void *ctx, int flags) { return ((MoonlightStreamCore *)ctx)->_handle_dr_setup(fmt, w, h); }
void MoonlightStreamCore::_dr_cleanup(void) {
	for (MoonlightStreamCore *instance : active_instances) {
		// 修复：锁定互斥锁以防止与使用该上下文的解码线程发生竞争
		if (instance->codec_mutex.is_valid()) {
			std::lock_guard<godot::Mutex> lock(*(instance->codec_mutex.ptr()));
			instance->_cleanup_ffmpeg_video();
		}
	}
}
int MoonlightStreamCore::_dr_submit_decode_unit(PDECODE_UNIT du) {
	// 修复：如果没有上下文指针，则遍历 active_instances
	if (du) {
		for (MoonlightStreamCore *instance : active_instances) {
			int ret = instance->_handle_dr_submit_decode_unit(du);
			if (ret != DR_OK)
				return ret;
		}
	}
	return DR_OK;
}

