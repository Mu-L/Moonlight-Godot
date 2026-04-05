//
// 此头文件公开了用于客户端使用的公共流 API
//

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 在调试期间启用此定义以启用断言
//#define LC_DEBUG

// 下面 'streamingRemotely' 字段的值
#define STREAM_CFG_LOCAL   0
#define STREAM_CFG_REMOTE  1
#define STREAM_CFG_AUTO    2

// 下面 'colorSpace' 字段的值。
// 在 GFE 主机上，H.264 视频流不支持 Rec. 2020。
#define COLORSPACE_REC_601  0
#define COLORSPACE_REC_709  1
#define COLORSPACE_REC_2020 2

// 下面 'colorRange' 字段的值
#define COLOR_RANGE_LIMITED  0
#define COLOR_RANGE_FULL     1

// 下面 'encryptionFlags' 字段的值
#define ENCFLG_NONE  0x00000000
#define ENCFLG_AUDIO 0x00000001
#define ENCFLG_VIDEO 0x00000002
#define ENCFLG_ALL   0xFFFFFFFF

// 此函数返回一个字符串，您应将其附加到 /launch 和 /resume
// 查询参数字符串的末尾。用于为 Sunshine 主机启用某些扩展功能。
// 返回的字符串归 moonlight-common-c 所有，
// 调用者不应释放它。
const char* LiGetLaunchUrlQueryParameters(void);

typedef struct _STREAM_CONFIGURATION {
    // 所需视频流的像素尺寸
    int width;
    int height;

    // 所需视频流的帧率
    int fps;

    // 所需视频流的比特率（音频额外增加约 1 Mbps）。这包括纠错数据，
    // 因此实际编码器比特率在使用标准 20% FEC 配置时将低约 20%。
    int bitrate;

    // 最大视频包大小（以字节为单位）（如果不确定，请使用 1024）。如果 STREAM_CFG_AUTO
    // 确定流是远程的（见下文），它会将此值上限设为 1024，以避免与 MTU 相关的问题，
    // 如丢包和分片。
    int packetSize;

    // 确定是否启用远程（通过互联网）流式传输优化。如果不确定，请设置为 STREAM_CFG_AUTO。
    // STREAM_CFG_AUTO 使用启发式方法（基于目标地址是否在 RFC 1918 地址块中）来决定流
    // 是否为远程。
    int streamingRemotely;

    // 指定音频流的声道配置。
    // 参见下面的 AUDIO_CONFIGURATION 常量和 MAKE_AUDIO_CONFIGURATION()。
    int audioConfiguration;

    // 指定支持的视频格式的掩码。
    // 参见下面的 VIDEO_FORMAT 常量。
    int supportedVideoFormats;

    // 如果指定，为客户端的显示刷新率乘以 100。例如，
    // 59.94 Hz 应指定为 5994。此参数用于较新版本的 GFE，以实现增强的帧 pacing。
    int clientRefreshRateX100;

    // 如果指定，将编码器色彩空间设置为提供的 COLORSPACE_* 选项（上面列出）。
    // 如果未设置，编码器将默认使用 Rec 601。
    int colorSpace;

    // 如果指定，将编码器色彩范围设置为提供的 COLOR_RANGE_* 选项（上面列出）。
    // 如果未设置，编码器将默认使用 Limited。
    int colorRange;

    // 指定可以由主机 PC 启用加密的数据流。理想情况下，您应传递 ENCFLG_ALL 以加密
    // 我们支持的所有流。然而，性能较低的硬件可能无法支持加密视频或音频等重型数据，
    // 因此可以在此处禁用这些流的加密。远程输入加密始终启用。
    int encryptionFlags;

    // 远程输入流的 AES 加密数据。必须与传递给 /launch 和 /resume 请求中的 rikey 和 rikeyid
    // 相同。
    char remoteInputAesKey[16];
    char remoteInputAesIv[16];
} STREAM_CONFIGURATION, *PSTREAM_CONFIGURATION;

// 使用此函数清零栈或堆上分配的流配置
void LiInitializeStreamConfiguration(PSTREAM_CONFIGURATION streamConfig);

// 这些常量用于标识 H.264 和 HEVC 格式中标识为 IDR 帧的帧的缓冲区列表中的编解码器配置数据。
// 对于其他编解码器，所有数据都标记为 BUFFER_TYPE_PICDATA。
#define BUFFER_TYPE_PICDATA  0x00
#define BUFFER_TYPE_SPS      0x01
#define BUFFER_TYPE_PPS      0x02
#define BUFFER_TYPE_VPS      0x03

typedef struct _LENTRY {
    // 指向下一个条目的指针，如果这是最后一个条目，则为 NULL
    struct _LENTRY* next;

    // 指向数据的指针（永不 NULL）
    char* data;

    // 数据大小（以字节为单位）（永不 <= 0）
    int length;

    // 缓冲区类型（上面列出，仅对 H.264 和 HEVC 格式设置）
    int bufferType;
} LENTRY, *PLENTRY;

// 这是引用 IDR 帧和之前的 P 帧的标准帧。
#define FRAME_TYPE_PFRAME 0x00

// 这是关键帧。
//
// 对于 H.264 和 HEVC，这意味着帧的第一个缓冲区列表包含 SPS、PPS 和 VPS（仅 HEVC）NAL 单元。
// I 帧数据紧跟在这些编解码器配置 NAL 单元之后。
//
// 对于其他编解码器，任何配置数据都不会拆分为单独的缓冲区。
#define FRAME_TYPE_IDR    0x01

// 解码单元描述由多个包组成的视频数据缓冲区链
typedef struct _DECODE_UNIT {
    // 帧编号
    int frameNumber;

    // 帧类型
    int frameType;

    // 可选的帧主机处理延迟，以 1/10 毫秒为单位。
    // 当主机未提供延迟数据或帧处理延迟不适用于当前帧时（当帧重复时发生），此值为零。
    uint16_t frameHostProcessingLatency;

    // 第一个缓冲区的接收时间，以微秒为单位。
    uint64_t receiveTimeUs;

    // 帧完全组装并排队等待视频解码器处理的时间。
    // 这也大致是接收到最后一个包的时间，因此
    // enqueueTimeUs - receiveTimeUs 是接收帧所花费的时间。当解码单元传递给 submitDecodeUnit() 时，
    // 可以计算出总队列延迟。此值以微秒为单位。
    uint64_t enqueueTimeUs;

    // 以第一个捕获帧为原点的显示时间，以微秒为单位。
    // 这可用于辅助帧 pacing 或丢弃在显示前排队过久的旧帧。
    uint64_t presentationTimeUs;

    // 原始 RTP 时间戳，以 90 kHz 为单位。当使用处理整数时间的 API（如 Apple 的 CMTime）时很有用。
    // 要精确恢复 RTP 时间戳，请使用类似 CMTimeMake((int64_t)du->rtpTimestamp, 90000) 的方法。
    uint32_t rtpTimestamp;

    // 整个缓冲区链的长度（以字节为单位）
    int fullLength;

    // 缓冲区链的头指针（永不 NULL）
    PLENTRY bufferList;

    // 确定此帧是 SDR 还是 HDR
    //
    // 注意：当前未从实际比特流中解析此字段，因此如果您的客户端可以访问比特流解析器，
    // 请优先使用解析器而不是此字段。
    bool hdrActive;

    // 提供此帧的色彩空间（见上面的 COLORSPACE_* 定义）
    //
    // 注意：当前未从实际比特流中解析此字段，因此如果您的客户端可以访问比特流解析器，
    // 请优先使用解析器而不是此字段。
    uint8_t colorspace;
} DECODE_UNIT, *PDECODE_UNIT;

