//
// 此头文件为客户端使用公开了流式传输 API
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

下面是 'colorSpace' 字段的取值。GFE 主机上的 H.264 视频流不支持 Rec. 2020。
#define COLORSPACE_REC_601  0
#define COLORSPACE_REC_709  1
#define COLORSPACE_REC_2020 2

// 下面 'colorRange' 字段的值
#define COLOR_RANGE_LIMITED  0
#define COLOR_RANGE_FULL     1

// 下面是 'encryptionFlags' 字段的值
#define ENCFLG_NONE  0x00000000
#define ENCFLG_AUDIO 0x00000001
#define ENCFLG_VIDEO 0x00000002
#define ENCFLG_ALL   0xFFFFFFFF

此函数返回一个字符串，你应当将其附加到 /launch 和 /resume 查询参数字符串中。它用于启用 Sunshine 主机的某些扩展功能。返回的字符串由 moonlight-common-c 拥有，调用者不应释放该字符串。
const char* LiGetLaunchUrlQueryParameters(void);

typedef struct _STREAM_CONFIGURATION {
    // 所需视频流的像素尺寸
    int width;
    int height;

    // 所需视频流的帧率
    int fps;

所需视频流的比特率（音频会额外增加约 1 Mbps）。这包括纠错数据，因此在使用标准 20% FEC 配置时，实际编码器比特率会低约 20%。
    int bitrate;

视频数据包的最大字节数（如果不确定，请使用1024）。如果 STREAM_CFG_AUTO 判定流是远程的（见下文），它会将此值限制为1024，以避免与 MTU 相关的问题，如数据包丢失和分片。
    int packetSize;

确定是否启用远程（通过互联网）流优化。如果不确定，请设置为 STREAM_CFG_AUTO。STREAM_CFG_AUTO 使用一种启发式方法（目标地址是否在 RFC 1918 地址块中）来判断流是否为远程流。
    int streamingRemotely;

    // 指定音频流的通道配置。  
    // 请参阅下方的 AUDIO_CONFIGURATION 常量和 MAKE_AUDIO_CONFIGURATION()。
    int audioConfiguration;

指定支持的视频格式掩码。请参见下面的 VIDEO_FORMAT 常量。
    int supportedVideoFormats;

如果指定，则表示客户端的显示刷新率乘以 100。例如，59.94 Hz 将指定为 5994。最近版本的 GFE 使用此设置来增强帧节奏控制。
    int clientRefreshRateX100;

如果指定，将编码器的色彩空间设置为提供的 COLORSPACE_* 选项（如上所列）。如果未设置，编码器将默认为 Rec 601。
    int colorSpace;

如果指定，则将编码器颜色范围设置为提供的 COLOR_RANGE_* 选项（如上所列）。如果未设置，编码器将默认使用有限范围。
    int colorRange;

指定可以启用加密的数据流（如果主机电脑支持）。理想情况下，您应传递 ENCFLG_ALL 以加密我们支持加密的所有内容。然而，性能较低的硬件可能无法支持加密像视频或音频这样的高负载数据，因此这里可能会禁用加密。远程输入加密始终启用。
    int encryptionFlags;

远程输入流的 AES 加密数据。这必须与在 /launch 和 /resume 请求中传递的 rikey 和 rikeyid 相同。
    char remoteInputAesKey[16];
    char remoteInputAesIv[16];
} STREAM_CONFIGURATION, *PSTREAM_CONFIGURATION;

// 当在堆栈或堆上分配时，使用此函数将流配置清零
void LiInitializeStreamConfiguration(PSTREAM_CONFIGURATION streamConfig);

这些用于识别缓冲列表中作为 IDR 帧标识的 H.264 和 HEVC 格式帧的编解码器配置数据。对于其他编解码器，所有数据都标记为 BUFFER_TYPE_PICDATA。
#define BUFFER_TYPE_PICDATA  0x00
#define BUFFER_TYPE_SPS      0x01
#define BUFFER_TYPE_PPS      0x02
#define BUFFER_TYPE_VPS      0x03

typedef struct _LENTRY {
    // 指向下一个条目的指针，如果这是最后一个条目则为 NULL
    struct _LENTRY* next;

    // 指向数据的指针（永不为 NULL）
    char* data;

    // 数据大小（字节）（绝不小于等于0）
    int length;

    // 缓冲类型（如上所列，仅适用于 H.264 和 HEVC 格式）
    int bufferType;
} LENTRY, *PLENTRY;

这是一个标准帧，它引用了IDR帧和之前的P帧。
#define FRAME_TYPE_PFRAME 0x00

这是关键帧。对于 H.264 和 HEVC，这意味着该帧包含 SPS、PPS 和 VPS（仅限 HEVC）NALU，作为列表中的第一个缓冲区。I 帧数据紧随编解码器配置 NALU 之后。对于其他编解码器，任何配置信息都不会拆分成单独的缓冲区。
#define FRAME_TYPE_IDR    0x01

// 解码单元描述了来自多个数据包的视频数据缓冲链
typedef struct _DECODE_UNIT {
    // 帧号
    int frameNumber;

    // 框架类型
    int frameType;

帧的可选主机处理延迟，单位为1/10毫秒。当主机未提供延迟数据或帧处理延迟不适用于当前帧时（发生在帧重复时），为零。
    uint16_t frameHostProcessingLatency;

    // 第一个缓冲区接收时间（微秒）。
    uint64_t receiveTimeUs;

帧完全组装并排队等待视频解码器处理的时间。 这大约也是接收最后一个数据包的时间，所以 enqueueTimeUs - receiveTimeUs 就是接收该帧所花费的时间。在 decode 单元被传递给 submitDecodeUnit() 时，可以计算出总队列延迟。该值以微秒为单位。
    uint64_t enqueueTimeUs;

以微秒为单位的呈现时间，起点为第一个捕获的帧。  
这可用于帮助帧速控制或丢弃在显示前排队时间过长的旧帧。
    uint64_t presentationTimeUs;

原始 RTP 时间戳，单位为 90kHz。在使用处理整数时间的 API（如 Apple 的 CMTime）时非常有用。要精确恢复 RTP 时间戳，可使用类似 CMTimeMake((int64_t)du->rtpTimestamp, 90000); 的方法。
    uint32_t rtpTimestamp;

    // 整个缓冲链的字节长度
    int fullLength;

    // 缓冲链的头（永不为 NULL）
    PLENTRY bufferList;

确定此帧是 SDR 还是 HDR
注意：目前这并不是从实际的比特流中解析的，因此如果你的客户端可以访问比特流解析器，建议优先使用解析器而不是此字段。
    bool hdrActive;

提供此帧的色彩空间（见上面的 COLORSPACE_* 定义）
注意：目前此信息并未从实际比特流解析，因此如果您的客户端可以访问比特流解析器，建议优先使用解析器而不是此字段。
    uint8_t colorspace;
} DECODE_UNIT, *PDECODE_UNIT;

