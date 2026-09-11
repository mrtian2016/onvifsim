#include "services/media2/Media2Service.h"

#include "core/VirtualCamera.h"
#include "services/ProfileQuirks.h"
#include "soap/Namespaces.h"

#include <QtCore/QList>
#include <QtCore/QStringList>

namespace onvifsim {
namespace {

// 限定名拼装。值参数一律走 QStringLiteral —— 裸字面量会被
// element(QString, bool) 抢走重载（const char* → bool 是标准转换）。
inline QString tt(const char *name)
{
    return QStringLiteral("tt:") + QLatin1String(name);
}

inline QString tr2(const char *name)
{
    return QStringLiteral("tr2:") + QLatin1String(name);
}

using profilequirks::declaredResolution;
using profilequirks::kMaxProfiles;
using profilequirks::ptSeconds;

CameraModel *modelOf(SoapContext &ctx)
{
    if (!ctx.camera) {
        SoapFault f;
        f.subcode = QString::fromLatin1(ter::Receiver);
        f.reason = QStringLiteral("no camera bound to this service");
        f.senderFault = false;
        ctx.fault(f);
        return nullptr;
    }
    return &ctx.camera->mutableModel();
}

// Media2 的 ProfileToken 是可选的：不带就返回全部 profile。
// 带了但找不到才是错。
int profileIndexArg(SoapContext &ctx, const CameraModel &model, bool required)
{
    const QString token = ctx.arg(QStringLiteral("ProfileToken"));
    if (token.isEmpty()) {
        if (!required)
            return -1;
        ctx.fault(QString::fromLatin1(ter::NoProfile),
                  QStringLiteral("ProfileToken is required"));
        return -2;
    }
    for (int i = 0; i < model.profiles.size(); ++i) {
        if (model.profiles.at(i).token == token)
            return i;
    }
    ctx.fault(soap::noProfile(token));
    return -2;
}

// ---------------------------------------------------------------------------
// 配置元素
// ---------------------------------------------------------------------------

void writeMulticast(XmlWriter &w)
{
    XmlWriter::Scope mc(w, tt("Multicast"));
    {
        XmlWriter::Scope addr(w, tt("Address"));
        w.element(tt("Type"), QStringLiteral("IPv4"));
        w.element(tt("IPv4Address"), QStringLiteral("0.0.0.0"));
    }
    w.element(tt("Port"), 0);
    w.element(tt("TTL"), 1);
    w.element(tt("AutoStart"), false);
}

void writeVideoSource(XmlWriter &w, const QString &qname, const VideoSourceConfig &c)
{
    XmlWriter::Scope s(w, qname);
    w.attr(QStringLiteral("token"), c.token);
    w.element(tt("Name"), c.name);
    w.element(tt("UseCount"), c.useCount);
    w.element(tt("SourceToken"), c.sourceToken);
    {
        XmlWriter::Scope b(w, tt("Bounds"));
        w.attr(QStringLiteral("x"), QString::number(c.boundsX));
        w.attr(QStringLiteral("y"), QString::number(c.boundsY));
        w.attr(QStringLiteral("width"), QString::number(c.boundsWidth));
        w.attr(QStringLiteral("height"), QString::number(c.boundsHeight));
    }
}

// Media2 的 VideoEncoder2Configuration：GovLength / Profile 是属性而不是子元素，
// 这是它跟 ver10 最容易写错的一处差别。
void writeVideoEncoder(XmlWriter &w, const QString &qname, const VideoEncoderConfig &c,
                       const Quirks &q)
{
    int width = 0;
    int height = 0;
    declaredResolution(c, q, &width, &height);

    XmlWriter::Scope s(w, qname);
    w.attr(QStringLiteral("token"), c.token);
    w.attr(QStringLiteral("GovLength"), QString::number(c.govLength));
    w.attr(QStringLiteral("Profile"), c.h264Profile);
    w.element(tt("Name"), c.name);
    w.element(tt("UseCount"), 1);
    w.element(tt("Encoding"), c.encoding);
    w.resolution(tt("Resolution"), width, height);
    {
        XmlWriter::Scope rc(w, tt("RateControl"));
        w.attr(QStringLiteral("ConstantBitRate"), QStringLiteral("false"));
        // ver20 的 FrameRateLimit 是 float，不必像 ver10 那样取整。
        w.element(tt("FrameRateLimit"), c.frameRate);
        w.element(tt("BitrateLimit"), c.bitrateKbps);
    }
    writeMulticast(w);
    w.element(tt("Quality"), c.quality);
}

void writeAudioSource(XmlWriter &w, const QString &qname, const AudioSourceConfig &c)
{
    XmlWriter::Scope s(w, qname);
    w.attr(QStringLiteral("token"), c.token);
    w.element(tt("Name"), c.name);
    w.element(tt("UseCount"), 1);
    w.element(tt("SourceToken"), c.sourceToken);
}

// ver10 用 ONVIF 自己的编码名（G711 / AAC），ver20 改用 IANA 的媒体子类型
// （PCMU / MP4A-LATM）。同一份模型两边要各自翻译。
QString media2AudioEncoding(const QString &onvifName)
{
    if (onvifName == QLatin1String("G711"))
        return QStringLiteral("PCMU");
    if (onvifName == QLatin1String("AAC"))
        return QStringLiteral("MP4A-LATM");
    return onvifName;   // G722 / G726 两边同名
}

void writeAudioEncoder(XmlWriter &w, const QString &qname, const AudioEncoderConfig &c)
{
    XmlWriter::Scope s(w, qname);
    w.attr(QStringLiteral("token"), c.token);
    w.element(tt("Name"), c.name);
    w.element(tt("UseCount"), 1);
    w.element(tt("Encoding"), media2AudioEncoding(c.encoding));
    writeMulticast(w);
    w.element(tt("Bitrate"), c.bitrateKbps);
    w.element(tt("SampleRate"), c.sampleRateKhz);
}

void writeAudioOutput(XmlWriter &w, const QString &qname, const AudioOutputConfig &c)
{
    XmlWriter::Scope s(w, qname);
    w.attr(QStringLiteral("token"), c.token);
    w.element(tt("Name"), c.name);
    w.element(tt("UseCount"), 1);
    w.element(tt("OutputToken"), c.outputToken);
    w.element(tt("SendPrimacy"), c.sendPrimacy);
    w.element(tt("OutputLevel"), c.outputLevel);
}

void writeAudioDecoder(XmlWriter &w, const QString &qname, const AudioDecoderConfig &c)
{
    XmlWriter::Scope s(w, qname);
    w.attr(QStringLiteral("token"), c.token);
    w.element(tt("Name"), c.name);
    w.element(tt("UseCount"), 1);
}

void writeMetadata(XmlWriter &w, const QString &qname, const MetadataConfig &c)
{
    XmlWriter::Scope s(w, qname);
    w.attr(QStringLiteral("token"), c.token);
    w.element(tt("Name"), c.name);
    w.element(tt("UseCount"), 1);
    if (c.ptzStatus) {
        XmlWriter::Scope ps(w, tt("PTZStatus"));
        w.element(tt("Status"), false);
        w.element(tt("Position"), true);
    }
    w.element(tt("Analytics"), c.analytics);
    writeMulticast(w);
    w.element(tt("SessionTimeout"), ptSeconds(60));
}

void writeAnalytics(XmlWriter &w, const QString &qname)
{
    XmlWriter::Scope s(w, qname);
    w.attr(QStringLiteral("token"), QStringLiteral("AnalyticsConfig"));
    w.element(tt("Name"), QStringLiteral("AnalyticsConfig"));
    w.element(tt("UseCount"), 1);
    // 最小实现：引擎与规则容器都在，但不列具体模块。
    // Frigate / HA 只要求这两个元素存在。
    w.emptyElement(tt("AnalyticsEngineConfiguration"));
    w.emptyElement(tt("RuleEngineConfiguration"));
}

void writePtz(XmlWriter &w, const QString &qname, const QString &token, const PtzNodeConfig &node)
{
    XmlWriter::Scope s(w, qname);
    w.attr(QStringLiteral("token"), token);
    w.element(tt("Name"), node.configName);
    w.element(tt("UseCount"), 1);
    w.element(tt("NodeToken"), node.nodeToken);
    w.element(tt("DefaultPTZTimeout"), ptSeconds(5));
}

void writeProfile(XmlWriter &w, const MediaProfile &p, const CameraModel &model,
                  const Quirks &q, int index)
{
    XmlWriter::Scope prof(w, tr2("Profiles"));
    w.attr(QStringLiteral("token"), p.token);
    w.attr(QStringLiteral("fixed"), p.fixed ? QStringLiteral("true") : QStringLiteral("false"));
    // Name 在 ver20 里是 tr2 命名空间的，不是 tt。
    w.element(tr2("Name"), p.name);

    XmlWriter::Scope set(w, tr2("Configurations"));
    writeVideoSource(w, tr2("VideoSource"), p.videoSource);
    if (p.hasAudio)
        writeAudioSource(w, tr2("AudioSource"), p.audioSource);
    writeVideoEncoder(w, tr2("VideoEncoder"), p.videoEncoder, q);
    if (p.hasAudio)
        writeAudioEncoder(w, tr2("AudioEncoder"), p.audioEncoder);
    if (p.hasAudioOutput)
        writeAudioOutput(w, tr2("AudioOutput"), p.audioOutput);
    if (p.hasAudioDecoder)
        writeAudioDecoder(w, tr2("AudioDecoder"), p.audioDecoder);
    if (p.hasMetadata)
        writeMetadata(w, tr2("Metadata"), p.metadata);
    writeAnalytics(w, tr2("Analytics"));
    if (profilequirks::advertisesPtz(p, q, index))
        writePtz(w, tr2("PTZ"), p.ptzConfigToken, model.ptzNode);
}

// B3：URI 多套一层。ver20 的响应本来就是一个裸 tr2:Uri，
// 这里再包一层 tt:Uri，只认顶层的客户端就会取到空。
void writeUri(XmlWriter &w, const Quirks &q, const QString &uri)
{
    if (q.isEnabled(QuirkId::StreamUriNested)) {
        XmlWriter::Scope outer(w, tr2("Uri"));
        w.element(tt("Uri"), uri);
        return;
    }
    w.element(tr2("Uri"), uri);
}

} // namespace

// ---------------------------------------------------------------------------
// 服务定义
// ---------------------------------------------------------------------------

const char *Media2Service::serviceNamespace() const
{
    return ns::Media2;
}

const char *Media2Service::serviceName() const
{
    return "media2";
}

QString Media2Service::defaultPath() const
{
    return QStringLiteral("/onvif/media2_service");
}

void Media2Service::writeServiceCapabilities(SoapContext &ctx) const
{
    XmlWriter &w = *ctx.out;
    XmlWriter::Scope caps(w, tr2("Capabilities"));
    w.attr(QStringLiteral("SnapshotUri"),
           ctx.quirks().isEnabled(QuirkId::NoGetSnapshotUri) ? QStringLiteral("false")
                                                             : QStringLiteral("true"));
    w.attr(QStringLiteral("Rotation"), QStringLiteral("false"));
    w.attr(QStringLiteral("VideoSourceMode"), QStringLiteral("true"));
    w.attr(QStringLiteral("OSD"), QStringLiteral("true"));
    {
        XmlWriter::Scope pc(w, tr2("ProfileCapabilities"));
        w.attr(QStringLiteral("MaximumNumberOfProfiles"), QString::number(kMaxProfiles));
        w.attr(QStringLiteral("ConfigurationsSupported"),
               QStringLiteral("VideoSource VideoEncoder AudioSource AudioEncoder Analytics "
                              "PTZ Metadata"));
    }
    {
        XmlWriter::Scope sc(w, tr2("StreamingCapabilities"));
        w.attr(QStringLiteral("RTSPStreaming"), QStringLiteral("true"));
        w.attr(QStringLiteral("RTPMulticast"), QStringLiteral("false"));
        w.attr(QStringLiteral("RTP_RTSP_TCP"), QStringLiteral("true"));
        w.attr(QStringLiteral("NonAggregateControl"), QStringLiteral("false"));
    }
}

Media2Service::Media2Service()
{
    op("GetProfiles", AuthLevel::User, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        // Type 参数（All / VideoSource / …）只决定回多少配置。
        // 真机普遍不管它一律全回，我们照这个来。
        const int only = profileIndexArg(ctx, *m, false);
        if (only == -2)
            return;
        XmlWriter::Scope resp(*ctx.out, tr2("GetProfilesResponse"));
        if (only >= 0) {
            writeProfile(*ctx.out, m->profiles.at(only), *m, ctx.quirks(), only);
            return;
        }
        for (int i = 0; i < m->profiles.size(); ++i)
            writeProfile(*ctx.out, m->profiles.at(i), *m, ctx.quirks(), i);
    });