// 指定音频流应编码为立体声（默认）
#define AUDIO_CONFIGURATION_STEREO MAKE_AUDIO_CONFIGURATION(2, 0x3)

// 指定如果 PC 支持，音频流应为 5.1 环绕声
#define AUDIO_CONFIGURATION_51_SURROUND MAKE_AUDIO_CONFIGURATION(6, 0x3F)

// 指定如果 PC 支持，音频流应为 7.1 环绕声
#define AUDIO_CONFIGURATION_71_SURROUND MAKE_AUDIO_CONFIGURATION(8, 0x63F)

// 通过声道数和声道掩码指定音频配置
// 有关 channelMask 值，请参见 https://docs.microsoft.com/en-us/windows-hardware/drivers/audio/channel-mask
// 注意：并非所有组合都受 GFE 和/或此库支持。
#define MAKE_AUDIO_CONFIGURATION(channelCount, channelMask) \
    (((channelMask) << 16) | (channelCount << 8) | 0xCA)

// 用于从音频配置中获取声道数和声道掩码的辅助宏
#define CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(x) (((x) >> 8) & 0xFF)
#define CHANNEL_MASK_FROM_AUDIO_CONFIGURATION(x) (((x) >> 16) & 0xFFFF)

// 辅助宏，用于获取启动会话时必须在 /launch 和 /resume HTTPS 请求中传递的 surroundAudioInfo 参数值。
#define SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(x) \
    (CHANNEL_MASK_FROM_AUDIO_CONFIGURATION(x) << 16 | CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(x))

// 支持的最大声道数
#define AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT 8

// 在 StreamConfiguration.supportedVideoFormats 中传递以指定支持的编解码器，
// 并在 DecoderRendererSetup() 中传递以指定所选编解码器。
#define VIDEO_FORMAT_H264            0x0001 // H.264 High Profile
#define VIDEO_FORMAT_H264_HIGH8_444  0x0004 // H.264 High 4:4:4 8-bit Profile
#define VIDEO_FORMAT_H265            0x0100 // HEVC Main Profile
#define VIDEO_FORMAT_H265_MAIN10     0x0200 // HEVC Main10 Profile
#define VIDEO_FORMAT_H265_REXT8_444  0x0400 // HEVC RExt 4:4:4 8-bit Profile
#define VIDEO_FORMAT_H265_REXT10_444 0x0800 // HEVC RExt 4:4:4 10-bit Profile
#define VIDEO_FORMAT_AV1_MAIN8       0x1000 // AV1 Main 8-bit profile
#define VIDEO_FORMAT_AV1_MAIN10      0x2000 // AV1 Main 10-bit profile
#define VIDEO_FORMAT_AV1_HIGH8_444   0x4000 // AV1 High 4:4:4 8-bit profile
#define VIDEO_FORMAT_AV1_HIGH10_444  0x8000 // AV1 High 4:4:4 10-bit profile

// 客户端用于匹配视频编解码器而不考虑 profile 特定细节的掩码。
#define VIDEO_FORMAT_MASK_H264   0x000F
#define VIDEO_FORMAT_MASK_H265   0x0F00
#define VIDEO_FORMAT_MASK_AV1    0xF000
#define VIDEO_FORMAT_MASK_10BIT  0xAA00
#define VIDEO_FORMAT_MASK_YUV444 0xCC04

// 如果在渲染器能力字段中设置，此标志将导致音频/视频数据直接从接收线程提交。
// 仅当渲染器是非阻塞的时才应指定此标志。此标志对音频和视频渲染器均有效。
#define CAPABILITY_DIRECT_SUBMIT 0x1

// 如果在视频渲染器能力字段中设置，此标志指定渲染器支持 AVC/H.264 流的参考帧失效。
// 此标志仅对视频渲染器有效。如果使用此功能，可能不会修补比特流（更改 num_ref_frames 或 max_dec_frame_buffering）
// 以避免丢包时的视频损坏。
#define CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC 0x2

// 如果在视频渲染器能力字段中设置，此标志指定渲染器支持 HEVC/H.265 流的参考帧失效。
// 此标志仅对视频渲染器有效。
#define CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC 0x4

// 如果在音频渲染器能力字段中设置，此标志将导致 RTSP 协商从不请求“高质量”音频预设。
// 如果未设置，当视频流比特率高于 15 Mbps 时，将使用高质量音频。
#define CAPABILITY_SLOW_OPUS_DECODER 0x8

// 如果在音频渲染器能力字段中设置，表示音频包可能包含多于或少于 5 毫秒的音频。
// 这要求音频渲染器读取 OPUS_MULTISTREAM_CONFIGURATION 中的 samplesPerFrame 字段，
// 以计算正确的解码缓冲区大小，而不是始终假设为 240。
#define CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION 0x10

// 此标志使渲染器选择基于拉取的模式，而不是默认的基于推送的回调模式。
// 渲染器必须调用新函数（LiWaitForNextVideoFrame()、LiCompleteVideoFrame() 等）来接收 A/V 数据。
// 设置此能力的同时也提供样本回调是不允许的。
#define CAPABILITY_PULL_RENDERER 0x20

// 如果在视频渲染器能力字段中设置，此标志指定渲染器支持 AV1 流的参考帧失效。
// 此标志仅对视频渲染器有效。
#define CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1 0x40

// 如果在视频渲染器能力字段中设置，此宏指定渲染器支持分片以提高解码性能。
// 参数指定每帧所需的切片数。此能力仅对视频渲染器有效。
#define CAPABILITY_SLICES_PER_FRAME(x) (((unsigned char)(x)) << 24)

// 此回调被调用来提供视频流的详细信息并允许配置解码器。
// 成功返回 0，失败返回非零值。
typedef int(*DecoderRendererSetup)(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags);

// 此回调通知解码器流即将开始。在此回调返回之前不能提交任何帧。
typedef void(*DecoderRendererStart)(void);

// 此回调通知解码器流即将停止。仍可能提交帧，但可以安全地丢弃它们。
typedef void(*DecoderRendererStop)(void);