// 指定音频流应以立体声编码（默认）
#define AUDIO_CONFIGURATION_STEREO MAKE_AUDIO_CONFIGURATION(2, 0x3)

// 如果电脑支持，指定音频流应为5.1环绕声
#define AUDIO_CONFIGURATION_51_SURROUND MAKE_AUDIO_CONFIGURATION(6, 0x3F)

// 如果电脑支持，指定音频流应为7.1环绕声
#define AUDIO_CONFIGURATION_71_SURROUND MAKE_AUDIO_CONFIGURATION(8, 0x63F)

// 通过通道数量和通道掩码指定音频配置。  
// 有关 channelMask 值，请参见 https://docs.microsoft.com/zh-cn/windows-hardware/drivers/audio/channel-mask  
// 注意：并非所有组合都受 GFE 和/或此库支持。
#define MAKE_AUDIO_CONFIGURATION(channelCount, channelMask) \
    (((channelMask) << 16) | (channelCount << 8) | 0xCA)

// 用于从音频配置中获取通道数量和通道掩码的辅助宏
#define CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(x) (((x) >> 8) & 0xFF)
#define CHANNEL_MASK_FROM_AUDIO_CONFIGURATION(x) (((x) >> 16) & 0xFFFF)

辅助宏，用于获取 surroundAudioInfo 参数值，该参数必须在启动会话时通过 /launch 和 /resume HTTPS 请求传递。
#define SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(x) \
    (CHANNEL_MASK_FROM_AUDIO_CONFIGURATION(x) << 16 | CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(x))

// 支持的最大通道数
#define AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT 8

传入 StreamConfiguration.supportedVideoFormats 来指定支持的编解码器，传入 DecoderRendererSetup() 来指定选定的编解码器。
#define VIDEO_FORMAT_H264            0x0001 // H.264 高级配置文件
#define VIDEO_FORMAT_H264_HIGH8_444  0x0004 // H.264 高 4:4:4 8 位配置文件
#define VIDEO_FORMAT_H265            0x0100 // HEVC 主配置文件
#define VIDEO_FORMAT_H265_MAIN10     0x0200 // HEVC 主10配置文件
#define VIDEO_FORMAT_H265_REXT8_444  0x0400 // HEVC RExt 4:4:4 8位配置文件
#define VIDEO_FORMAT_H265_REXT10_444 0x0800 // HEVC RExt 4:4:4 10位配置文件
#define VIDEO_FORMAT_AV1_MAIN8       0x1000 // AV1 主 8 位配置文件
#define VIDEO_FORMAT_AV1_MAIN10      0x2000 // AV1 主 10 位配置文件
#define VIDEO_FORMAT_AV1_HIGH8_444   0x4000 // AV1 高级 4:4:4 8位配置文件
#define VIDEO_FORMAT_AV1_HIGH10_444  0x8000 // AV1 高级 4:4:4 10位配置文件

// 供客户使用的掩码，用于匹配视频编解码器而不涉及特定配置文件的细节。
#define VIDEO_FORMAT_MASK_H264   0x000F
#define VIDEO_FORMAT_MASK_H265   0x0F00
#define VIDEO_FORMAT_MASK_AV1    0xF000
#define VIDEO_FORMAT_MASK_10BIT  0xAA00
#define VIDEO_FORMAT_MASK_YUV444 0xCC04

如果在渲染器能力字段中设置此标志，音频/视频数据将直接从接收线程提交。只有在渲染器为非阻塞时才应指定此选项。此标志在音频和视频渲染器上均有效。
#define CAPABILITY_DIRECT_SUBMIT 0x1

如果在视频渲染器功能字段中设置，此标志表示渲染器支持 AVC/H.264 流的参考帧失效。此标志仅在视频渲染器上有效。使用此功能时，可能无法对比特流进行修改（更改 num_ref_frames 或 max_dec_frame_buffering），以避免在数据包丢失时出现视频损坏。
#define CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC 0x2

如果在视频渲染器功能字段中设置，该标志表示渲染器支持 HEVC/H.265 流的参考帧失效。此标志仅在视频渲染器上有效。
#define CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC 0x4

如果在音频渲染器功能字段中设置此标志，RTSP 协商将永远不会请求“高质量”音频预设。如果未设置，则超过 15 Mbps 的视频流将使用高质量音频。
#define CAPABILITY_SLOW_OPUS_DECODER 0x8

如果在音频渲染器功能字段中设置，这表明音频包可能包含多于或少于 5 毫秒的音频。这要求音频渲染器读取 OPUS_MULTISTREAM_CONFIGURATION 中的 samplesPerFrame 字段来计算正确的解码缓冲区大小，而不仅仅是假设它总是为 240。
#define CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION 0x10

此标志将渲染器切换为基于拉取的模型，而不是默认的基于推送的回调模型。渲染器必须调用新的函数（LiWaitForNextVideoFrame()、LiCompleteVideoFrame() 等）来接收音视频数据。在提供示例回调的同时设置此功能是不允许的。
#define CAPABILITY_PULL_RENDERER 0x20

