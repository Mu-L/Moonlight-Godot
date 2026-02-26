#include "stream_core.h"
using namespace godot;

// Moonlight 流核心：音频 

// 音频：Playback implemention

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

// 音频：流与缓冲器实现

AudioStreamMoonlight::AudioStreamMoonlight() : mix_rate(48000) {
	buffer_mutex.instantiate();
	// 以最大支持声道数为基准分配环形缓冲（单位：float）
	rb_capacity = (int)(48000 * AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT * 0.2); // 200ms
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
	// `count` is number of floats (interleaved samples)
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
	int available_frames = 0;
	if (channel_count > 0)
		available_frames = rb_used / channel_count;
	int frames_to_read = MIN(frame_count, available_frames);
	const float *ptr = ring_buffer.ptr();
	int current_pos = rb_read_pos;
	for (int i = 0; i < frames_to_read; i++) {
		float ch0 = ptr[current_pos];
		current_pos = (current_pos + 1) % rb_capacity;
		float ch1 = 0.0f;
		if (channel_count > 1) {
			ch1 = ptr[current_pos];
			current_pos = (current_pos + 1) % rb_capacity;
			// 跳过剩余通道样本以对齐到下一帧
			for (int c = 2; c < channel_count; c++) {
				current_pos = (current_pos + 1) % rb_capacity;
			}
		}
		if (channel_count == 1) {
			dst_buffer[i].left = ch0;
			dst_buffer[i].right = ch0;
		} else {
			dst_buffer[i].left = ch0;
			dst_buffer[i].right = ch1;
		}
	}
	rb_read_pos = current_pos;
	rb_used -= (frames_to_read * channel_count);
	buffer_mutex->unlock();
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

// 音频流核心

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
	swr_ctx_multi = swr_alloc();

	// 输出 downmix 到立体声以兼容现有音频管线
	AVChannelLayout out_layout_downmix;
	av_channel_layout_default(&out_layout_downmix, 2);

	// 配置 downmix swr_ctx：输入（协商声道）-> 输出（立体声浮点）
	av_opt_set_chlayout(swr_ctx, "in_chlayout", &a_codec_ctx->ch_layout, 0);
	av_opt_set_int(swr_ctx, "in_sample_rate", 48000, 0);
	av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", a_codec_ctx->sample_fmt, 0);
	av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_layout_downmix, 0);
	av_opt_set_int(swr_ctx, "out_sample_rate", 48000, 0);
	av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

	// 配置 multichannel swr_ctx_multi：输入（协商声道）-> 输出（原始通道数，浮点）
	av_opt_set_chlayout(swr_ctx_multi, "in_chlayout", &a_codec_ctx->ch_layout, 0);
	av_opt_set_int(swr_ctx_multi, "in_sample_rate", 48000, 0);
	av_opt_set_sample_fmt(swr_ctx_multi, "in_sample_fmt", a_codec_ctx->sample_fmt, 0);
	av_opt_set_chlayout(swr_ctx_multi, "out_chlayout", &a_codec_ctx->ch_layout, 0);
	av_opt_set_int(swr_ctx_multi, "out_sample_rate", 48000, 0);
	av_opt_set_sample_fmt(swr_ctx_multi, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

	if (swr_init(swr_ctx) < 0 || swr_init(swr_ctx_multi) < 0) {
		UtilityFunctions::printerr(LOG_PREFIX "Failed to init Audio Resampler");
		if (swr_ctx) {
			swr_free(&swr_ctx);
			swr_ctx = nullptr;
		}
		if (swr_ctx_multi) {
			swr_free(&swr_ctx_multi);
			swr_ctx_multi = nullptr;
		}
		return -1;
	}

	if (in_channels > 2) {
		UtilityFunctions::print(LOG_PREFIX "Audio Initialized: Opus 48kHz, input channels=", in_channels, " (downmix to stereo + multichannel available)");
	} else {
		UtilityFunctions::print(LOG_PREFIX "Audio Initialized: Opus 48kHz, input channels=", in_channels);
	}
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

		// 如果配置了 multichannel resampler 且请求了按通道流，则生成多通道浮点缓冲并分发到每个单声道 AudioStream
		int in_ch = a_codec_ctx ? a_codec_ctx->ch_layout.nb_channels : 2;
		if (swr_ctx_multi && audio_streams.size() > 0) {
			int max_out_samples_multi = swr_get_out_samples(swr_ctx_multi, a_frame->nb_samples);
			if (max_out_samples_multi > 0) {
				size_t buf_bytes = (size_t)max_out_samples_multi * in_ch * sizeof(float);
				float *multi_buf = (float *)av_malloc(buf_bytes);
				if (multi_buf) {
					int out_samples_multi = swr_convert(swr_ctx_multi, (uint8_t **)&multi_buf, max_out_samples_multi, (const uint8_t **)a_frame->data, a_frame->nb_samples);
					if (out_samples_multi > 0) {
						// 对每个通道提取并推送到对应的单声道流
						for (int ch = 0; ch < in_ch; ch++) {
							if (ch < (int)audio_streams.size() && audio_streams[ch].is_valid()) {
								float *chan_buf = (float *)av_malloc(out_samples_multi * sizeof(float));
								if (!chan_buf)
									continue;
								for (int s = 0; s < out_samples_multi; s++) {
									chan_buf[s] = multi_buf[s * in_ch + ch];
								}
								audio_streams[ch]->push_audio(chan_buf, out_samples_multi);
								av_free(chan_buf);
							}
						}
					}
					av_free(multi_buf);
				}
			}
		}
}

void MoonlightStreamCore::_cleanup_ffmpeg_audio() {
	if (swr_ctx) {
		swr_free(&swr_ctx);
		swr_ctx = nullptr;
	}
	if (swr_ctx_multi) {
		swr_free(&swr_ctx_multi);
		swr_ctx_multi = nullptr;
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

// 静态回调与绑定
int MoonlightStreamCore::_ar_init(int cfg, const POPUS_MULTISTREAM_CONFIGURATION opus, void *ctx, int flags) { return ((MoonlightStreamCore *)ctx)->_handle_ar_init(cfg); }
void MoonlightStreamCore::_ar_cleanup(void) {
	if (singleton_instance)
		singleton_instance->_cleanup_ffmpeg_audio();
}
void MoonlightStreamCore::_ar_decode_and_play_sample(char *data, int len) {
	if (singleton_instance)
		singleton_instance->_handle_ar_decode_and_play_sample(data, len);
}