// 此回调执行视频解码器的清理工作。当此回调被调用时，将不再有帧提交。
typedef void(*DecoderRendererCleanup)(void);


// 此回调向解码器提供 Annex B 格式的基本流数据。
// 如果解码器由于某种原因无法处理提交的数据，它必须返回 DR_NEED_IDR 以生成关键帧。
#define DR_OK 0
#define DR_NEED_IDR -1
typedef int(*DecoderRendererSubmitDecodeUnit)(PDECODE_UNIT decodeUnit);

typedef struct _DECODER_RENDERER_CALLBACKS {
    DecoderRendererSetup setup;
    DecoderRendererStart start;
    DecoderRendererStop stop;
    DecoderRendererCleanup cleanup;
    DecoderRendererSubmitDecodeUnit submitDecodeUnit;
    int capabilities;
} DECODER_RENDERER_CALLBACKS, *PDECODER_RENDERER_CALLBACKS;

// 使用此函数清零栈或堆上分配的视频回调
void LiInitializeVideoCallbacks(PDECODER_RENDERER_CALLBACKS drCallbacks);

// 此结构提供成功解码从计算机发送的音频流所需的 Opus 多流解码器参数。
// 有关这些字段的详细信息，请参阅 opus_multistream_decoder_init 文档。
//
// 提供的映射数组根据以下输出声道顺序进行索引：
// 0 - 左前
// 1 - 右前
// 2 - 中置
// 3 - 低音炮
// 4 - 左后
// 5 - 右后
// 6 - 左侧
// 7 - 右侧
//
// 如果映射顺序与音频渲染器的声道顺序不匹配，您可以交换不匹配索引中的值，直到映射数组与所需的声道顺序匹配。
typedef struct _OPUS_MULTISTREAM_CONFIGURATION {
    int sampleRate;
    int channelCount;
    int streams;
    int coupledStreams;
    int samplesPerFrame;
    unsigned char mapping[AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT];
} OPUS_MULTISTREAM_CONFIGURATION, *POPUS_MULTISTREAM_CONFIGURATION;

// 此回调初始化音频渲染器。audioConfiguration 参数提供协商的音频配置。
// 这可能与流配置中指定的不同。成功返回 0，失败返回非零值。
typedef int(*AudioRendererInit)(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags);

// 此回调通知解码器流即将开始。在此回调返回之前不能提交任何音频。
typedef void(*AudioRendererStart)(void);

// 此回调通知解码器流即将停止。仍可能提交音频样本，但可以安全地丢弃它们。
typedef void(*AudioRendererStop)(void);

// 此回调执行音频解码器的最终清理工作。在此回调被调用后，将不再有音频提交。
typedef void(*AudioRendererCleanup)(void);

// 此回调提供要解码和播放的 Opus 音频数据。sampleLength 以字节为单位。
typedef void(*AudioRendererDecodeAndPlaySample)(char* sampleData, int sampleLength);

typedef struct _AUDIO_RENDERER_CALLBACKS {
    AudioRendererInit init;
    AudioRendererStart start;
    AudioRendererStop stop;
    AudioRendererCleanup cleanup;
    AudioRendererDecodeAndPlaySample decodeAndPlaySample;
    int capabilities;
} AUDIO_RENDERER_CALLBACKS, *PAUDIO_RENDERER_CALLBACKS;

// 使用此函数清零栈或堆上分配的音频回调
void LiInitializeAudioCallbacks(PAUDIO_RENDERER_CALLBACKS arCallbacks);

// 在未来的版本中可能会更改
// 使用 LiGetStageName() 获取稳定的阶段名称
#define STAGE_NONE 0
#define STAGE_PLATFORM_INIT 1
#define STAGE_NAME_RESOLUTION 2
#define STAGE_AUDIO_STREAM_INIT 3
#define STAGE_RTSP_HANDSHAKE 4
#define STAGE_CONTROL_STREAM_INIT 5
#define STAGE_VIDEO_STREAM_INIT 6
#define STAGE_INPUT_STREAM_INIT 7
#define STAGE_CONTROL_STREAM_START 8
#define STAGE_VIDEO_STREAM_START 9
#define STAGE_AUDIO_STREAM_START 10
#define STAGE_INPUT_STREAM_START 11
#define STAGE_MAX 12

// 此回调被调用以指示初始化过程的某个阶段即将开始
typedef void(*ConnListenerStageStarting)(int stage);

// 此回调被调用以指示初始化过程的某个阶段已完成
typedef void(*ConnListenerStageComplete)(int stage);

// 此回调被调用以指示初始化过程的某个阶段已失败。
// 因为连接尚未完全建立，所以不会调用 ConnListenerConnectionTerminated()。
// LiInterruptConnection() 和 LiStopConnection() 可能导致此回调被调用，但不保证。
typedef void(*ConnListenerStageFailed)(int stage, int errorCode);

// 在成功建立连接后调用此回调
typedef void(*ConnListenerConnectionStarted)(void);

// 当连接在建立后被终止时调用此回调。
// 如果终止据报告是服务器有意为之（例如，用户关闭了游戏），则 errorCode 将为 0。
// 如果 errorCode 非零，则意味着终止可能是意外的（网络丢失、崩溃或类似情况）。
// 此回调不会因调用 LiStopConnection() 或 LiInterruptConnection() 而被调用。
typedef void(*ConnListenerConnectionTerminated)(int errorCode);

// 当流被主机正常终止时，此错误码传递给 ConnListenerConnectionTerminated()。
// 通常表示主机 PC 上的应用程序已退出。
#define ML_ERROR_GRACEFUL_TERMINATION 0

// 如果在此连接的等待几秒钟后从未接收到视频数据，则此错误传递给 ConnListenerConnectionTerminated()。
// 可能表示由于防火墙或端口转发规则缺失或不正确，导致 UDP 47998 流量出现问题。
#define ML_ERROR_NO_VIDEO_TRAFFIC -100

// 如果在等待几秒钟后无法接收到完整帧，则此错误传递给 ConnListenerConnectionTerminated()。
// 可能表示连接极其不稳定或比特率过高。
#define ML_ERROR_NO_VIDEO_FRAME -101

// 如果流在启动后不久由于主机的正常终止而结束，则此错误传递给 ConnListenerConnectionTerminated()。
// 通常，如果屏幕上出现受 DRM 保护的内容（在 GFE 3.22 之前），或编码器无法成功捕获视频的其他问题，似乎会发生此情况。
#define ML_ERROR_UNEXPECTED_EARLY_TERMINATION -102

// 如果由于主机的受保护内容错误而结束流，则此错误传递给 ConnListenerConnectionTerminated()。
// 此值在 GFE 3.22+ 上受支持。
#define ML_ERROR_PROTECTED_CONTENT -103