如果在视频渲染器功能字段中设置，该标志表示渲染器支持 AV1 流的参考帧失效。该标志仅在视频渲染器上有效。
#define CAPABILITY_REFERENCE_FRAME_INVALIDATION_AV1 0x40

如果在视频渲染器能力字段中设置，该宏指定渲染器支持切片以提高解码性能。该参数指定每帧所需的切片数量。此能力仅对视频渲染器有效。
#define CAPABILITY_SLICES_PER_FRAME(x) (((unsigned char)(x)) << 24)

此回调被调用以提供有关视频流的详细信息，并允许配置解码器。成功时返回0，失败时返回非零值。
typedef int(*DecoderRendererSetup)(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags);

// 此回调通知解码器流即将开始。在此回调返回之前，无法提交任何帧。
typedef void(*DecoderRendererStart)(void);

// 此回调通知解码器流正在停止。仍然可以提交帧，但它们可以安全地被丢弃。
typedef void(*DecoderRendererStop)(void);

// 此回调会执行视频解码器的拆除操作。当此回调被调用时，将不再提交任何帧。
typedef void(*DecoderRendererCleanup)(void);


此回调将 Annex B 格式的基本流数据提供给解码器。如果解码器因某种原因无法处理提交的数据，则必须返回 DR_NEED_IDR 以生成关键帧。
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

// 使用此功能在分配到栈或堆时将视频回调置零
void LiInitializeVideoCallbacks(PDECODER_RENDERER_CALLBACKS drCallbacks);

该结构提供了 Opus 多流解码器参数，用于成功解码从计算机发送的音频流。有关这些字段的详细信息，请参阅 opus_multistream_decoder_init 文档。
所提供的映射数组根据以下输出声道顺序进行索引：
0 - 前左
1 - 前右
2 - 中置
3 - 低音炮 (LFE)
4 - 后左
5 - 后右
6 - 侧左
7 - 侧右
如果映射顺序与音频渲染器的声道顺序不匹配，可以交换不匹配索引处的值，直到映射数组符合所需的声道顺序。
typedef struct _OPUS_MULTISTREAM_CONFIGURATION {
    int sampleRate;
    int channelCount;
    int streams;
    int coupledStreams;
    int samplesPerFrame;
    unsigned char mapping[AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT];
} OPUS_MULTISTREAM_CONFIGURATION, *POPUS_MULTISTREAM_CONFIGURATION;

此回调用于初始化音频渲染器。音频配置参数提供协商后的音频配置。这可能与流配置中指定的配置不同。成功返回0，失败返回非0。
typedef int(*AudioRendererInit)(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags);

// This callback notifies the decoder that the stream is starting. No audio can be submitted before this callback returns.
typedef void(*AudioRendererStart)(void);

// 此回调通知解码器流正在停止。音频样本仍可提交，但可以安全地丢弃。
typedef void(*AudioRendererStop)(void);

// 此回调执行音频解码器的最终拆卸。当此回调被调用时，将不再提交任何音频。
typedef void(*AudioRendererCleanup)(void);

// 此回调提供要解码和播放的 Opus 音频数据。sampleLength 的单位是字节。
typedef void(*AudioRendererDecodeAndPlaySample)(char* sampleData, int sampleLength);

typedef struct _AUDIO_RENDERER_CALLBACKS {
    AudioRendererInit init;
    AudioRendererStart start;
    AudioRendererStop stop;
    AudioRendererCleanup cleanup;
    AudioRendererDecodeAndPlaySample decodeAndPlaySample;
    int capabilities;
} AUDIO_RENDERER_CALLBACKS, *PAUDIO_RENDERER_CALLBACKS;

// 当在栈或堆上分配时，使用此函数将音频回调归零
void LiInitializeAudioCallbacks(PAUDIO_RENDERER_CALLBACKS arCallbacks);

// 未来版本可能会有所更改
// 请使用 LiGetStageName() 获取稳定的关卡名称
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

// 此回调会在初始化阶段即将开始时被调用
typedef void(*ConnListenerStageStarting)(int stage);

// 此回调被调用以表示初始化的一个阶段已完成
typedef void(*ConnListenerStageComplete)(int stage);

此回调用于指示初始化的某个阶段失败。由于连接尚未完全建立，ConnListenerConnectionTerminated() 将不会被调用。LiInterruptConnection() 和 LiStopConnection() 可能会导致此回调被调用，但不能保证一定会发生。
typedef void(*ConnListenerStageFailed)(int stage, int errorCode);

// 在连接成功建立后将调用此回调
typedef void(*ConnListenerConnectionStarted)(void);

当连接在建立后被终止时，将调用此回调。如果服务器报告终止是有意的（例如，用户关闭了游戏），则 errorCode 为 0。如果 errorCode 非零，则表示终止可能是意外的（网络丢失、崩溃或类似情况）。调用 LiStopConnection() 或 LiInterruptConnection() 不会触发此回调。
typedef void(*ConnListenerConnectionTerminated)(int errorCode);

当流被主机正常终止时，该错误代码会传递给 ConnListenerConnectionTerminated()。通常这意味着主机电脑上的应用程序已经退出。
#define ML_ERROR_GRACEFUL_TERMINATION 0

如果在等待几秒后从未收到该连接的视频数据，此错误将传递给 ConnListenerConnectionTerminated()。这很可能表明由于防火墙或端口转发规则缺失或错误，UDP 47998 上的流量存在问题。
#define ML_ERROR_NO_VIDEO_TRAFFIC -100

如果在等待了几秒钟后仍无法接收到完整的帧，则会将此错误传递给 ConnListenerConnectionTerminated()。这可能表示连接极不稳定或比特率过高。
#define ML_ERROR_NO_VIDEO_FRAME -101

