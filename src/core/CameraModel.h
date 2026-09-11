#pragma once

// 一台虚拟相机的可序列化状态：身份、网络、用户表、profiles、能力开关。
// 场景文件（Scenario）与 REST 控制面直接读写这个结构。
// 协议 handler 只读它 + 各状态机（PtzState / ImagingState / EventEngine）。

#include <QtCore/QJsonObject>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtNetwork/QHostAddress>

namespace onvifsim {

// 继电器 / 数字输入 / 音频输出的 token。Device 服务、DeviceIO 服务与事件层
// 必须报同一个串（客户端拿它去关联事件与控制端点），散在三处迟早会漂。
namespace iotoken {
inline constexpr const char *Relay = "RelayOutput_1";
inline constexpr const char *DigitalInput = "DigitalInput_1";
inline constexpr const char *AudioOutput = "AudioOutput_1";
} // namespace iotoken

enum class UserLevel { Administrator, Operator, User, Anonymous };

struct User {
    QString username;
    QString password;
    UserLevel level = UserLevel::Administrator;
};

struct VideoSourceConfig {
    QString token = QStringLiteral("VideoSourceConfig");
    QString name = QStringLiteral("VideoSourceConfig");
    QString sourceToken = QStringLiteral("VideoSource_1");
    int useCount = 3;
    int boundsX = 0;
    int boundsY = 0;
    int boundsWidth = 1920;
    int boundsHeight = 1080;
};

struct VideoEncoderConfig {
    QString token = QStringLiteral("VideoEncoder_1");
    QString name = QStringLiteral("VideoEncoder_1");
    QString encoding = QStringLiteral("H264");   // H264 / H265 / JPEG
    int width = 1920;
    int height = 1080;
    double frameRate = 15.0;
    int bitrateKbps = 4096;
    int govLength = 15;
    int quality = 5;
    QString h264Profile = QStringLiteral("Baseline");  // Baseline / Main / High
    int sessionTimeoutSec = 60;
};

struct AudioEncoderConfig {
    QString token = QStringLiteral("AudioEncoder_1");
    QString name = QStringLiteral("AudioEncoder_1");
    QString encoding = QStringLiteral("G711");   // G711 / G726 / AAC
    int bitrateKbps = 64;
    int sampleRateKhz = 8;
};

struct AudioSourceConfig {
    QString token = QStringLiteral("AudioSourceConfig");
    QString name = QStringLiteral("AudioSourceConfig");
    QString sourceToken = QStringLiteral("AudioSource_1");
};

struct AudioOutputConfig {
    QString token = QStringLiteral("AudioOutputConfig");
    QString name = QStringLiteral("AudioOutputConfig");
    QString outputToken = QStringLiteral("AudioOutput_1");
    QString sendPrimacy = QStringLiteral("www.onvif.org/ver20/HalfDuplex/Server");
    int outputLevel = 50;
};

struct AudioDecoderConfig {
    QString token = QStringLiteral("AudioDecoderConfig");
    QString name = QStringLiteral("AudioDecoderConfig");
};

struct MetadataConfig {
    QString token = QStringLiteral("MetadataConfig");
    QString name = QStringLiteral("MetadataConfig");
    bool analytics = true;
    bool events = true;
    bool ptzStatus = true;
};

// 一条媒体 profile。RTSP 路径由 Persona 决定风格，这里存最终串。
struct MediaProfile {
    QString token = QStringLiteral("Profile_1");
    QString name = QStringLiteral("MainStream");
    bool fixed = true;

    VideoSourceConfig videoSource;
    VideoEncoderConfig videoEncoder;

    bool hasAudio = true;
    AudioSourceConfig audioSource;
    AudioEncoderConfig audioEncoder;

    // PTZConfiguration 挂不挂由 quirk C1 / C2 与预设决定。
    bool hasPtz = true;
    QString ptzConfigToken = QStringLiteral("PTZConfig_1");

    // 对讲相关：客户端靠 AddAudioOutputConfiguration 绑定，必须真的改状态。
    bool hasAudioOutput = false;
    AudioOutputConfig audioOutput;
    bool hasAudioDecoder = false;
    AudioDecoderConfig audioDecoder;