// 如果由于帧转换错误而结束流，则此错误传递给 ConnListenerConnectionTerminated()。
// 最常见的原因是启用 HDR 时桌面分辨率与流式分辨率不兼容。
// 此值在 GFE 3.22+ 上受支持。
#define ML_ERROR_FRAME_CONVERSION -104

// 此回调用于记录调试消息
typedef void(*ConnListenerLogMessage)(const char* format, ...);

// 此回调用于振动游戏手柄。期望此回调中设置的振动效果将持续到将来调用设置不同触觉效果或通过为两个电机传递 0 来关闭电机。
// 可能会接收到针对物理上不存在的游戏手柄的振动事件，因此您的回调应处理这种情况。
typedef void(*ConnListenerRumble)(unsigned short controllerNumber, unsigned short lowFreqMotor, unsigned short highFreqMotor);

// 此回调用于通知客户端连接状态的变化。
// 考虑为用户显示一个叠加层，以通知他们为什么流性能不佳。
#define CONN_STATUS_OKAY    0
#define CONN_STATUS_POOR    1
typedef void(*ConnListenerConnectionStatusUpdate)(int connectionStatus);

// 此回调用于通知客户端主机上 HDR 模式的变化。
// 客户端可能希望更新本地显示模式以匹配主机上的 HDR 状态。
// 即使流未使用支持 HDR 的编解码器，也可能调用此回调。
typedef void(*ConnListenerSetHdrMode)(bool hdrEnabled);

// 此回调用于振动游戏手柄的扳机键。更多详细信息，请参见上面 ConnListenerRumble() 的注释。
typedef void(*ConnListenerRumbleTriggers)(uint16_t controllerNumber, uint16_t leftTriggerMotor, uint16_t rightTriggerMotor);

// 此回调用于通知客户端主机希望以指定的报告速率（或尽可能接近）接收指定游戏手柄的运动传感器报告。
//
// 如果 reportRateHz 为 0，主机要求停止运动事件报告。
typedef void(*ConnListenerSetMotionEventState)(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz);

// 此回调用于通知客户端 DualSense 自适应扳机配置的变化。
#define DS_EFFECT_PAYLOAD_SIZE 10
#define DS_EFFECT_RIGHT_TRIGGER 0x04
#define DS_EFFECT_LEFT_TRIGGER 0x08
typedef void(*ConnListenerSetAdaptiveTriggers)(uint16_t controllerNumber, uint8_t eventFlags, uint8_t typeLeft, uint8_t typeRight, uint8_t *left, uint8_t *right);

// 此回调用于设置控制器的 RGB LED（如果存在）。
typedef void(*ConnListenerSetControllerLED)(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b);

typedef struct _CONNECTION_LISTENER_CALLBACKS {
    ConnListenerStageStarting stageStarting;
    ConnListenerStageComplete stageComplete;
    ConnListenerStageFailed stageFailed;
    ConnListenerConnectionStarted connectionStarted;
    ConnListenerConnectionTerminated connectionTerminated;
    ConnListenerLogMessage logMessage;
    ConnListenerRumble rumble;
    ConnListenerConnectionStatusUpdate connectionStatusUpdate;
    ConnListenerSetHdrMode setHdrMode;
    ConnListenerRumbleTriggers rumbleTriggers;
    ConnListenerSetMotionEventState setMotionEventState;
    ConnListenerSetControllerLED setControllerLED;
    ConnListenerSetAdaptiveTriggers setAdaptiveTriggers;
} CONNECTION_LISTENER_CALLBACKS, *PCONNECTION_LISTENER_CALLBACKS;

// 使用此函数清零栈或堆上分配的回调
void LiInitializeConnectionCallbacks(PCONNECTION_LISTENER_CALLBACKS clCallbacks);

// ServerCodecModeSupport 值
#define SCM_H264            0x00000001
#define SCM_HEVC            0x00000100
#define SCM_HEVC_MAIN10     0x00000200
#define SCM_AV1_MAIN8       0x00010000 // Sunshine 扩展
#define SCM_AV1_MAIN10      0x00020000 // Sunshine 扩展
#define SCM_H264_HIGH8_444  0x00040000 // Sunshine 扩展
#define SCM_HEVC_REXT8_444  0x00080000 // Sunshine 扩展
#define SCM_HEVC_REXT10_444 0x00100000 // Sunshine 扩展
#define SCM_AV1_HIGH8_444   0x00200000 // Sunshine 扩展
#define SCM_AV1_HIGH10_444  0x00400000 // Sunshine 扩展

// 用于标识各种编解码器能力的 SCM 掩码
#define SCM_MASK_H264   (SCM_H264 | SCM_H264_HIGH8_444)
#define SCM_MASK_HEVC   (SCM_HEVC | SCM_HEVC_MAIN10 | SCM_HEVC_REXT8_444 | SCM_HEVC_REXT10_444)
#define SCM_MASK_AV1    (SCM_AV1_MAIN8 | SCM_AV1_MAIN10 | SCM_AV1_HIGH8_444 | SCM_AV1_HIGH10_444)
#define SCM_MASK_10BIT  (SCM_HEVC_MAIN10 | SCM_HEVC_REXT10_444 | SCM_AV1_MAIN10 | SCM_AV1_HIGH10_444)
#define SCM_MASK_YUV444 (SCM_H264_HIGH8_444 | SCM_HEVC_REXT8_444 | SCM_HEVC_REXT10_444 | SCM_AV1_HIGH8_444 | SCM_AV1_HIGH10_444)

typedef struct _SERVER_INFORMATION {
    // 服务器主机名或文本形式的 IP 地址
    const char* address;

    // /serverinfo 中 'appversion' 标签内的文本
    const char* serverInfoAppVersion;

    // /serverinfo 中 'GfeVersion' 标签内的文本（如果存在）
    const char* serverInfoGfeVersion;

    // /resume 和 /launch 中 'sessionUrl0' 标签内的文本（如果存在）
    const char* rtspSessionUrl;

    // 从 /serverinfo 响应中指定的 'ServerCodecModeSupport'。
    int serverCodecModeSupport;
} SERVER_INFORMATION, *PSERVER_INFORMATION;

// 使用此函数清零栈或堆上分配的服务器信息
void LiInitializeServerInformation(PSERVER_INFORMATION serverInfo);

// 此函数开始流式传输。
//
// 回调都是可选的。传递 NULL 给每个结构中的单个回调，或为整个结构传递 NULL 以对所有回调使用默认值。
//
// 此函数不是线程安全的。
//
int LiStartConnection(PSERVER_INFORMATION serverInfo, PSTREAM_CONFIGURATION streamConfig, PCONNECTION_LISTENER_CALLBACKS clCallbacks,
    PDECODER_RENDERER_CALLBACKS drCallbacks, PAUDIO_RENDERER_CALLBACKS arCallbacks, void* renderContext, int drFlags,
    void* audioContext, int arFlags);