如果流在启动后不久因主机的正常终止而结束，则此错误会传递给 ConnListenerConnectionTerminated()。通常，如果屏幕上显示受 DRM 保护的内容（GFE 3.22 之前版本）或其他导致编码器无法成功捕获视频的问题时，通常会发生这种情况。
#define ML_ERROR_UNEXPECTED_EARLY_TERMINATION -102

如果流因主机的受保护内容错误而结束，此错误将传递给 ConnListenerConnectionTerminated()。此值在 GFE 3.22 及以上版本中受支持。
#define ML_ERROR_PROTECTED_CONTENT -103

如果流由于帧转换错误而结束，此错误将传递给 ConnListenerConnectionTerminated()。这最常见的原因是桌面分辨率与启用 HDR 的流媒体分辨率不兼容。此值在 GFE 3.22 及更高版本中受支持。
#define ML_ERROR_FRAME_CONVERSION -104

// 此回调用于记录调试信息
typedef void(*ConnListenerLogMessage)(const char* format, ...);

此回调用于触发游戏手柄的震动。在此回调中设置的震动效果值预计将持续有效，直到将来有另一次调用设置不同的触觉效果或通过对两个马达传递 0 来关闭马达。可能会收到针对物理上不存在的游戏手柄的震动事件，因此您的回调应该处理这种可能性。
typedef void(*ConnListenerRumble)(unsigned short controllerNumber, unsigned short lowFreqMotor, unsigned short highFreqMotor);

此回调用于通知客户端连接状态的变化。可以考虑显示一个覆盖层，让用户了解他们的流媒体为什么未达到预期效果。
#define CONN_STATUS_OKAY    0
#define CONN_STATUS_POOR    1
typedef void(*ConnListenerConnectionStatusUpdate)(int connectionStatus);

当主机的 HDR 模式发生变化时，将调用此回调以通知客户端。客户端可能需要更新本地显示模式以匹配主机上的 HDR 状态。即使流媒体未使用支持 HDR 的编解码器，也可能会调用此回调。
typedef void(*ConnListenerSetHdrMode)(bool hdrEnabled);

此回调用于振动游戏手柄的扳机。更多详情，请参见上方关于 ConnListenerRumble() 的注释。
typedef void(*ConnListenerRumbleTriggers)(uint16_t controllerNumber, uint16_t leftTriggerMotor, uint16_t rightTriggerMotor);

此回调用于通知客户端主机希望指定的游戏手柄提供运动传感器报告（参见 LiSendControllerMotionEvent()），报告将以指定的频率进行（或尽可能接近）。如果 reportRateHz 为 0，则主机正在请求停止运动事件报告。
typedef void(*ConnListenerSetMotionEventState)(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz);

此回调用于通知客户端双感应自适应扳机配置的更改。
#define DS_EFFECT_PAYLOAD_SIZE 10
#define DS_EFFECT_RIGHT_TRIGGER 0x04
#define DS_EFFECT_LEFT_TRIGGER 0x08
typedef void(*ConnListenerSetAdaptiveTriggers)(uint16_t controllerNumber, uint8_t eventFlags, uint8_t typeLeft, uint8_t typeRight, uint8_t *left, uint8_t *right);

// 此回调用于设置控制器的 RGB LED（如果有的话）。
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

// 当在栈或堆上分配时，使用此函数将连接回调归零
void LiInitializeConnectionCallbacks(PCONNECTION_LISTENER_CALLBACKS clCallbacks);

// ServerCodecModeSupport 值
#define SCM_H264            0x00000001
#define SCM_HEVC            0x00000100
#define SCM_HEVC_MAIN10     0x00000200
#define SCM_AV1_MAIN8       0x00010000 // 阳光扩展
#define SCM_AV1_MAIN10      0x00020000 // 阳光扩展
#define SCM_H264_HIGH8_444  0x00040000 // 阳光扩展
#define SCM_HEVC_REXT8_444  0x00080000 // 阳光扩展
#define SCM_HEVC_REXT10_444 0x00100000 // 阳光扩展
#define SCM_AV1_HIGH8_444   0x00200000 // 阳光扩展
#define SCM_AV1_HIGH10_444  0x00400000 // 阳光扩展

// SCM 掩码用于识别各种编解码器功能
#define SCM_MASK_H264   (SCM_H264 | SCM_H264_HIGH8_444)
#define SCM_MASK_HEVC   (SCM_HEVC | SCM_HEVC_MAIN10 | SCM_HEVC_REXT8_444 | SCM_HEVC_REXT10_444)
#define SCM_MASK_AV1    (SCM_AV1_MAIN8 | SCM_AV1_MAIN10 | SCM_AV1_HIGH8_444 | SCM_AV1_HIGH10_444)
#define SCM_MASK_10BIT  (SCM_HEVC_MAIN10 | SCM_HEVC_REXT10_444 | SCM_AV1_MAIN10 | SCM_AV1_HIGH10_444)
#define SCM_MASK_YUV444 (SCM_H264_HIGH8_444 | SCM_HEVC_REXT8_444 | SCM_HEVC_REXT10_444 | SCM_AV1_HIGH8_444 | SCM_AV1_HIGH10_444)

typedef struct _SERVER_INFORMATION {
    // 服务器主机名或 IP 地址（文本形式）
    const char* address;

    // /serverinfo 中 'appversion' 标签内的文本
    const char* serverInfoAppVersion;

    // /serverinfo 中 'GfeVersion' 标签内的文本（如果存在）
    const char* serverInfoGfeVersion;

    // /resume 和 /launch 中 'sessionUrl0' 标签内的文本（如果存在）
    const char* rtspSessionUrl;

    // 指定 /serverinfo 响应中的 'ServerCodecModeSupport'。
    int serverCodecModeSupport;
} SERVER_INFORMATION, *PSERVER_INFORMATION;

// 当服务器信息分配到栈或堆上时，使用此函数将其归零
void LiInitializeServerInformation(PSERVER_INFORMATION serverInfo);