    op("GetVideoEncoderConfigurations", AuthLevel::User, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        const QString wanted = ctx.arg(QStringLiteral("ConfigurationToken"));
        XmlWriter::Scope resp(*ctx.out, tr2("GetVideoEncoderConfigurationsResponse"));
        QStringList seen;
        for (const MediaProfile &p : m->profiles) {
            if (seen.contains(p.videoEncoder.token))
                continue;
            if (!wanted.isEmpty() && p.videoEncoder.token != wanted)
                continue;
            seen.append(p.videoEncoder.token);
            writeVideoEncoder(*ctx.out, tr2("Configurations"), p.videoEncoder, ctx.quirks());
        }
    });

    op("GetAnalyticsConfigurations", AuthLevel::User, [](SoapContext &ctx) {
        XmlWriter::Scope resp(*ctx.out, tr2("GetAnalyticsConfigurationsResponse"));
        writeAnalytics(*ctx.out, tr2("Configurations"));
    });

    op("GetStreamUri", AuthLevel::User, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        const int index = profileIndexArg(ctx, *m, true);
        if (index < 0)
            return;

        // ver20 把传输方式合成了一个 Protocol 字串。只认 RtspUnicast 就够，
        // 参照客户端也只发这一种。
        const QString protocol = ctx.arg(QStringLiteral("Protocol"));
        if (!protocol.isEmpty() && protocol != QLatin1String("RtspUnicast")
            && protocol != QLatin1String("RTSP")) {
            ctx.fault(soap::invalidArg(QStringLiteral("Protocol=%1").arg(protocol)));
            return;
        }

        const QString uri = ctx.camera->streamUri(m->profiles.at(index),
                                                  ctx.http ? ctx.http->peerAddress
                                                           : QHostAddress());
        XmlWriter::Scope resp(*ctx.out, tr2("GetStreamUriResponse"));
        writeUri(*ctx.out, ctx.quirks(), uri);
    });

    op("GetSnapshotUri", AuthLevel::User, [](SoapContext &ctx) {
        // B7：ver20 这边也一并不实现。Profile T 的客户端先探这里，
        // 两边都要一致才不至于让它拿到互相矛盾的结论。
        if (ctx.quirks().isEnabled(QuirkId::NoGetSnapshotUri)) {
            ctx.faultNotSupported();
            return;
        }
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        const int index = profileIndexArg(ctx, *m, true);
        if (index < 0)
            return;
        const QString uri = ctx.camera->snapshotUri(m->profiles.at(index),
                                                    ctx.http ? ctx.http->peerAddress
                                                             : QHostAddress());
        XmlWriter::Scope resp(*ctx.out, tr2("GetSnapshotUriResponse"));
        writeUri(*ctx.out, ctx.quirks(), uri);
    });

    op("GetServiceCapabilities", AuthLevel::PreAuth, [this](SoapContext &ctx) {
        XmlWriter::Scope resp(*ctx.out, tr2("GetServiceCapabilitiesResponse"));
        writeServiceCapabilities(ctx);
    });
}

namespace services {

SoapService *createMedia2()
{
    return new Media2Service;
}

} // namespace services
} // namespace onvifsim