// 此函数停止流式传输。此函数不是线程安全的。
void LiStopConnection(void);

// 此函数中断挂起的 LiStartConnection() 调用。此中断是异步发生的，因此在第一个 LiStartConnection() 调用返回之前启动另一个连接是不安全的。
void LiInterruptConnection(void);

// 用于从传递给 ConnListenerStageXXX 回调的整数获取用户可见的字符串以显示初始化进度
const char* LiGetStageName(int stage);

// 此函数返回通过 ENet 协议统计信息获得的到主机 PC 的当前 RTT 估计值。
// 如果当前的 GFE 版本未对控制流使用 ENet（非常旧的版本），或者 ENet 对等端未连接，此函数将失败。
// 此函数只能在 LiStartConnection() 和 LiStopConnection() 之间调用。
bool LiGetEstimatedRttInfo(uint32_t* estimatedRtt, uint32_t* estimatedRttVariance);

// 此函数将相对鼠标移动事件排队，以便发送到远程服务器。
int LiSendMouseMoveEvent(short deltaX, short deltaY);

// 此函数将鼠标位置更新事件排队，以便发送到远程服务器。
// 此功能仅在 GFE 3.20 或更高版本上可靠支持。较早的版本可能无法正确定位鼠标。
//
// 在许多游戏中，绝对鼠标移动不起作用，因此在进行流式传输时，此模式不应成为鼠标的默认模式。
// 当 LiSendTouchEvent() 不受支持且触摸屏不是主要输入方式时，它可能作为触摸屏的默认行为是可取的。
// 在后一种情况下，使用 LiSendMouseMoveEvent() 的触摸屏模拟触控板模式可能更适合游戏用例。
//
// x 和 y 值被转换为主机坐标，就好像它们来自一个参考宽度为 referenceWidth 且参考高度为 referenceHeight 的平面。
// 这允许您提供相对于任意平面的坐标，例如窗口、屏幕或缩放后的视频视图。
//
// 例如，如果您想直接将窗口坐标作为 x 和 y 传递，可以将 referenceWidth 和 referenceHeight 设置为您的窗口宽度和高度。
int LiSendMousePositionEvent(short x, short y, short referenceWidth, short referenceHeight);

// 此函数将鼠标位置更新事件排队，以便发送到远程服务器，因此上面提到的 LiSendMousePositionEvent() 的所有限制也适用于此！
//
// 此函数的行为类似于 LiSendMouseMoveEvent() 和 LiSendMousePositionEvent() 的组合，因为它发送一个相对运动事件，
// 但是它基于虚拟客户端光标的计算位置发送此数据作为绝对位置，该虚拟客户端光标在调用 LiSendMousePositionEvent() 或
// LiSendMouseMoveAsMousePositionEvent() 时被“移动”。由于此内部虚拟光标状态，调用者必须确保 LiSendMousePositionEvent() 和
// LiSendMouseMoveAsMousePositionEvent() 不会并发调用！
//
// 此函数的一大优点是它允许调用者避免使用 LiSendMouseMoveEvent() 时本会影响的鼠标加速。缺点是它具有与 LiSendMousePositionEvent() 相同的游戏兼容性问题。
//
// 当鼠标捕获是接收鼠标输入的唯一可行方式时（如在 Android 或 iOS 上），并且操作系统在捕获时无法提供原始的未加速鼠标运动，此函数非常有用。
// 使用此函数可以避免在客户端运动也加速时出现双重加速。
int LiSendMouseMoveAsMousePositionEvent(short deltaX, short deltaY, short referenceWidth, short referenceHeight);

// 错误返回值，表示请求的功能不受主机支持
#define LI_ERR_UNSUPPORTED -5501

// 此函数允许将多点触控输入直接发送到 Sunshine 主机。x 和 y 值是归一化的设备坐标，
// 从视频区域的左上角 (0.0, 0.0) 延伸到右下角 (1.0, 1.0)。
//
// 指针 ID 是一个不透明的 ID，必须唯一标识屏幕上的每个活动触摸点。它必须在涉及单个触摸交互的 down/up/move/cancel 事件中保持不变。
//
// 旋转角度以度为单位，从垂直 Y 维度（平行于屏幕）开始，范围 0..360。如果旋转角度未知，请传递 LI_ROT_UNKNOWN。
//
// 压力是一个从 0.0 到 1.0 的值，表示从最小压力到最大压力。发送 down/move 事件时，压力为 0.0 表示实际压力未知。
//
// 对于悬停事件，压力值被视为从 1.0 到 0.0 的距触摸表面的距离范围，其中 1.0 是可测量的最远距离，0.0 是实际接触显示器（对于悬停事件无效）。
// 报告距离 0.0 表示悬停事件的实际距离未知。
//
// 接触区域被建模为一个椭圆，具有归一化设备坐标中的长轴和短轴值。如果接触区域未知，请为两个接触区域轴参数报告 0.0。
// 对于圆形接触区域或如果短轴值不可用，请为长轴和短轴传递相同的值。对于不将接触区域报告为椭圆的 API 或设备，可以使用近似值，例如：
// https://docs.kernel.org/input/multi-touch-protocol.html#event-computation
//
// 对于悬停事件，“接触区域”是悬停手指/工具的大小。如果不可用，请为两个接触区域参数传递 0.0。
//
// 触摸可以使用 LI_TOUCH_EVENT_CANCEL 或 LI_TOUCH_EVENT_CANCEL_ALL 取消。使用 LI_TOUCH_EVENT_CANCEL 时，只有 pointerId 参数有效。所有其他参数都将被忽略。
// 要取消所有活动触摸（例如在失去焦点时），请使用 LI_TOUCH_EVENT_CANCEL_ALL。
//
// 如果主机不支持，此函数将返回 LI_ERR_UNSUPPORTED，调用者应考虑退回到其他函数来发送此输入（例如 LiSendMousePositionEvent()）。
//
// 要确定 LiSendTouchEvent() 是否受支持而无需调用它，请调用 LiGetHostFeatureFlags() 并检查 LI_FF_PEN_TOUCH_EVENTS 标志。
#define LI_TOUCH_EVENT_HOVER       0x00
#define LI_TOUCH_EVENT_DOWN        0x01
#define LI_TOUCH_EVENT_UP          0x02
#define LI_TOUCH_EVENT_MOVE        0x03
#define LI_TOUCH_EVENT_CANCEL      0x04
#define LI_TOUCH_EVENT_BUTTON_ONLY 0x05
#define LI_TOUCH_EVENT_HOVER_LEAVE 0x06
#define LI_TOUCH_EVENT_CANCEL_ALL  0x07
#define LI_ROT_UNKNOWN 0xFFFF
int LiSendTouchEvent(uint8_t eventType, uint32_t pointerId, float x, float y, float pressureOrDistance,
                     float contactAreaMajor, float contactAreaMinor, uint16_t rotation);