此函数开始流式传输。回调都是可选的。对于每个结构体中的单个回调传入NULL，或者对整个结构体传入NULL，以使用所有回调的默认值。此函数不是线程安全的。
int LiStartConnection(PSERVER_INFORMATION serverInfo, PSTREAM_CONFIGURATION streamConfig, PCONNECTION_LISTENER_CALLBACKS clCallbacks,
    PDECODER_RENDERER_CALLBACKS drCallbacks, PAUDIO_RENDERER_CALLBACKS arCallbacks, void* renderContext, int drFlags,
    void* audioContext, int arFlags);

// 此功能会停止流媒体播放。此功能线程不安全。
void LiStopConnection(void);

此函数会中断正在进行的 LiStartConnection() 调用。此中断是异步发生的，因此在第一个 LiStartConnection() 调用返回之前启动另一个连接是不安全的。
void LiInterruptConnection(void);

用于获取一个用户可见的字符串，以显示从传递给 ConnListenerStageXXX 回调的整数的初始化进度
const char* LiGetStageName(int stage);

此函数返回通过 ENet 协议统计获得的到主机 PC 的当前 RTT 估算值。如果当前 GFE 版本不使用 ENet 进行控制流（非常旧的版本），或者 ENet 对等端未连接，则此函数将失败。此函数仅可在 LiStartConnection() 和 LiStopConnection() 之间调用。
bool LiGetEstimatedRttInfo(uint32_t* estimatedRtt, uint32_t* estimatedRttVariance);

// 此功能会将相对鼠标移动事件排队，以发送到远程服务器。
int LiSendMouseMoveEvent(short deltaX, short deltaY);

此功能会将鼠标位置更新事件排入队列，以发送到远程服务器。
此功能仅在 GFE 3.20 或更高版本上可靠支持。早期版本可能无法正确定位鼠标。
在许多游戏中绝对鼠标移动无法使用，因此在流式传输时不应将此模式作为鼠标的默认模式。当 LiSendTouchEvent() 不受支持且触摸屏不是主要输入方式时，将其作为默认触摸屏行为可能是可取的。
在后一种情况下，使用 LiSendMouseMoveEvent() 的触摸屏作为触控板模式可能更适合游戏场景。
x 和 y 值会被转换为主机坐标，仿佛它们来自一个大小为 referenceWidth × referenceHeight 的平面。这使您可以提供相对于任意平面的坐标，例如窗口、屏幕或缩放视频视图。
例如，如果您想直接将窗口坐标作为 x 和 y 传递，则应设置
将 referenceWidth 和 referenceHeight 设置为你的窗口宽度和高度。
int LiSendMousePositionEvent(short x, short y, short referenceWidth, short referenceHeight);

此函数将鼠标位置更新事件排队，发送到远程服务器，因此上面提到的 LiSendMousePositionEvent() 的所有限制在这里同样适用！

此函数的行为类似于 LiSendMouseMoveEvent() 和 LiSendMousePositionEvent() 的组合，它发送相对移动事件，但这些数据是作为绝对位置发送的，基于虚拟客户端光标的计算位置。每当调用 LiSendMousePositionEvent() 或 LiSendMouseMoveAsMousePositionEvent() 时，该光标都会“移动”。因此，由于这种内部虚拟光标状态，调用者必须确保 LiSendMousePositionEvent() 和 LiSendMouseMoveAsMousePositionEvent() 不会同时被调用！

这个函数的主要优点是，它允许调用者避免鼠标加速，这种加速会影响使用 LiSendMouseMoveEvent() 时的移动。缺点是，它具有与 LiSendMousePositionEvent() 相同的游戏兼容性问题。
当鼠标捕获是接收鼠标输入的唯一可行方式时，例如在 Android 或 iOS 上，并且操作系统在捕获时无法提供未经加速的原始鼠标运动时，这个函数可能会非常有用。在客户端的鼠标移动也被加速的情况下，使用此函数可以避免双重加速。
int LiSendMouseMoveAsMousePositionEvent(short deltaX, short deltaY, short referenceWidth, short referenceHeight);

// 错误返回值表示主机不支持所请求的功能
#define LI_ERR_UNSUPPORTED -5501

此功能允许多点触控输入直接发送到 Sunshine 主机。x 和 y 值是标准化的设备坐标，从视频区域的左上角 (0.0, 0.0) 延伸到右下角 (1.0, 1.0)。
指针 ID 是一个不透明的 ID，必须唯一标识屏幕上的每一个活动触点。在一次触控操作中，任何按下/抬起/移动/取消事件中，该 ID 必须保持不变。
旋转角度以 Y 轴方向的垂直度数表示（平行于屏幕，0..360）。如果旋转未知，请传入 LI_ROT_UNKNOWN。
压力值范围是 0.0 到 1.0，从最小到最大压力。发送压力为 0.0 的按下/移动事件表示实际压力未知。
对于悬停事件，压力值被视为距离触控表面的 1.0 到 0.0 范围，其中 1.0 表示可测量的最远距离，而 0.0 表示实际接触显示屏（对于悬停事件无效）。悬停事件报告距离为 0.0 表示
实际距离未知。接触区域被建模为一个椭圆，其长轴和短轴的数值以归一化设备坐标表示。如果接触区域未知，请将两个接触区域轴参数都报告为 0.0。对于圆形接触区域或当短轴数值不可用时，长轴和短轴使用相同的数值。对于不以椭圆形式报告接触区域的 API 或设备，可以使用近似值，例如：https://docs.kernel.org/input/multi-touch-protocol.html#event-computation。对于悬停事件，“接触区域”是悬停的手指/工具的大小。如果不可用，请将两个接触区域参数都设置为 0.0。可以使用 LI_TOUCH_EVENT_CANCEL 或 LI_TOUCH_EVENT_CANCEL_ALL 取消触摸事件。使用 LI_TOUCH_EVENT_CANCEL 时，只有 pointerId 参数有效，其他所有参数将被忽略。要取消所有活动触摸（例如在失去焦点时），请使用 LI_TOUCH_EVENT_CANCEL_ALL。
如果主机不支持，这将返回 LI_ERR_UNSUPPORTED，调用方应考虑使用其他函数发送此输入（例如 LiSendMousePositionEvent()）。要在不调用 LiSendTouchEvent() 的情况下确定其是否受支持，请调用 LiGetHostFeatureFlags() 并检查 LI_FF_PEN_TOUCH_EVENTS 标志。
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