    bool hasMetadata = true;
    MetadataConfig metadata;

    QString streamPath;   // "/Streaming/Channels/101"，空则按 Persona 生成
    // 内嵌样片档位："360p" / "720p" / "1080p"，或外部文件绝对路径。
    QString mediaAsset = QStringLiteral("1080p");
};

// PTZ 节点声明。四档能力表现由 quirk C3 / C4 控制。
struct PtzNodeConfig {
    QString nodeToken = QStringLiteral("PTZNode_1");
    QString nodeName = QStringLiteral("PTZNode");
    QString configToken = QStringLiteral("PTZConfig_1");
    QString configName = QStringLiteral("PTZConfig");
    bool supportsContinuous = true;
    bool supportsAbsolute = true;
    bool supportsRelative = true;
    bool supportsZoom = true;
    bool homeSupported = true;
    int maxPresets = 300;
};

struct ImagingCapabilities {
    bool brightness = true;
    bool contrast = true;
    bool saturation = true;
    bool sharpness = true;
    bool irCutFilter = true;
    bool whiteLight = false;   // 补光灯 / 白光灯，与厂商私有接口联动
    bool focus = false;
};

class CameraModel
{
public:
    CameraModel();

    // ---- 身份 ----
    QString id;                  // 进程内唯一 id，如 "cam1"
    QString displayName = QStringLiteral("Camera 1");
    QString personaKey = QStringLiteral("generic");

    QString manufacturer = QStringLiteral("ONVIFSim");
    QString model = QStringLiteral("Virtual Camera");
    QString firmwareVersion = QStringLiteral("1.0.0");
    QString serialNumber;
    QString hardwareId = QStringLiteral("ONVIFSIM-HW");

    // WS-Discovery 去重键，单次运行内固定。
    QString endpointReference;   // "urn:uuid:..."
    QStringList scopes;          // 完整 scope URI 列表
    QString hostname = QStringLiteral("onvifsim");
    QString location = QStringLiteral("lab");

    // ---- 网络 ----
    QHostAddress bindAddress = QHostAddress(QHostAddress::AnyIPv4);
    quint16 httpPort = 8000;
    quint16 rtspPort = 8554;
    // 对外宣称的地址；空表示按请求的 Host 头 / 绑定地址推断。
    QString advertisedHost;
    quint16 advertisedHttpPort = 0;
    quint16 advertisedRtspPort = 0;
    QString macAddress;          // 独立 IP 模式下与宿主网卡相同

    // ---- 用户 ----
    QList<User> users;
    const User *findUser(const QString &username) const;
    bool checkPassword(const QString &username, const QString &password) const;
    UserLevel levelOf(const QString &username) const;

    // ---- 媒体 ----
    QList<MediaProfile> profiles;
    MediaProfile *profileByToken(const QString &token);
    const MediaProfile *profileByToken(const QString &token) const;

    PtzNodeConfig ptzNode;
    ImagingCapabilities imaging;

    // ---- 能力开关（决定装哪些服务）----
    bool hasPtzService = true;
    bool hasImagingService = true;
    bool hasEventsService = true;
    bool hasAnalyticsService = true;
    bool hasDeviceIoService = true;
    bool hasMedia2Service = true;
    bool hasAudioBackchannel = true;
    bool hasRelayOutputs = true;
    bool hasDigitalInputs = true;

    // ---- 运行控制 ----
    bool enabled = true;         // false = 停在离线状态

    // ---- 序列化 ----
    QJsonObject toJson() const;
    static CameraModel fromJson(const QJsonObject &obj, QStringList *errors = nullptr);

    // 按 persona + 序号填一台默认相机（三个 profile、一个 admin 用户、标准 scopes）。
    static CameraModel makeDefault(const QString &id, const QString &personaKey, int index);

    // profile 命名风格，见 quirk B1。客户端判定主 / 子码流只看 Name 字串是否含
    // main|primary|high / sub|secondary|low，所以换个风格就能让它判错。
    static QString profileNameForStyle(const QString &style, int index);

    static QString userLevelName(UserLevel level);
    static UserLevel userLevelFromName(const QString &name, bool *ok = nullptr);
};

} // namespace onvifsim