// 此函数类似于 LiSendTouchEvent()，但允许与笔输入相关的附加参数，包括倾斜和按钮。
// 倾斜角度以度为单位，从垂直 Z 维度（垂直于屏幕）开始，范围 0..90。有关其他参数的详细文档，请参阅 LiSendTouchEvent()。
//
// 对于 LI_TOUCH_EVENT_BUTTON_ONLY 事件，x、y、压力、旋转、接触区域和倾斜被忽略。
// 如果其中某一项发生变化，请改为发送 LI_TOUCH_EVENT_MOVE 或 LI_TOUCH_EVENT_HOVER。
//
// 要确定 LiSendPenEvent() 是否受支持而无需调用它，请调用 LiGetHostFeatureFlags() 并检查 LI_FF_PEN_TOUCH_EVENTS 标志。
#define LI_TOOL_TYPE_UNKNOWN 0x00
#define LI_TOOL_TYPE_PEN     0x01
#define LI_TOOL_TYPE_ERASER  0x02
#define LI_PEN_BUTTON_PRIMARY   0x01
#define LI_PEN_BUTTON_SECONDARY 0x02
#define LI_PEN_BUTTON_TERTIARY  0x04
#define LI_TILT_UNKNOWN 0xFF
int LiSendPenEvent(uint8_t eventType, uint8_t toolType, uint8_t penButtons,
                   float x, float y, float pressureOrDistance,
                   float contactAreaMajor, float contactAreaMinor,
                   uint16_t rotation, uint8_t tilt);

// 此函数将鼠标按钮事件排队，以便发送到远程服务器。
#define BUTTON_ACTION_PRESS 0x07
#define BUTTON_ACTION_RELEASE 0x08
#define BUTTON_LEFT 0x01
#define BUTTON_MIDDLE 0x02
#define BUTTON_RIGHT 0x03
#define BUTTON_X1 0x04
#define BUTTON_X2 0x05
int LiSendMouseButtonEvent(char action, int button);

// 此函数将键盘事件排队，以便发送到远程服务器。
// 键码是 Win32 虚拟键码（VK），并按照美式英语键盘上的键进行解释。
#define KEY_ACTION_DOWN 0x03
#define KEY_ACTION_UP 0x04
#define MODIFIER_SHIFT 0x01
#define MODIFIER_CTRL 0x02
#define MODIFIER_ALT 0x04
#define MODIFIER_META 0x08
int LiSendKeyboardEvent(short keyCode, char keyAction, char modifiers);

// 类似于 LiSendKeyboardEvent()，但允许客户端告知主机该键码未映射到标准美式英语扫描码，应按原样解释。
// 这是 Sunshine 协议扩展。
#define SS_KBE_FLAG_NON_NORMALIZED 0x01
int LiSendKeyboardEvent2(short keyCode, char keyAction, char modifiers, char flags);

// 此函数将 UTF-8 编码的文本排队，以便发送到远程服务器。
int LiSendUtf8TextEvent(const char *text, unsigned int length);

// 按钮标志
#define A_FLAG     0x1000
#define B_FLAG     0x2000
#define X_FLAG     0x4000
#define Y_FLAG     0x8000
#define UP_FLAG    0x0001
#define DOWN_FLAG  0x0002
#define LEFT_FLAG  0x0004
#define RIGHT_FLAG 0x0008
#define LB_FLAG    0x0100
#define RB_FLAG    0x0200
#define PLAY_FLAG  0x0010
#define BACK_FLAG  0x0020
#define LS_CLK_FLAG  0x0040
#define RS_CLK_FLAG  0x0080
#define SPECIAL_FLAG 0x0400

// 扩展按钮（仅 Sunshine）
#define PADDLE1_FLAG  0x010000
#define PADDLE2_FLAG  0x020000
#define PADDLE3_FLAG  0x040000
#define PADDLE4_FLAG  0x080000
#define TOUCHPAD_FLAG 0x100000 // Sony 控制器上的触摸板按钮
#define MISC_FLAG     0x200000 // 各种控制器上的分享/麦克风/捕获/静音按钮

// 此函数将控制器事件排队，以便发送到远程服务器。它将在计算机上被视为第一个控制器。
int LiSendControllerEvent(int buttonFlags, unsigned char leftTrigger, unsigned char rightTrigger,
    short leftStickX, short leftStickY, short rightStickX, short rightStickY);

// 此函数将控制器事件排队，以便发送到远程服务器。controllerNumber 参数是此事件对应的控制器的从零开始的索引。
// 对于 GFE 主机，最大的合法控制器编号是 3；对于 Sunshine 主机，是 15。在第三代服务器（GFE 2.1.x）上，
// 无论 controllerNumber 参数如何，这些都将作为控制器 0 发送。
//
// activeGamepadMask 参数是一个位字段，为每个存在的控制器设置对应的位。
// 在 GFE 上，activeGamepadMask 限制为最多 4 位（0xF）。
// 在 Sunshine 上，限制为 16 位（0xFFFF）。
//
// 为了指示游戏手柄的到达，您可以发送一个空事件，其中控制器编号设置为新控制器，并在活动游戏手柄掩码中设置新控制器的位。
// 但是，您应优先使用 LiSendControllerArrivalEvent() 而不是此函数来实现此目的，因为它允许主机更好地选择模拟的控制器。
//
// 为了指示游戏手柄的移除，发送一个空事件，其中控制器编号设置为已移除的控制器，并在活动游戏手柄掩码中清除已移除控制器的位。
int LiSendMultiControllerEvent(short controllerNumber, short activeGamepadMask,
    int buttonFlags, unsigned char leftTrigger, unsigned char rightTrigger,
    short leftStickX, short leftStickY, short rightStickX, short rightStickY);

// 此函数提供一种告知主机新控制器上可用按钮和功能的方法。这是指示新控制器到达的推荐方法。
//
// 这可以让主机在决定模拟哪种类型的控制器以及向操作系统上的虚拟控制器通告哪些功能时做出更好的决策。
//
// 如果主机不支持控制器到达事件，此函数将回退到通过 LiSendMultiControllerEvent() 指示到达。
#define LI_CTYPE_UNKNOWN  0x00
#define LI_CTYPE_XBOX     0x01
#define LI_CTYPE_PS       0x02
#define LI_CTYPE_NINTENDO 0x03
#define LI_CCAP_ANALOG_TRIGGERS 0x01 // 报告 0x00 到 0xFF 之间的扳机轴值
#define LI_CCAP_RUMBLE          0x02 // 能够响应 ConnListenerRumble() 回调进行振动
#define LI_CCAP_TRIGGER_RUMBLE  0x04 // 能够响应 ConnListenerRumbleTriggers() 回调进行扳机振动
#define LI_CCAP_TOUCHPAD        0x08 // 通过 LiSendControllerTouchEvent() 报告触摸板事件
#define LI_CCAP_ACCEL           0x10 // 能够通过 LiSendControllerMotionEvent() 报告加速度计事件
#define LI_CCAP_GYRO            0x20 // 能够通过 LiSendControllerMotionEvent() 报告陀螺仪事件
#define LI_CCAP_BATTERY_STATE   0x40 // 通过 LiSendControllerBatteryEvent() 报告电池状态
#define LI_CCAP_RGB_LED         0x80 // 能够通过 ConnListenerSetControllerLED() 设置 RGB LED 状态
int LiSendControllerArrivalEvent(uint8_t controllerNumber, uint16_t activeGamepadMask, uint8_t type,
                                 uint32_t supportedButtonFlags, uint16_t capabilities);