此函数类似于 LiSendTouchEvent()，但允许使用与笔输入相关的额外参数，包括倾斜和按钮。倾斜以垂直方向的度数表示（沿 Z 方向，垂直于屏幕，范围 0..90）。有关其他参数的详细文档，请参阅 LiSendTouchEvent()。对于 LI_TOUCH_EVENT_BUTTON_ONLY 事件，x、y、压力、旋转、接触面积和倾斜会被忽略。如果这些参数之一发生变化，请改为发送 LI_TOUCH_EVENT_MOVE 或 LI_TOUCH_EVENT_HOVER。要在不调用 LiSendPenEvent() 的情况下确定其是否受支持，请调用 LiGetHostFeatureFlags() 并检查 LI_FF_PEN_TOUCH_EVENTS 标志。
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

// 此功能将鼠标按钮事件排队，以发送到远程服务器。
#define BUTTON_ACTION_PRESS 0x07
#define BUTTON_ACTION_RELEASE 0x08
#define BUTTON_LEFT 0x01
#define BUTTON_MIDDLE 0x02
#define BUTTON_RIGHT 0x03
#define BUTTON_X1 0x04
#define BUTTON_X2 0x05
int LiSendMouseButtonEvent(char action, int button);

此函数将键盘事件排入队列，以发送到远程服务器。键码为 Win32 虚拟键（VK）码，并按美式英语键盘布局解释为对应的按键。
#define KEY_ACTION_DOWN 0x03
#define KEY_ACTION_UP 0x04
#define MODIFIER_SHIFT 0x01
#define MODIFIER_CTRL 0x02
#define MODIFIER_ALT 0x04
#define MODIFIER_META 0x08
int LiSendKeyboardEvent(short keyCode, char keyAction, char modifiers);

类似于 LiSendKeyboardEvent()，但允许客户端通知主机该按键码未映射到标准美式英语扫描码，应按原样解释。这是 Sunshine 协议的扩展。
#define SS_KBE_FLAG_NON_NORMALIZED 0x01
int LiSendKeyboardEvent2(short keyCode, char keyAction, char modifiers, char flags);

// 此函数将 UTF-8 编码的文本排队发送到远程服务器。
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

// 扩展按钮（仅限 Sunshine）
#define PADDLE1_FLAG  0x010000
#define PADDLE2_FLAG  0x020000
#define PADDLE3_FLAG  0x040000
#define PADDLE4_FLAG  0x080000
#define TOUCHPAD_FLAG 0x100000 // 索尼手柄上的触控板按钮
#define MISC_FLAG     0x200000 // 各类控制器上的分享/麦克风/捕捉/静音按钮

此功能将控制器事件排入队列，以发送到远程服务器。计算机将其视为第一个控制器。
int LiSendControllerEvent(int buttonFlags, unsigned char leftTrigger, unsigned char rightTrigger,
    short leftStickX, short leftStickY, short rightStickX, short rightStickY);

此函数将控制器事件排队以发送到远程服务器。controllerNumber 参数是该事件对应的控制器的从零开始的索引。对于 GFE 主机，最大合法控制器编号为 3，对于 Sunshine 主机，最大为 15。在第 3 代服务器（GFE 2.1.x）上，无论 controllerNumber 参数为何，这些事件都将作为控制器 0 发送。
activeGamepadMask 参数是一个位字段，每个存在的控制器对应的位将被设置。
在 GFE 上，activeGamepadMask 的最大位数限制为 4 位（0xF）。
在 Sunshine 上，它的限制为 16 位（0xFFFF）。
为了表示游戏手柄的到达，你可以发送一个空事件，将控制器编号设置为新控制器，并在 activeGamepadMask 中设置新控制器的位。
然而，对于此目的，你应优先使用 LiSendControllerArrivalEvent() 而不是此函数，因为它允许主机更好地选择模拟控制器。
要表示移除手柄，请发送一个空事件，并将控制器编号设置为已移除的控制器，同时在活动手柄掩码中清除已移除控制器的位。
int LiSendMultiControllerEvent(short controllerNumber, short activeGamepadMask,
    int buttonFlags, unsigned char leftTrigger, unsigned char rightTrigger,
    short leftStickX, short leftStickY, short rightStickX, short rightStickY);

此功能提供了一种方法，用于向主机告知新控制器上的可用按键和功能。这是指示新控制器到来的推荐方法。这样可以让主机更好地决定模拟何种类型的控制器以及向操作系统在虚拟控制器上显示哪些功能。如果主机不支持控制器到达事件，则将退回为通过 LiSendMultiControllerEvent() 指示到达。
#define LI_CTYPE_UNKNOWN  0x00
#define LI_CTYPE_XBOX     0x01
#define LI_CTYPE_PS       0x02
#define LI_CTYPE_NINTENDO 0x03
#define LI_CCAP_ANALOG_TRIGGERS 0x01 // 报告触发轴的值在 0x00 到 0xFF 之间
#define LI_CCAP_RUMBLE          0x02 // 可以响应 ConnListenerRumble() 回调震动
#define LI_CCAP_TRIGGER_RUMBLE  0x04 // 可以在 ConnListenerRumbleTriggers() 回调中触发震动
#define LI_CCAP_TOUCHPAD        0x08 // 通过 LiSendControllerTouchEvent() 报告触控板事件
#define LI_CCAP_ACCEL           0x10 // 可以通过 LiSendControllerMotionEvent() 报告加速度计事件
#define LI_CCAP_GYRO            0x20 // 可以通过 LiSendControllerMotionEvent() 报告陀螺仪事件
#define LI_CCAP_BATTERY_STATE   0x40 // 通过 LiSendControllerBatteryEvent() 报告电池状态
#define LI_CCAP_RGB_LED         0x80 // 可以通过 ConnListenerSetControllerLED() 设置 RGB LED 状态
int LiSendControllerArrivalEvent(uint8_t controllerNumber, uint16_t activeGamepadMask, uint8_t type,
                                 uint32_t supportedButtonFlags, uint16_t capabilities);

此函数类似于 LiSendTouchEvent()，但触摸事件是与游戏手柄上的触控板设备相关联的，而不是触摸屏。如果主机不支持此功能，将返回 LI_ERR_UNSUPPORTED，调用方应考虑使用此触摸输入来模拟触控板输入。要在不调用 LiSendControllerTouchEvent() 的情况下确定其是否受支持，请调用 LiGetHostFeatureFlags() 并检查 LI_FF_CONTROLLER_TOUCH_EVENTS 标志。
int LiSendControllerTouchEvent(uint8_t controllerNumber, uint8_t eventType, uint32_t pointerId, float x, float y, float pressure);

此功能允许客户端将与控制器相关的运动事件发送到支持的主机。出于功率和性能的考虑，除非主机通过 ConnListenerSetMotionEventState() 明确请求运动事件报告，否则不应启用运动传感器。
LI_MOTION_TYPE_ACCEL 应报告 m/s² 的数据（包含重力加速度）。
LI_MOTION_TYPE_GYRO 应报告 deg/s 的数据。
x/y/z 轴的分配遵循 SDL 的惯例，文档链接如下：https://github.com/libsdl-org/SDL/blob/96720f335002bef62115e39327940df454d78f6c/include/SDL3/SDL_sensor.h#L80-L124
#define LI_MOTION_TYPE_ACCEL 0x01
#define LI_MOTION_TYPE_GYRO  0x02
int LiSendControllerMotionEvent(uint8_t controllerNumber, uint8_t motionType, float x, float y, float z);

此功能允许客户端将控制器电池状态发送到支持的主机。如果主机可以调整模拟控制器的电池状态，它可以使用此信息使虚拟控制器与客户端的物理控制器匹配。
#define LI_BATTERY_STATE_UNKNOWN      0x00
#define LI_BATTERY_STATE_NOT_PRESENT  0x01
#define LI_BATTERY_STATE_DISCHARGING  0x02
#define LI_BATTERY_STATE_CHARGING     0x03
#define LI_BATTERY_STATE_NOT_CHARGING 0x04 // 已连接电源但未充电
#define LI_BATTERY_STATE_FULL         0x05
#define LI_BATTERY_PERCENTAGE_UNKNOWN 0xFF
int LiSendControllerBatteryEvent(uint8_t controllerNumber, uint8_t batteryState, uint8_t batteryPercentage);

此功能将垂直滚动事件排队发送到远程服务器。发送到 PC 之前，“点击”次数会乘以 WHEEL_DELTA（120）。
int LiSendScrollEvent(signed char scrollClicks);

此函数将垂直滚动事件排入远程服务器的队列。与 LiSendScrollEvent() 不同，此函数可以为支持“高分辨率”滚动的设备（如 Apple 触控板、Microsoft 精准触控板等）发送小于 120 单位的滚轮事件。
int LiSendHighResScrollEvent(short scrollAmount);

这些函数将水平滚动事件发送到主机，这些事件类似于 LiSendScrollEvent() 和 LiSendHighResScrollEvent()。这是 Sunshine 协议的扩展。
int LiSendHScrollEvent(signed char scrollClicks);
int LiSendHighResHScrollEvent(short scrollAmount);

此函数返回微秒级时间，其纪元由实现定义。它只应与之前对自身调用的返回值进行比较。
uint64_t LiGetMicroseconds(void);

此函数返回以毫秒为单位的时间，使用实现定义的纪元。它只应与先前对自身的调用返回值进行比较。
uint64_t LiGetMillis(void);

这是一个简单的 STUN 功能，可以帮助客户端获取它们通过 IPv4 使用 mDNS 发现的机器的 WAN 地址。 在 GFE 停止发送外部地址一段时间后，这可以用于预先填充流媒体的外部地址。wanAddr 以网络字节序返回。
int LiFindExternalAddressIP4(const char* stunServer, unsigned short stunPort, unsigned int* wanAddr);

返回准备好传递的排队视频帧数量。仅当视频渲染器未设置 CAPABILITY_DIRECT_SUBMIT 时才相关。
int LiGetPendingVideoFrames(void);

返回准备交付的排队音频帧数量。仅当音频渲染器未设置 CAPABILITY_DIRECT_SUBMIT 时相关。对于大多数用途，LiGetPendingAudioDuration() 可能比此函数更合适。
int LiGetPendingAudioFrames(void);

类似于 LiGetPendingAudioFrames()，只是它返回的是以毫秒为单位的待处理音频，而不是帧，这使调用者无需关心协商的音频帧时长。
int LiGetPendingAudioDuration(void);

返回指向包含有关 RTP 音频流的各种统计信息的结构体的指针。数据应视为只读，不能修改。
typedef struct _RTP_AUDIO_STATS {
    uint32_t packetCountAudio;         // 总音频包
    uint32_t packetCountFec;           // FEC类型的数据包总数
    uint32_t packetCountFecRecovered;  // 一个包已保存
    uint32_t packetCountFecFailed;     // 试图恢复但损失太大
    uint32_t packetCountOOS;           // 乱序数据包
    uint32_t packetCountInvalid;       // 损坏的数据包等
    uint32_t packetCountFecInvalid;    // 无效的 FEC 数据包
} RTP_AUDIO_STATS, *PRTP_AUDIO_STATS;

const RTP_AUDIO_STATS* LiGetRTPAudioStats(void);