// 此函数类似于 LiSendTouchEvent()，但触摸事件与游戏控制器上存在的触摸板设备相关联，而不是触摸屏。
//
// 如果主机不支持，此函数将返回 LI_ERR_UNSUPPORTED，调用者应考虑使用此触摸输入来模拟触控板输入。
//
// 要确定 LiSendControllerTouchEvent() 是否受支持而无需调用它，请调用 LiGetHostFeatureFlags() 并检查 LI_FF_CONTROLLER_TOUCH_EVENTS 标志。
int LiSendControllerTouchEvent(uint8_t controllerNumber, uint8_t eventType, uint32_t pointerId, float x, float y, float pressure);

// 此函数允许客户端将控制器相关的运动事件发送到支持的主机。
//
// 出于功耗和性能原因，除非主机通过 ConnListenerSetMotionEventState() 明确请求运动事件报告，否则不应启用运动传感器。
//
// LI_MOTION_TYPE_ACCEL 应以 m/s² 为单位报告数据（包括重力加速度）。
// LI_MOTION_TYPE_GYRO 应以 deg/s 为单位报告数据。
//
// x/y/z 轴分配遵循 SDL 在此处记录的约定：
// https://github.com/libsdl-org/SDL/blob/96720f335002bef62115e39327940df454d78f6c/include/SDL3/SDL_sensor.h#L80-L124
#define LI_MOTION_TYPE_ACCEL 0x01
#define LI_MOTION_TYPE_GYRO  0x02
int LiSendControllerMotionEvent(uint8_t controllerNumber, uint8_t motionType, float x, float y, float z);

// 此函数允许客户端向支持的主机发送控制器电池状态。如果主机可以在模拟的控制器上调整电池状态，
// 它可以使用此信息使虚拟控制器与客户端上的物理控制器匹配。
#define LI_BATTERY_STATE_UNKNOWN      0x00
#define LI_BATTERY_STATE_NOT_PRESENT  0x01
#define LI_BATTERY_STATE_DISCHARGING  0x02
#define LI_BATTERY_STATE_CHARGING     0x03
#define LI_BATTERY_STATE_NOT_CHARGING 0x04 // 连接到电源但未充电
#define LI_BATTERY_STATE_FULL         0x05
#define LI_BATTERY_STATE_PERCENTAGE_UNKNOWN 0xFF
int LiSendControllerBatteryEvent(uint8_t controllerNumber, uint8_t batteryState, uint8_t batteryPercentage);

// 此函数将垂直滚动事件排队到远程服务器。
// “点击”次数在发送到 PC 之前乘以 WHEEL_DELTA（120）。
int LiSendScrollEvent(signed char scrollClicks);

// 此函数将垂直滚动事件排队到远程服务器。
// 与 LiSendScrollEvent() 不同，此函数可以发送小于 120 单位的滚轮事件，适用于支持“高分辨率”滚动的设备（Apple 触控板、Microsoft 精确触控板等）。
int LiSendHighResScrollEvent(short scrollAmount);

// 这些函数将水平滚动事件发送到主机，类似于 LiSendScrollEvent() 和 LiSendHighResScrollEvent()。
// 这是 Sunshine 协议扩展。
int LiSendHScrollEvent(signed char scrollClicks);
int LiSendHighResHScrollEvent(short scrollAmount);

// 此函数返回一个微秒级的时间，其实现定义的纪元。
// 它只应与之前调用自身返回的值进行比较。
uint64_t LiGetMicroseconds(void);

// 此函数返回一个毫秒级的时间，其实现定义的纪元。
// 它只应与之前调用自身返回的值进行比较。
uint64_t LiGetMillis(void);

// 这是一个简单的 STUN 函数，可以帮助客户端获取通过 IPv4 使用 mDNS 发现的计算机的 WAN 地址。
// 在 GFE 停止发送外部地址后，这可用于预填充流式传输的外部地址。wanAddr 以网络字节顺序返回。
int LiFindExternalAddressIP4(const char* stunServer, unsigned short stunPort, unsigned int* wanAddr);

// 返回已准备好传递的排队视频帧数。仅当未为视频渲染器设置 CAPABILITY_DIRECT_SUBMIT 时相关。
int LiGetPendingVideoFrames(void);

// 返回已准备好传递的排队音频帧数。仅当未为音频渲染器设置 CAPABILITY_DIRECT_SUBMIT 时相关。
// 对于大多数用途，LiGetPendingAudioDuration() 可能比此函数更可取。
int LiGetPendingAudioFrames(void);

// 类似于 LiGetPendingAudioFrames()，但返回的是毫秒级的排队音频持续时间，而不是帧数，
// 这使调用者能够忽略协商的音频帧持续时间。
int LiGetPendingAudioDuration(void);

// 返回指向包含 RTP 音频流各种统计信息的结构的指针。
// 数据应被视为只读，不得修改。
typedef struct _RTP_AUDIO_STATS {
    uint32_t packetCountAudio;         // 总音频包数
    uint32_t packetCountFec;           // 类型为 FEC 的总包数
    uint32_t packetCountFecRecovered;  // 成功恢复的包
    uint32_t packetCountFecFailed;     // 尝试恢复但因丢包过多而失败
    uint32_t packetCountOOS;           // 乱序包数
    uint32_t packetCountInvalid;       // 损坏的包等
    uint32_t packetCountFecInvalid;    // 无效的 FEC 包
} RTP_AUDIO_STATS, *PRTP_AUDIO_STATS;

const RTP_AUDIO_STATS* LiGetRTPAudioStats(void);

// 返回指向包含 RTP 视频流各种统计信息的结构的指针。
// 数据应被视为只读，不得修改。
// 目前这主要用于跟踪总视频和 FEC 包数，因为在 moonlight-qt 中已经在更高级别实现了许多视频统计信息。
typedef struct _RTP_VIDEO_STATS {
    uint32_t packetCountVideo;         // 总视频包数
    uint32_t packetCountFec;           // 类型为 FEC 的总包数
    uint32_t packetCountFecRecovered;  // 成功恢复的包
    uint32_t packetCountFecFailed;     // 尝试恢复但因丢包过多而失败
    uint32_t packetCountOOS;           // 乱序包数
    uint32_t packetCountInvalid;       // 损坏的包等
    uint32_t packetCountFecInvalid;    // 无效的 FEC 包
} RTP_VIDEO_STATS, *PRTP_VIDEO_STATS;