返回一个指向结构体的指针，其中包含关于 RTP 视频流的各种统计信息。数据应视为只读，不能修改。当前，这主要用于跟踪总的视频包和 FEC 包，因为在 moonlight-qt 的更高级别已经实现了许多视频统计功能。
typedef struct _RTP_VIDEO_STATS {
    uint32_t packetCountVideo;         // 视频总包数
    uint32_t packetCountFec;           // FEC类型的数据包总数
    uint32_t packetCountFecRecovered;  // 一个包已保存
    uint32_t packetCountFecFailed;     // 试图恢复但损失太大
    uint32_t packetCountOOS;           // 乱序数据包
    uint32_t packetCountInvalid;       // 损坏的数据包等
    uint32_t packetCountFecInvalid;    // 无效的 FEC 数据包
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

// Returns the port flags that correspond to ports involved in a failing connection stage, or
// connection termination error.
//
// These may be used to specifically test the ports that could have caused the connection failure.
// If no ports are likely involved with a given failure, this function returns 0.
unsigned int LiGetPortFlagsFromStage(int stage);
unsigned int LiGetPortFlagsFromTerminationErrorCode(int errorCode);

// 返回指定端口索引的 IPPROTO_* 值
int LiGetProtocolFromPortFlagIndex(int portFlagIndex);

// 返回指定端口索引的端口号
unsigned short LiGetPortFromPortFlagIndex(int portFlagIndex);

将输出缓冲区填充为输入参数中设置的端口标志的字符串化列表。第二个及后续的条目将以“separator”（如果提供）作为前缀。如果输出缓冲区太小，输出将被截断以适应提供的缓冲区。
void LiStringifyPortFlags(unsigned int portFlags, const char* separator, char* outputBuffer, int outputBufferLength);

此功能可用于测试本地网络是否阻止了 Moonlight 的端口。它需要在可通过互联网访问的主机上运行测试服务器。要执行测试，请传入测试服务器的 DNS 主机名、参考 TCP 端口以确保测试主机可以访问（选择不太可能被阻止的端口，例如 80 或 443），以及一组对应要测试端口的 ML_PORT_FLAG_* 值。返回时，如果发生严重错误，会返回 ML_TEST_RESULT_INCONCLUSIVE；否则返回验证失败的端口标志集合。如果所有端口验证成功，则返回 0。建议不要显式使用端口标志（因为 GameStream 的端口将来可能会变化），而是在连接失败时使用 ML_PORT_FLAG_ALL 或 LiGetPortFlagsFromStage()。测试服务器可在 https://github.com/cgutman/gfe-loopback 获取。
#define ML_TEST_RESULT_INCONCLUSIVE 0xFFFFFFFF
unsigned int LiTestClientConnectivity(const char* testServer, unsigned short referencePort, unsigned int testPortFlags);

这类函数可用于选择自行管理解码/渲染线程的拉取式视频渲染器。在成功调用用于出队视频帧的 WaitFor/Poll 变体之后，必须调用 LiCompleteVideoFrame() 来通知处理已完成。从 drSubmitDecodeUnit() 返回的相同 DR_* 状态值必须作为 drStatus 参数传递给 LiCompleteVideoFrame()。为了安全使用这些函数，必须在视频解码器上设置 CAPABILITY_PULL_RENDERER。
typedef void* VIDEO_FRAME_HANDLE;
bool LiWaitForNextVideoFrame(VIDEO_FRAME_HANDLE* frameHandle, PDECODE_UNIT* decodeUnit);
bool LiPollNextVideoFrame(VIDEO_FRAME_HANDLE* frameHandle, PDECODE_UNIT* decodeUnit);
bool LiPeekNextVideoFrame(PDECODE_UNIT* decodeUnit);
void LiWakeWaitForVideoFrame(void);
void LiCompleteVideoFrame(VIDEO_FRAME_HANDLE handle, int drStatus);

// 此功能返回主机 PC 最近报告的 HDR 模式。
// 有关详细信息，请参见 ConnListenerSetHdrMode()。
bool LiGetCurrentHostDisplayHdrMode(void);

typedef struct _SS_HDR_METADATA {
    // RGB顺序
    struct {
        uint16_t x; // 标准化为50,000
        uint16_t y; // 标准化为50,000
    } displayPrimaries[3];

    struct {
        uint16_t x; // 标准化为50,000
        uint16_t y; // 标准化为50,000
    } whitePoint;

    uint16_t maxDisplayLuminance; // 虱卵
    uint16_t minDisplayLuminance; // 一万分之一尼特

    // 这些是特定内容的值，可能并非所有主机都有。
    uint16_t maxContentLightLevel; // 虱卵
    uint16_t maxFrameAverageLightLevel; // 虱卵

    // 这些是特定于显示器的值，可能并非所有主机都可用。
    uint16_t maxFullFrameLuminance; // 虱卵
} SS_HDR_METADATA, *PSS_HDR_METADATA;

此函数会用来自主机 PC 显示器和内容的 HDR 元数据填充提供的母带元数据结构（如果可用）。只有在主机处于 HDR 模式时调用此函数才有效。这是 Sunshine 协议的扩展。
bool LiGetHdrMetadata(PSS_HDR_METADATA metadata);

此函数向主机请求一个 IDR 帧。通常使用 DR_NEED_IDR 来完成此操作，但异步处理帧的客户端即使在前一帧返回 DR_OK 后，也可能需要重置解码器状态。与其等待新帧并为其返回 DR_NEED_IDR，不如直接调用此 API。请注意，此函数并不能保证*下一帧*就是 IDR 帧，只是保证很快会有 IDR 帧到来。
void LiRequestIdrFrame(void);

// 此功能返回主机支持的任何扩展功能标志。
#define LI_FF_PEN_TOUCH_EVENTS        0x01 // 支持 LiSendTouchEvent()/LiSendPenEvent()
#define LI_FF_CONTROLLER_TOUCH_EVENTS 0x02 // LiSendControllerTouchEvent() 已支持
uint32_t LiGetHostFeatureFlags(void);

#ifdef __cplusplus
}
#endif