const RTP_VIDEO_STATS* LiGetRTPVideoStats(void);

// 用于 LiGetPortFromPortFlagIndex() 和 LiGetProtocolFromPortFlagIndex() 的端口索引标志
#define ML_PORT_INDEX_TCP_47984 0
#define ML_PORT_INDEX_TCP_47989 1
#define ML_PORT_INDEX_TCP_48010 2
#define ML_PORT_INDEX_UDP_47998 8
#define ML_PORT_INDEX_UDP_47999 9
#define ML_PORT_INDEX_UDP_48000 10
#define ML_PORT_INDEX_UDP_48010 11

// 用于 LiTestClientConnectivity() 的端口标志
#define ML_PORT_FLAG_ALL       0xFFFFFFFF
#define ML_PORT_FLAG_TCP_47984 0x0001
#define ML_PORT_FLAG_TCP_47989 0x0002
#define ML_PORT_FLAG_TCP_48010 0x0004
#define ML_PORT_FLAG_UDP_47998 0x0100
#define ML_PORT_FLAG_UDP_47999 0x0200
#define ML_PORT_FLAG_UDP_48000 0x0400
#define ML_PORT_FLAG_UDP_48010 0x0800

// 返回与连接失败阶段或连接终止错误相关的端口对应的端口标志。
//
// 这些可用于专门测试可能导致连接失败的端口。如果给定的失败不太可能涉及任何端口，则此函数返回 0。
unsigned int LiGetPortFlagsFromStage(int stage);
unsigned int LiGetPortFlagsFromTerminationErrorCode(int errorCode);

// 返回指定端口索引的 IPPROTO_* 值
int LiGetProtocolFromPortFlagIndex(int portFlagIndex);

// 返回指定端口索引的端口号
unsigned short LiGetPortFromPortFlagIndex(int portFlagIndex);

// 将输入参数中设置的端口标志转换为字符串列表，填充到输出缓冲区。
// 第二个及后续条目将以 'separator'（如果提供）为前缀。
// 如果输出缓冲区太小，输出将被截断以适应提供的缓冲区。
void LiStringifyPortFlags(unsigned int portFlags, const char* separator, char* outputBuffer, int outputBufferLength);

// 此函数可用于测试本地网络是否阻止了 Moonlight 的端口。它需要运行在可访问互联网的主机上的测试服务器。
// 要执行测试，请传入测试服务器的 DNS 主机名、一个参考 TCP 端口以确保测试主机基本可达（非常不可能被阻止的端口，如 80 或 443），
// 以及一组对应于您要测试的端口的 ML_PORT_FLAG_* 值。返回时，如果发生灾难性错误，则返回 ML_TEST_RESULT_INCONCLUSIVE，
// 否则返回验证失败的端口标志集。如果所有端口均成功验证，则返回 0。
//
// 建议不要显式使用端口标志（因为 GameStream 端口将来可能更改），而是使用 ML_PORT_FLAG_ALL 或在连接失败时使用 LiGetPortFlagsFromStage()。
//
// 测试服务器可在 https://github.com/cgutman/gfe-loopback 获取
#define ML_TEST_RESULT_INCONCLUSIVE 0xFFFFFFFF
unsigned int LiTestClientConnectivity(const char* testServer, unsigned short referencePort, unsigned int testPortFlags);

// 此系列函数可用于选择拉取模式的视频渲染器，这些渲染器选择自己管理解码/渲染线程。
// 在成功调用等待/轮询变体以出队视频帧后，您必须调用 LiCompleteVideoFrame() 以通知处理已完成。
// 必须将从 drSubmitDecodeUnit() 返回的相同 DR_* 状态值作为 drStatus 参数传递给 LiCompleteVideoFrame()。
//
// 要安全地使用这些函数，必须在视频解码器上设置 CAPABILITY_PULL_RENDERER。
typedef void* VIDEO_FRAME_HANDLE;
bool LiWaitForNextVideoFrame(VIDEO_FRAME_HANDLE* frameHandle, PDECODE_UNIT* decodeUnit);
bool LiPollNextVideoFrame(VIDEO_FRAME_HANDLE* frameHandle, PDECODE_UNIT* decodeUnit);
bool LiPeekNextVideoFrame(PDECODE_UNIT* decodeUnit);
void LiWakeWaitForVideoFrame(void);
void LiCompleteVideoFrame(VIDEO_FRAME_HANDLE handle, int drStatus);

// 此函数返回主机 PC 上次报告的 HDR 模式。
// 有关更多详细信息，请参阅 ConnListenerSetHdrMode()。
bool LiGetCurrentHostDisplayHdrMode(void);

typedef struct _SS_HDR_METADATA {
    // RGB 顺序
    struct {
        uint16_t x; // 归一化为 50,000
        uint16_t y; // 归一化为 50,000
    } displayPrimaries[3];

    struct {
        uint16_t x; // 归一化为 50,000
        uint16_t y; // 归一化为 50,000
    } whitePoint;

    uint16_t maxDisplayLuminance; // 尼特
    uint16_t minDisplayLuminance; // 1/10000 尼特

    // 这些是特定于内容的数值，可能并非对所有主机都可用。
    uint16_t maxContentLightLevel; // 尼特
    uint16_t maxFrameAverageLightLevel; // 尼特

    // 这些是特定于显示器的数值，可能并非对所有主机都可用。
    uint16_t maxFullFrameLuminance; // 尼特
} SS_HDR_METADATA, *PSS_HDR_METADATA;

// 此函数用主机 PC 显示器的 HDR 元数据（如果可用）填充提供的 mastering metadata 结构。
// 仅当主机上 HDR 模式处于活动状态时调用此函数才有效。这是 Sunshine 协议扩展。
bool LiGetHdrMetadata(PSS_HDR_METADATA metadata);

// 此函数向主机请求一个 IDR 帧。通常使用 DR_NEED_IDR 完成，但异步处理帧的客户端可能需要在为前一帧返回 DR_OK 后重置其解码器状态。
// 与其等待新帧并为其返回 DR_NEED_IDR，不如直接调用此 API。请注意，此函数并不保证 *下一个* 帧是 IDR 帧，只保证 IDR 帧很快会到达。
void LiRequestIdrFrame(void);

// 此函数返回主机支持的任何扩展功能标志。
#define LI_FF_PEN_TOUCH_EVENTS        0x01 // 支持 LiSendTouchEvent()/LiSendPenEvent()
#define LI_FF_CONTROLLER_TOUCH_EVENTS 0x02 // 支持 LiSendControllerTouchEvent()
uint32_t LiGetHostFeatureFlags(void);

#ifdef __cplusplus
}
#endif