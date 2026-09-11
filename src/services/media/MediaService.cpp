#include "services/media/MediaService.h"

#include "core/VirtualCamera.h"
#include "services/ProfileQuirks.h"
#include "services/media/SnapshotEndpoint.h"
#include "soap/Namespaces.h"

#include <QtCore/QList>
#include <QtCore/QStringList>

namespace onvifsim {
namespace {

// 限定名拼装。写响应时满屏 QStringLiteral("tt:Xxx") 太吵，
// 而且值参数写成裸字面量会被 element(QString,bool) 悄悄吃掉（const char* → bool
// 是标准转换，优先级高于 QString 的用户定义转换），所以值一律走 QStringLiteral。
inline QString tt(const char *name)
{
    return QStringLiteral("tt:") + QLatin1String(name);
}

inline QString trt(const char *name)
{
    return QStringLiteral("trt:") + QLatin1String(name);
}

using profilequirks::declaredResolution;
using profilequirks::kMaxProfiles;
using profilequirks::ptSeconds;

using namespace profilequirks::ptzspace;

// ---------------------------------------------------------------------------
// 取模型 / 取 profile
// ---------------------------------------------------------------------------

// 所有 handler 都要相机。理论上 dispatcher 可以不挂相机（纯桩），统一挡在这里。
// 读路径也从这里拿引用，只是不改内容。
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

// 按请求里的 ProfileToken 找 profile 的下标，找不到就打 ter:NoProfile 并返回 -1。
// 返回下标而不是指针：写操作会往 profiles 里增删，指针不稳，下标好定位。
int requireProfileIndex(SoapContext &ctx, const CameraModel &model,
                        const QString &argName = QStringLiteral("ProfileToken"))
{
    const QString token = ctx.arg(argName);
    if (token.isEmpty()) {
        ctx.fault(QString::fromLatin1(ter::NoProfile),
                  QStringLiteral("%1 is required").arg(argName));
        return -1;
    }
    for (int i = 0; i < model.profiles.size(); ++i) {
        if (model.profiles.at(i).token == token)
            return i;
    }
    ctx.fault(soap::noProfile(token));
    return -1;
}

// E16：AudioOutput / AudioDecoder 相关的操作整体不实现。
// 真机上这一族操作是「要么全有要么全无」，客户端只能静默跳过再直接开 backchannel 试。
bool audioOutputUnsupported(SoapContext &ctx)
{
    if (!ctx.quirks().isEnabled(QuirkId::NoAudioOutputConfig))
        return false;
    ctx.faultNotSupported();
    return true;
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

void writeVideoSourceConfiguration(XmlWriter &w, const QString &qname, const VideoSourceConfig &c)
{
    XmlWriter::Scope s(w, qname);
    w.attr(QStringLiteral("token"), c.token);
    w.element(tt("Name"), c.name);
    w.element(tt("UseCount"), c.useCount);
    w.element(tt("SourceToken"), c.sourceToken);
    {
        // Bounds 在 schema 里是属性形态，不是子元素。
        XmlWriter::Scope b(w, tt("Bounds"));
        w.attr(QStringLiteral("x"), QString::number(c.boundsX));
        w.attr(QStringLiteral("y"), QString::number(c.boundsY));
        w.attr(QStringLiteral("width"), QString::number(c.boundsWidth));
        w.attr(QStringLiteral("height"), QString::number(c.boundsHeight));
    }
}

void writeVideoEncoderConfiguration(XmlWriter &w, const QString &qname,
                                    const VideoEncoderConfig &c, const Quirks &q)
{
    int width = 0;
    int height = 0;
    declaredResolution(c, q, &width, &height);

    XmlWriter::Scope s(w, qname);
    w.attr(QStringLiteral("token"), c.token);
    w.element(tt("Name"), c.name);
    w.element(tt("UseCount"), 1);
    w.element(tt("Encoding"), c.encoding);
    w.resolution(tt("Resolution"), width, height);
    w.element(tt("Quality"), c.quality);
    {
        XmlWriter::Scope rc(w, tt("RateControl"));
        // ver10 的 FrameRateLimit 是整数，小数帧率只能取整。
        w.element(tt("FrameRateLimit"), qRound(c.frameRate));
        w.element(tt("EncodingInterval"), 1);
        w.element(tt("BitrateLimit"), c.bitrateKbps);
    }
    if (c.encoding == QLatin1String("H264")) {
        XmlWriter::Scope h(w, tt("H264"));
        w.element(tt("GovLength"), c.govLength);
        w.element(tt("H264Profile"), c.h264Profile);
    }
    writeMulticast(w);
    w.element(tt("SessionTimeout"), ptSeconds(c.sessionTimeoutSec));
}

void writeAudioSourceConfiguration(XmlWriter &w, const QString &qname, const AudioSourceConfig &c)
{
    XmlWriter::Scope s(w, qname);
    w.attr(QStringLiteral("token"), c.token);
    w.element(tt("Name"), c.name);
    w.element(tt("UseCount"), 1);
    w.element(tt("SourceToken"), c.sourceToken);
}

// quirk CodecMismatch（E1 的一种）：ONVIF 里声明一种编码，RTP 上实发另一种。
// 「很多家用摄像头 capability 列了 G.711 / G.722 / AAC，但 backchannel SDP 只列 PCMU」——
// 这条就是让客户端撞上这个矛盾，逼它以 SDP / 实际码流为准。
QString declaredAudioEncoding(const AudioEncoderConfig &c, const Quirks &q)
{
    if (!q.isEnabled(QuirkId::CodecMismatch))
        return c.encoding;
    const QString declared = q.paramString(QuirkId::CodecMismatch, QStringLiteral("declared"));
    return declared.isEmpty() ? c.encoding : declared;
}

void writeAudioEncoderConfiguration(XmlWriter &w, const QString &qname,
                                    const AudioEncoderConfig &c, const Quirks &q)
{
    XmlWriter::Scope s(w, qname);
    w.attr(QStringLiteral("token"), c.token);
    w.element(tt("Name"), c.name);
    w.element(tt("UseCount"), 1);
    w.element(tt("Encoding"), declaredAudioEncoding(c, q));
    // 这里按规范走：Bitrate kbps、SampleRate kHz。
    // 单位与形态的那几条 quirk（E2 / E3 / E5）只落在两个 *Options 响应上。
    w.element(tt("Bitrate"), c.bitrateKbps);
    w.element(tt("SampleRate"), c.sampleRateKhz);
    writeMulticast(w);
    w.element(tt("SessionTimeout"), ptSeconds(60));
}

void writeAudioOutputConfiguration(XmlWriter &w, const QString &qname, const AudioOutputConfig &c)
{
    XmlWriter::Scope s(w, qname);
    w.attr(QStringLiteral("token"), c.token);
    w.element(tt("Name"), c.name);
    w.element(tt("UseCount"), 1);
    w.element(tt("OutputToken"), c.outputToken);
    w.element(tt("SendPrimacy"), c.sendPrimacy);
    w.element(tt("OutputLevel"), c.outputLevel);
}

void writeAudioDecoderConfiguration(XmlWriter &w, const QString &qname,
                                    const AudioDecoderConfig &c)
{
    XmlWriter::Scope s(w, qname);
    w.attr(QStringLiteral("token"), c.token);
    w.element(tt("Name"), c.name);
    w.element(tt("UseCount"), 1);
}

void writeMetadataConfiguration(XmlWriter &w, const QString &qname, const MetadataConfig &c)
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

void writePtzConfiguration(XmlWriter &w, const QString &qname, const QString &token,
                           const PtzNodeConfig &node)
{
    XmlWriter::Scope s(w, qname);
    w.attr(QStringLiteral("token"), token);
    w.element(tt("Name"), node.configName);
    w.element(tt("UseCount"), 1);
    w.element(tt("NodeToken"), node.nodeToken);
    // 「Pant」不是笔误 —— ONVIF schema 里这个元素名本身就拼错了，必须照抄。
    w.element(tt("DefaultAbsolutePantTiltPositionSpace"),
              QString::fromLatin1(kAbsolutePanTilt));
    w.element(tt("DefaultAbsoluteZoomPositionSpace"), QString::fromLatin1(kAbsoluteZoom));
    w.element(tt("DefaultRelativePanTiltTranslationSpace"),
              QString::fromLatin1(kRelativePanTilt));
    w.element(tt("DefaultRelativeZoomTranslationSpace"),
              QString::fromLatin1(kRelativeZoom));
    w.element(tt("DefaultContinuousPanTiltVelocitySpace"),
              QString::fromLatin1(kContinuousPanTilt));
    w.element(tt("DefaultContinuousZoomVelocitySpace"), QString::fromLatin1(kContinuousZoom));
    {
        XmlWriter::Scope sp(w, tt("DefaultPTZSpeed"));
        {
            XmlWriter::Scope pt(w, tt("PanTilt"));
            w.attr(QStringLiteral("x"), QStringLiteral("0.5"));
            w.attr(QStringLiteral("y"), QStringLiteral("0.5"));
            w.attr(QStringLiteral("space"), QString::fromLatin1(kPanTiltSpeed));
        }
        {
            XmlWriter::Scope z(w, tt("Zoom"));
            w.attr(QStringLiteral("x"), QStringLiteral("0.5"));
            w.attr(QStringLiteral("space"), QString::fromLatin1(kZoomSpeed));
        }
    }
    w.element(tt("DefaultPTZTimeout"), ptSeconds(5));
}

void writeProfile(XmlWriter &w, const QString &qname, const MediaProfile &p,
                  const CameraModel &model, const Quirks &q, int index)
{
    XmlWriter::Scope prof(w, qname);
    w.attr(QStringLiteral("token"), p.token);
    w.attr(QStringLiteral("fixed"), p.fixed ? QStringLiteral("true") : QStringLiteral("false"));
    // B1：命名风格由 CameraModel 按 persona 生成，这里照实写。
    // 客户端判定主 / 子码流只看这个字串，改风格就能逼它按顺序猜。
    w.element(tt("Name"), p.name);

    writeVideoSourceConfiguration(w, tt("VideoSourceConfiguration"), p.videoSource);
    if (p.hasAudio)
        writeAudioSourceConfiguration(w, tt("AudioSourceConfiguration"), p.audioSource);
    writeVideoEncoderConfiguration(w, tt("VideoEncoderConfiguration"), p.videoEncoder, q);
    if (p.hasAudio)
        writeAudioEncoderConfiguration(w, tt("AudioEncoderConfiguration"), p.audioEncoder, q);
    if (profilequirks::advertisesPtz(p, q, index))
        writePtzConfiguration(w, tt("PTZConfiguration"), p.ptzConfigToken, model.ptzNode);
    if (p.hasMetadata)
        writeMetadataConfiguration(w, tt("MetadataConfiguration"), p.metadata);

    // ver10 的 schema 把 AudioOutput / AudioDecoder 放在 Extension 下。
    // 客户端调完 AddAudioOutputConfiguration 就靠这里回读绑定结果。
    if (p.hasAudioOutput || p.hasAudioDecoder) {
        XmlWriter::Scope ext(w, tt("Extension"));
        if (p.hasAudioOutput)
            writeAudioOutputConfiguration(w, tt("AudioOutputConfiguration"), p.audioOutput);
        if (p.hasAudioDecoder)
            writeAudioDecoderConfiguration(w, tt("AudioDecoderConfiguration"), p.audioDecoder);
    }
}

// ---------------------------------------------------------------------------
// 音频能力（E1 ~ E5 全在这一段）
// ---------------------------------------------------------------------------

struct AudioCodecOption {
    QString name;              // G711 / G722 / G726 / AAC
    QList<int> sampleRates;    // Hz，升序
    QList<int> bitrates;       // kbps，升序（输出时按 E3 换算）
};

// 声明给客户端看的解码 / 编码能力。
QList<AudioCodecOption> audioCodecOptions(const Quirks &q)
{
    QList<AudioCodecOption> list;
    list.append(AudioCodecOption{ QStringLiteral("G711"), { 8000 }, { 64 } });
    // E2：G.722 实际是 16 kHz，但很多固件在 ONVIF 能力里照着 RFC 3551 的
    // RTP 时钟率错写成 8000。客户端必须强制按 16000 处理。
    const int g722Rate = q.isEnabled(QuirkId::G722SampleRate8000) ? 8000 : 16000;
    list.append(AudioCodecOption{ QStringLiteral("G722"), { g722Rate }, { 64 } });
    // E1：capability 里多列几种 backchannel SDP 根本不会开的编码。
    // 运行时必须以 SDP 为准，这条就是用来验证客户端有没有做这个校验的。
    if (q.isEnabled(QuirkId::AudioCapabilityLie)) {
        list.append(AudioCodecOption{ QStringLiteral("G726"), { 8000 }, { 16, 24, 32, 40 } });
        list.append(AudioCodecOption{ QStringLiteral("AAC"), { 16000, 48000 }, { 64, 128 } });
    }
    return list;
}

// E3：码率单位 kbps / bps，差 1000 倍。客户端的判据是「< 1024 视作 kbps」。
QList<int> bitrateValues(const QList<int> &kbps, const Quirks &q)
{
    if (!q.isEnabled(QuirkId::BitrateUnitVariant)
        || q.choice(QuirkId::BitrateUnitVariant, QStringLiteral("kbps"))
               != QLatin1String("bps")) {
        return kbps;
    }
    QList<int> out;
    out.reserve(kbps.size());
    for (int v : kbps)
        out.append(v * 1000);
    return out;
}

// E5：采样率字段形态。quirk 关着时按规范走 IntList（Items 容器）。
QString intFieldShape(const Quirks &q)
{
    if (!q.isEnabled(QuirkId::SampleRateShape))
        return QStringLiteral("list");
    return q.choice(QuirkId::SampleRateShape, QStringLiteral("range"));
}

// 按形态写一组整数：range → Min/Max；list → 若干 tt:Items；single → 裸数字。
void writeIntField(XmlWriter &w, const QString &qname, const QList<int> &values,
                   const QString &shape)
{
    if (values.isEmpty()) {
        w.emptyElement(qname);
        return;
    }
    if (shape == QLatin1String("range")) {
        w.intRange(qname, values.first(), values.last());
        return;
    }
    if (shape == QLatin1String("single")) {
        // 裸数字：真机上出现过，客户端要能直接 int() 掉。取上限那个值。
        w.element(qname, values.last());
        return;
    }
    XmlWriter::Scope s(w, qname);
    for (int v : values)
        w.element(tt("Items"), v);
}

// E4：GetAudioDecoderConfigurationOptions 的两种响应形态，客户端两种都得认。
void writeAudioDecoderOptions(XmlWriter &w, const Quirks &q)
{
    const QList<AudioCodecOption> codecs = audioCodecOptions(q);
    const QString shape = intFieldShape(q);
    const bool shapeQuirk = q.isEnabled(QuirkId::AudioDecoderOptionsShape);
    const QString form = shapeQuirk ? q.choice(QuirkId::AudioDecoderOptionsShape,
                                               QStringLiteral("a"))
                                    : QStringLiteral("a");

    XmlWriter::Scope opts(w, trt("Options"));
    if (form == QLatin1String("b")) {
        // 形态 B：AudioDecoderOptions 列表，每项自带 Encoding。
        // 字段名跟着 E5 的形态走：range 档用 *Range，其余用 *List。
        const bool useRange = shape == QLatin1String("range");
        for (const AudioCodecOption &c : codecs) {
            XmlWriter::Scope item(w, tt("AudioDecoderOptions"));
            w.element(tt("Encoding"), c.name);
            writeIntField(w, useRange ? tt("SampleRateRange") : tt("SampleRateList"),
                          c.sampleRates, shape);
            writeIntField(w, useRange ? tt("BitrateRange") : tt("BitrateList"),
                          bitrateValues(c.bitrates, q), shape);
        }
        return;
    }

    // 形态 A：每种编码一个子元素直挂在 Options 下，各含 Bitrate + SampleRateRange。
    // quirk 关着时用规范里的 G711DecOptions / G726DecOptions / AACDecOptions 名字；
    // 打开 "a" 档换成真机那种裸名字 —— 客户端正是按 G711 / G722 / G726 / AAC 找的。
    for (const AudioCodecOption &c : codecs) {
        const QString qname = shapeQuirk
                                  ? QStringLiteral("tt:") + c.name
                                  : QStringLiteral("tt:") + c.name + QStringLiteral("DecOptions");
        XmlWriter::Scope item(w, qname);
        // Bitrate 在 schema 里是 IntList，这里始终走 Items 形态，只有采样率受 E5 摆布。
        writeIntField(w, tt("Bitrate"), bitrateValues(c.bitrates, q), QStringLiteral("list"));
        writeIntField(w, tt("SampleRateRange"), c.sampleRates, shape);
    }
}

void writeAudioEncoderOptions(XmlWriter &w, const Quirks &q)
{
    const QList<AudioCodecOption> codecs = audioCodecOptions(q);
    const QString shape = intFieldShape(q);
    const bool useRange = shape == QLatin1String("range");

    XmlWriter::Scope opts(w, trt("Options"));
    for (const AudioCodecOption &c : codecs) {
        XmlWriter::Scope item(w, tt("Options"));
        w.element(tt("Encoding"), c.name);
        writeIntField(w, useRange ? tt("BitrateRange") : tt("BitrateList"),
                      bitrateValues(c.bitrates, q), shape);
        writeIntField(w, useRange ? tt("SampleRateRange") : tt("SampleRateList"),
                      c.sampleRates, shape);
    }
}

// ---------------------------------------------------------------------------
// 去重后的配置列表
// ---------------------------------------------------------------------------

// 三个 profile 常常共用同一份 VideoSourceConfiguration，列表里不能重复出现。
template <typename T, typename Getter>
QList<const T *> uniqueConfigs(const QList<MediaProfile> &profiles, Getter get)
{
    QList<const T *> out;
    QStringList seen;
    for (const MediaProfile &p : profiles) {
        const T *c = get(p);
        if (!c || seen.contains(c->token))
            continue;
        seen.append(c->token);
        out.append(c);
    }
    return out;
}

// 设备级的 AudioOutput / AudioDecoder 配置。模型把它们存在 profile 里（默认不挂），
// 这里取第一个 profile 的那份当「设备上现成可用的一份」。
AudioOutputConfig deviceAudioOutput(const CameraModel &model)
{
    return model.profiles.isEmpty() ? AudioOutputConfig() : model.profiles.first().audioOutput;
}

AudioDecoderConfig deviceAudioDecoder(const CameraModel &model)
{
    return model.profiles.isEmpty() ? AudioDecoderConfig() : model.profiles.first().audioDecoder;
}

// 分辨率 → 内嵌样片档位。客户端不信 VideoEncoderConfiguration、全靠 ffprobe 探流，
// 所以改了声明就必须同时把实际发的码流换掉，否则一探就露馅。
QString assetForResolution(int width, int height)
{
    if (height >= 900 || width >= 1600)
        return QStringLiteral("1080p");
    if (height >= 600 || width >= 1000)
        return QStringLiteral("720p");
    return QStringLiteral("360p");
}

bool isBuiltinAsset(const QString &asset)
{
    return asset == QLatin1String("360p") || asset == QLatin1String("720p")
           || asset == QLatin1String("1080p");
}

// ---------------------------------------------------------------------------
// MediaUri 响应（B3 就落在这里）
// ---------------------------------------------------------------------------

void writeMediaUriBody(XmlWriter &w, const QString &uri, int timeoutSec)
{
    w.element(tt("Uri"), uri);
    w.element(tt("InvalidAfterConnect"), false);
    w.element(tt("InvalidAfterReboot"), false);
    w.element(tt("Timeout"), ptSeconds(timeoutSec));
}

void writeMediaUri(XmlWriter &w, const Quirks &q, const QString &uri, int timeoutSec)
{
    XmlWriter::Scope outer(w, trt("MediaUri"));
    if (q.isEnabled(QuirkId::StreamUriNested)) {
        // B3：URI 套在 MediaUri/Uri 下多一层。只认顶层 Uri 的客户端会取到空串，
        // 走 MediaUri.Uri 兜底那条路径的才拿得到。
        XmlWriter::Scope inner(w, tt("MediaUri"));
        writeMediaUriBody(w, uri, timeoutSec);
        return;
    }
    writeMediaUriBody(w, uri, timeoutSec);
}

} // namespace

// ---------------------------------------------------------------------------
// 服务定义
// ---------------------------------------------------------------------------

const char *MediaService::serviceNamespace() const
{
    return ns::Media;
}

const char *MediaService::serviceName() const
{
    return "media";
}

QString MediaService::defaultPath() const
{
    return QStringLiteral("/onvif/media_service");
}

void MediaService::writeServiceCapabilities(SoapContext &ctx) const
{
    XmlWriter &w = *ctx.out;
    XmlWriter::Scope caps(w, trt("Capabilities"));
    // B7 打开时能力里也要如实说「没有快照」，否则客户端会一直白探。
    w.attr(QStringLiteral("SnapshotUri"),
           ctx.quirks().isEnabled(QuirkId::NoGetSnapshotUri) ? QStringLiteral("false")
                                                             : QStringLiteral("true"));
    w.attr(QStringLiteral("Rotation"), QStringLiteral("false"));
    w.attr(QStringLiteral("VideoSourceMode"), QStringLiteral("true"));
    w.attr(QStringLiteral("OSD"), QStringLiteral("true"));
    {
        XmlWriter::Scope pc(w, trt("ProfileCapabilities"));
        w.attr(QStringLiteral("MaximumNumberOfProfiles"), QString::number(kMaxProfiles));
    }
    {
        XmlWriter::Scope sc(w, trt("StreamingCapabilities"));
        w.attr(QStringLiteral("RTPMulticast"), QStringLiteral("false"));
        w.attr(QStringLiteral("RTP_TCP"), QStringLiteral("true"));
        w.attr(QStringLiteral("RTP_RTSP_TCP"), QStringLiteral("true"));
        w.attr(QStringLiteral("NonAggregateControl"), QStringLiteral("false"));
        w.attr(QStringLiteral("NoRTSPStreaming"), QStringLiteral("false"));
    }
}

MediaService::MediaService()
{
    registerProfileOps();
    registerVideoSourceOps();
    registerVideoEncoderOps();
    registerAudioOps();
    registerAudioOutputOps();
    registerUriOps();
    registerMiscReadOps();
    registerMulticastOps();
}

void MediaService::registerProfileOps()
{
    // ---- profile ----------------------------------------------------------

    op("GetProfiles", AuthLevel::User, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        XmlWriter::Scope resp(*ctx.out, trt("GetProfilesResponse"));
        for (int i = 0; i < m->profiles.size(); ++i)
            writeProfile(*ctx.out, trt("Profiles"), m->profiles.at(i), *m, ctx.quirks(), i);
    });

    op("GetProfile", AuthLevel::User, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        const int index = requireProfileIndex(ctx, *m);
        if (index < 0)
            return;
        XmlWriter::Scope resp(*ctx.out, trt("GetProfileResponse"));
        writeProfile(*ctx.out, trt("Profile"), m->profiles.at(index), *m, ctx.quirks(), index);
    });

    op("CreateProfile", AuthLevel::Operator, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        if (m->profiles.size() >= kMaxProfiles) {
            ctx.fault(QString::fromLatin1(ter::MaxNVTProfiles),
                      QStringLiteral("Maximum number of profiles reached"));
            return;
        }
        const QString name = ctx.arg(QStringLiteral("Name"));
        if (name.isEmpty()) {
            ctx.fault(soap::invalidArg(QStringLiteral("Name")));
            return;
        }
        QString token = ctx.arg(QStringLiteral("Token"));
        if (token.isEmpty())
            token = QStringLiteral("Profile_%1").arg(m->profiles.size() + 1);
        if (m->profileByToken(token)) {
            ctx.fault(QString::fromLatin1(ter::ProfileExists),
                      QStringLiteral("Profile token already in use: %1").arg(token));
            return;
        }

        // 新建的 profile 沿用主码流的编码配置，只换 token / 名字，
        // 客户端随后会自己 SetVideoEncoderConfiguration 调整。
        MediaProfile p = m->profiles.isEmpty() ? MediaProfile() : m->profiles.first();
        p.token = token;
        p.name = name;
        p.fixed = false;
        p.hasAudioOutput = false;
        p.hasAudioDecoder = false;
        m->profiles.append(p);
        // 先把要写进响应的那份**拷**出来，再 applyModelChanges()。
        // applyModelChanges() 会同步发 modelChanged，任何一个槽只要动了 profiles，
        // m->profiles.last() 的引用就失效了 —— 今天恰好没人动，所以还没炸。
        const MediaProfile created = m->profiles.last();
        const int createdIndex = m->profiles.size() - 1;
        ctx.camera->applyModelChanges();

        XmlWriter::Scope resp(*ctx.out, trt("CreateProfileResponse"));
        writeProfile(*ctx.out, trt("Profile"), created, *m, ctx.quirks(), createdIndex);
    });

    op("DeleteProfile", AuthLevel::Operator, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        const int index = requireProfileIndex(ctx, *m);
        if (index < 0)
            return;
        if (m->profiles.at(index).fixed) {
            ctx.fault(QString::fromLatin1(ter::DeletionOfFixedProfile),
                      QStringLiteral("Profile %1 is fixed and cannot be deleted")
                          .arg(m->profiles.at(index).token));
            return;
        }
        m->profiles.removeAt(index);
        ctx.camera->applyModelChanges();
        ctx.out->emptyElement(trt("DeleteProfileResponse"));
    });

}

void MediaService::registerVideoSourceOps()
{
    // ---- 视频源 -----------------------------------------------------------

    op("GetVideoSources", AuthLevel::User, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        XmlWriter &w = *ctx.out;
        XmlWriter::Scope resp(w, trt("GetVideoSourcesResponse"));
        QStringList seen;
        for (const MediaProfile &p : m->profiles) {
            if (seen.contains(p.videoSource.sourceToken))
                continue;
            seen.append(p.videoSource.sourceToken);
            XmlWriter::Scope s(w, trt("VideoSources"));
            w.attr(QStringLiteral("token"), p.videoSource.sourceToken);
            w.element(tt("Framerate"), p.videoEncoder.frameRate);
            w.resolution(tt("Resolution"), p.videoSource.boundsWidth, p.videoSource.boundsHeight);
        }
    });

    op("GetVideoSourceConfigurations", AuthLevel::User, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        XmlWriter::Scope resp(*ctx.out, trt("GetVideoSourceConfigurationsResponse"));
        const auto configs = uniqueConfigs<VideoSourceConfig>(
            m->profiles, [](const MediaProfile &p) { return &p.videoSource; });
        for (const VideoSourceConfig *c : configs)
            writeVideoSourceConfiguration(*ctx.out, trt("Configurations"), *c);
    });

    op("GetVideoSourceConfiguration", AuthLevel::User, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        const QString token = ctx.arg(QStringLiteral("ConfigurationToken"));
        for (const MediaProfile &p : m->profiles) {
            if (p.videoSource.token != token)
                continue;
            XmlWriter::Scope resp(*ctx.out, trt("GetVideoSourceConfigurationResponse"));
            writeVideoSourceConfiguration(*ctx.out, trt("Configuration"), p.videoSource);
            return;
        }
        ctx.fault(QString::fromLatin1(ter::NoConfig),
                  QStringLiteral("No video source configuration %1").arg(token));
    });

    op("GetVideoSourceModes", AuthLevel::User, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        XmlWriter &w = *ctx.out;
        XmlWriter::Scope resp(w, trt("GetVideoSourceModesResponse"));
        struct Mode {
            const char *token;
            int width;
            int height;
            int fps;
        };
        static const Mode modes[] = { { "VideoSourceMode_1", 1920, 1080, 30 },
                                      { "VideoSourceMode_2", 1280, 720, 30 } };
        const int currentWidth = m->profiles.isEmpty() ? 1920
                                                       : m->profiles.first().videoEncoder.width;
        for (const Mode &mode : modes) {
            XmlWriter::Scope s(w, trt("VideoSourceModes"));
            w.attr(QStringLiteral("token"), QString::fromLatin1(mode.token));
            w.attr(QStringLiteral("Enabled"), mode.width == currentWidth
                                                  ? QStringLiteral("true")
                                                  : QStringLiteral("false"));
            w.element(tt("MaxFramerate"), mode.fps);
            w.resolution(tt("MaxResolution"), mode.width, mode.height);
            w.element(tt("Encodings"), QStringLiteral("H264"));
            w.element(tt("Reboot"), false);
            w.element(tt("Description"), QStringLiteral("%1x%2").arg(mode.width).arg(mode.height));
        }
    });

}

void MediaService::registerVideoEncoderOps()
{
    // ---- 视频编码 ---------------------------------------------------------

    op("GetVideoEncoderConfigurations", AuthLevel::User, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        XmlWriter::Scope resp(*ctx.out, trt("GetVideoEncoderConfigurationsResponse"));
        const auto configs = uniqueConfigs<VideoEncoderConfig>(
            m->profiles, [](const MediaProfile &p) { return &p.videoEncoder; });
        for (const VideoEncoderConfig *c : configs)
            writeVideoEncoderConfiguration(*ctx.out, trt("Configurations"), *c, ctx.quirks());
    });

    op("GetVideoEncoderConfiguration", AuthLevel::User, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        const QString token = ctx.arg(QStringLiteral("ConfigurationToken"));
        for (const MediaProfile &p : m->profiles) {
            if (p.videoEncoder.token != token)
                continue;
            XmlWriter::Scope resp(*ctx.out, trt("GetVideoEncoderConfigurationResponse"));
            writeVideoEncoderConfiguration(*ctx.out, trt("Configuration"), p.videoEncoder,
                                           ctx.quirks());
            return;
        }
        ctx.fault(QString::fromLatin1(ter::NoConfig),
                  QStringLiteral("No video encoder configuration %1").arg(token));
    });

    op("GetVideoEncoderConfigurationOptions", AuthLevel::User, [](SoapContext &ctx) {
        XmlWriter &w = *ctx.out;
        XmlWriter::Scope resp(w, trt("GetVideoEncoderConfigurationOptionsResponse"));
        XmlWriter::Scope opts(w, trt("Options"));
        w.intRange(tt("QualityRange"), 1, 10);
        // 三个量程和 Baseline 在下面两个块里是一样的，抽出来写一次 ——
        // 分开写的结果必然是改了一处漏一处，而客户端只会拿到两个自相矛盾的量程。
        auto commonRanges = [&w] {
            w.intRange(tt("GovLengthRange"), 1, 100);
            w.intRange(tt("FrameRateRange"), 1, 30);
            w.intRange(tt("EncodingIntervalRange"), 1, 10);
            w.element(tt("H264ProfilesSupported"), QStringLiteral("Baseline"));
        };
        {
            XmlWriter::Scope h(w, tt("H264"));
            w.resolution(tt("ResolutionsAvailable"), 1920, 1080);
            w.resolution(tt("ResolutionsAvailable"), 1280, 720);
            w.resolution(tt("ResolutionsAvailable"), 640, 360);
            commonRanges();
            w.element(tt("H264ProfilesSupported"), QStringLiteral("Main"));
            w.element(tt("H264ProfilesSupported"), QStringLiteral("High"));
        }
        {
            // 码率范围在 ver10 里只能挂在 Extension 下。
            XmlWriter::Scope ext(w, tt("Extension"));
            XmlWriter::Scope h(w, tt("H264"));
            w.resolution(tt("ResolutionsAvailable"), 1920, 1080);
            commonRanges();
            w.intRange(tt("BitrateRange"), 64, 8192);
        }
    });

    op("SetVideoEncoderConfiguration", AuthLevel::Operator, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        const XmlNode *cfg = ctx.argNode(QStringLiteral("Configuration"));
        if (!cfg) {
            ctx.fault(soap::invalidArg(QStringLiteral("Configuration")));
            return;
        }
        const QString token = cfg->attribute(QStringLiteral("token"));
        bool applied = false;
        for (MediaProfile &p : m->profiles) {
            if (p.videoEncoder.token != token)
                continue;
            VideoEncoderConfig &c = p.videoEncoder;
            c.name = cfg->childText(QStringLiteral("Name"), c.name);
            c.encoding = cfg->childText(QStringLiteral("Encoding"), c.encoding);
            c.quality = cfg->childInt(QStringLiteral("Quality"), c.quality);
            if (const XmlNode *res = cfg->child(QStringLiteral("Resolution"))) {
                c.width = res->childInt(QStringLiteral("Width"), c.width);
                c.height = res->childInt(QStringLiteral("Height"), c.height);
            }
            if (const XmlNode *rc = cfg->child(QStringLiteral("RateControl"))) {
                c.frameRate = rc->childDouble(QStringLiteral("FrameRateLimit"), c.frameRate);
                c.bitrateKbps = rc->childInt(QStringLiteral("BitrateLimit"), c.bitrateKbps);
            }
            if (const XmlNode *h264 = cfg->child(QStringLiteral("H264"))) {
                c.govLength = h264->childInt(QStringLiteral("GovLength"), c.govLength);
                c.h264Profile = h264->childText(QStringLiteral("H264Profile"), c.h264Profile);
            }
            // 改分辨率就得换样片档位，否则声明与实际码流对不上（客户端拿 ffprobe 一探就知道）。
            // 用户挂了外部样片文件时不动它。
            if (isBuiltinAsset(p.mediaAsset))
                p.mediaAsset = assetForResolution(c.width, c.height);
            applied = true;
        }
        if (!applied) {
            ctx.fault(QString::fromLatin1(ter::NoConfig),
                      QStringLiteral("No video encoder configuration %1").arg(token));
            return;
        }
        ctx.camera->applyModelChanges();
        ctx.out->emptyElement(trt("SetVideoEncoderConfigurationResponse"));
    });

}

void MediaService::registerAudioOps()
{
    // ---- 音频源 / 音频编码 ------------------------------------------------

    op("GetAudioSources", AuthLevel::User, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        XmlWriter &w = *ctx.out;
        XmlWriter::Scope resp(w, trt("GetAudioSourcesResponse"));
        QStringList seen;
        for (const MediaProfile &p : m->profiles) {
            if (!p.hasAudio || seen.contains(p.audioSource.sourceToken))
                continue;
            seen.append(p.audioSource.sourceToken);
            XmlWriter::Scope s(w, trt("AudioSources"));
            w.attr(QStringLiteral("token"), p.audioSource.sourceToken);
            w.element(tt("Channels"), 1);
        }
    });

    op("GetAudioEncoderConfigurations", AuthLevel::User, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        XmlWriter::Scope resp(*ctx.out, trt("GetAudioEncoderConfigurationsResponse"));
        const auto configs = uniqueConfigs<AudioEncoderConfig>(
            m->profiles,
            [](const MediaProfile &p) { return p.hasAudio ? &p.audioEncoder : nullptr; });
        for (const AudioEncoderConfig *c : configs)
            writeAudioEncoderConfiguration(*ctx.out, trt("Configurations"), *c, ctx.quirks());
    });

    op("GetAudioEncoderConfigurationOptions", AuthLevel::User, [](SoapContext &ctx) {
        XmlWriter::Scope resp(*ctx.out, trt("GetAudioEncoderConfigurationOptionsResponse"));
        writeAudioEncoderOptions(*ctx.out, ctx.quirks());
    });

    op("SetAudioEncoderConfiguration", AuthLevel::Operator, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        const XmlNode *cfg = ctx.argNode(QStringLiteral("Configuration"));
        if (!cfg) {
            ctx.fault(soap::invalidArg(QStringLiteral("Configuration")));
            return;
        }
        const QString token = cfg->attribute(QStringLiteral("token"));
        bool applied = false;
        for (MediaProfile &p : m->profiles) {
            if (!p.hasAudio || p.audioEncoder.token != token)
                continue;
            AudioEncoderConfig &c = p.audioEncoder;
            c.name = cfg->childText(QStringLiteral("Name"), c.name);
            c.encoding = cfg->childText(QStringLiteral("Encoding"), c.encoding);
            c.bitrateKbps = cfg->childInt(QStringLiteral("Bitrate"), c.bitrateKbps);
            // 请求里的 SampleRate 按规范是 kHz；有的客户端会发 8000，
            // 大于 1000 就当它发的是 Hz，换算回来。
            const int rate = cfg->childInt(QStringLiteral("SampleRate"), c.sampleRateKhz);
            c.sampleRateKhz = rate > 1000 ? rate / 1000 : rate;
            applied = true;
        }
        if (!applied) {
            ctx.fault(QString::fromLatin1(ter::NoConfig),
                      QStringLiteral("No audio encoder configuration %1").arg(token));
            return;
        }
        ctx.camera->applyModelChanges();
        ctx.out->emptyElement(trt("SetAudioEncoderConfigurationResponse"));
    });

}

void MediaService::registerAudioOutputOps()
{
    // ---- 音频输出 / 解码（对讲绑定链路，E16 可整体关掉）------------------

    op("GetAudioOutputs", AuthLevel::User, [](SoapContext &ctx) {
        if (audioOutputUnsupported(ctx))
            return;
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        XmlWriter &w = *ctx.out;
        XmlWriter::Scope resp(w, trt("GetAudioOutputsResponse"));
        XmlWriter::Scope s(w, trt("AudioOutputs"));
        w.attr(QStringLiteral("token"), deviceAudioOutput(*m).outputToken);
    });

    op("GetAudioOutputConfigurations", AuthLevel::User, [](SoapContext &ctx) {
        if (audioOutputUnsupported(ctx))
            return;
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        XmlWriter::Scope resp(*ctx.out, trt("GetAudioOutputConfigurationsResponse"));
        writeAudioOutputConfiguration(*ctx.out, trt("Configurations"), deviceAudioOutput(*m));
    });

    op("GetCompatibleAudioOutputConfigurations", AuthLevel::User, [](SoapContext &ctx) {
        if (audioOutputUnsupported(ctx))
            return;
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        if (requireProfileIndex(ctx, *m) < 0)
            return;
        XmlWriter::Scope resp(*ctx.out, trt("GetCompatibleAudioOutputConfigurationsResponse"));
        writeAudioOutputConfiguration(*ctx.out, trt("Configurations"), deviceAudioOutput(*m));
    });

    op("GetAudioDecoderConfigurations", AuthLevel::User, [](SoapContext &ctx) {
        if (audioOutputUnsupported(ctx))
            return;
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        XmlWriter::Scope resp(*ctx.out, trt("GetAudioDecoderConfigurationsResponse"));
        writeAudioDecoderConfiguration(*ctx.out, trt("Configurations"), deviceAudioDecoder(*m));
    });

    op("GetCompatibleAudioDecoderConfigurations", AuthLevel::User, [](SoapContext &ctx) {
        if (audioOutputUnsupported(ctx))
            return;
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        if (requireProfileIndex(ctx, *m) < 0)
            return;
        XmlWriter::Scope resp(*ctx.out, trt("GetCompatibleAudioDecoderConfigurationsResponse"));
        writeAudioDecoderConfiguration(*ctx.out, trt("Configurations"), deviceAudioDecoder(*m));
    });

    op("GetAudioDecoderConfigurationOptions", AuthLevel::User, [](SoapContext &ctx) {
        if (audioOutputUnsupported(ctx))
            return;
        XmlWriter::Scope resp(*ctx.out, trt("GetAudioDecoderConfigurationOptionsResponse"));
        writeAudioDecoderOptions(*ctx.out, ctx.quirks());
    });

    // 下面四个是对讲绑定链路上真正会改状态的操作：客户端拿
    // GetCompatibleAudioOutputConfigurations 的第一条 token 调 Add，
    // 然后期望 GetProfiles 里能读回来。这里必须真的写进模型。
    op("AddAudioOutputConfiguration", AuthLevel::Operator, [](SoapContext &ctx) {
        if (audioOutputUnsupported(ctx))
            return;
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        const int index = requireProfileIndex(ctx, *m);
        if (index < 0)
            return;
        MediaProfile *p = &m->profiles[index];
        const QString token = ctx.arg(QStringLiteral("ConfigurationToken"));
        if (token.isEmpty()) {
            ctx.fault(soap::invalidArg(QStringLiteral("ConfigurationToken")));
            return;
        }
        const AudioOutputConfig device = deviceAudioOutput(*m);
        if (token != device.token) {
            ctx.fault(QString::fromLatin1(ter::NoConfig),
                      QStringLiteral("No audio output configuration %1").arg(token));
            return;
        }
        p->audioOutput = device;
        p->hasAudioOutput = true;
        ctx.camera->applyModelChanges();
        ctx.out->emptyElement(trt("AddAudioOutputConfigurationResponse"));
    });

    op("RemoveAudioOutputConfiguration", AuthLevel::Operator, [](SoapContext &ctx) {
        if (audioOutputUnsupported(ctx))
            return;
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        const int index = requireProfileIndex(ctx, *m);
        if (index < 0)
            return;
        MediaProfile *p = &m->profiles[index];
        p->hasAudioOutput = false;
        ctx.camera->applyModelChanges();
        ctx.out->emptyElement(trt("RemoveAudioOutputConfigurationResponse"));
    });

    op("AddAudioDecoderConfiguration", AuthLevel::Operator, [](SoapContext &ctx) {
        if (audioOutputUnsupported(ctx))
            return;
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        const int index = requireProfileIndex(ctx, *m);
        if (index < 0)
            return;
        MediaProfile *p = &m->profiles[index];
        const QString token = ctx.arg(QStringLiteral("ConfigurationToken"));
        if (token.isEmpty()) {
            ctx.fault(soap::invalidArg(QStringLiteral("ConfigurationToken")));
            return;
        }
        const AudioDecoderConfig device = deviceAudioDecoder(*m);
        if (token != device.token) {
            ctx.fault(QString::fromLatin1(ter::NoConfig),
                      QStringLiteral("No audio decoder configuration %1").arg(token));
            return;
        }
        p->audioDecoder = device;
        p->hasAudioDecoder = true;
        ctx.camera->applyModelChanges();
        ctx.out->emptyElement(trt("AddAudioDecoderConfigurationResponse"));
    });

    op("RemoveAudioDecoderConfiguration", AuthLevel::Operator, [](SoapContext &ctx) {
        if (audioOutputUnsupported(ctx))
            return;
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        const int index = requireProfileIndex(ctx, *m);
        if (index < 0)
            return;
        MediaProfile *p = &m->profiles[index];
        p->hasAudioDecoder = false;
        ctx.camera->applyModelChanges();
        ctx.out->emptyElement(trt("RemoveAudioDecoderConfigurationResponse"));
    });

}

void MediaService::registerUriOps()
{
    // ---- URI --------------------------------------------------------------

    op("GetStreamUri", AuthLevel::User, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        const int index = requireProfileIndex(ctx, *m);
        if (index < 0)
            return;
        const MediaProfile *p = &m->profiles.at(index);

        // StreamSetup 只认 RTP-Unicast + RTSP。参照客户端也只发这一种组合，
        // 其余（多播、RTP over HTTP）如实回 InvalidArgVal 更贴近真机。
        if (const XmlNode *setup = ctx.argNode(QStringLiteral("StreamSetup"))) {
            const QString stream = setup->childText(QStringLiteral("Stream"));
            if (!stream.isEmpty() && stream != QLatin1String("RTP-Unicast")) {
                ctx.fault(soap::invalidArg(QStringLiteral("StreamSetup/Stream=%1").arg(stream)));
                return;
            }
            if (const XmlNode *transport = setup->child(QStringLiteral("Transport"))) {
                const QString protocol = transport->childText(QStringLiteral("Protocol"));
                if (!protocol.isEmpty() && protocol != QLatin1String("RTSP")) {
                    ctx.fault(soap::invalidArg(
                        QStringLiteral("StreamSetup/Transport/Protocol=%1").arg(protocol)));
                    return;
                }
            }
        }

        // 占位 IP（StreamUriPlaceholderIp）与自带 userinfo（B2）都由 VirtualCamera 处理。
        const QString uri = ctx.camera->streamUri(*p, ctx.http ? ctx.http->peerAddress
                                                               : QHostAddress());
        XmlWriter::Scope resp(*ctx.out, trt("GetStreamUriResponse"));
        writeMediaUri(*ctx.out, ctx.quirks(), uri, p->videoEncoder.sessionTimeoutSec);
    });

    op("GetSnapshotUri", AuthLevel::User, [](SoapContext &ctx) {
        // B7：整个操作不实现，客户端会退避 300s 再探。
        if (ctx.quirks().isEnabled(QuirkId::NoGetSnapshotUri)) {
            ctx.faultNotSupported();
            return;
        }
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        const int index = requireProfileIndex(ctx, *m);
        if (index < 0)
            return;
        const MediaProfile *p = &m->profiles.at(index);

        QString uri = ctx.camera->snapshotUri(*p, ctx.http ? ctx.http->peerAddress
                                                           : QHostAddress());
        // B5 的 token 档：一次性令牌只能从 GetSnapshotUri 拿，过期就得重探。
        if (ctx.quirks().isEnabled(QuirkId::SnapshotAuthMode)
            && ctx.quirks().choice(QuirkId::SnapshotAuthMode, QStringLiteral("digest"))
                   == QLatin1String("token")) {
            uri += (uri.contains(QLatin1Char('?')) ? QLatin1Char('&') : QLatin1Char('?'))
                   + QStringLiteral("auth=")
                   + services::snapshotAccessToken(ctx.camera, p->token);
        }

        XmlWriter::Scope resp(*ctx.out, trt("GetSnapshotUriResponse"));
        writeMediaUri(*ctx.out, ctx.quirks(), uri, 0);
    });

}

void MediaService::registerMiscReadOps()
{
    // ---- 其余读操作 -------------------------------------------------------

    op("GetOSDs", AuthLevel::User, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        XmlWriter &w = *ctx.out;
        XmlWriter::Scope resp(w, trt("GetOSDsResponse"));
        if (m->profiles.isEmpty())
            return;
        XmlWriter::Scope osd(w, trt("OSDs"));
        w.attr(QStringLiteral("token"), QStringLiteral("OSD_1"));
        w.element(tt("VideoSourceConfigurationToken"), m->profiles.first().videoSource.token);
        w.element(tt("Type"), QStringLiteral("Text"));
        {
            XmlWriter::Scope pos(w, tt("Position"));
            w.element(tt("Type"), QStringLiteral("UpperLeft"));
        }
        {
            XmlWriter::Scope text(w, tt("TextString"));
            w.element(tt("Type"), QStringLiteral("Plain"));
            w.element(tt("PlainText"), m->displayName);
        }
    });

    op("GetMetadataConfigurations", AuthLevel::User, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m)
            return;
        XmlWriter::Scope resp(*ctx.out, trt("GetMetadataConfigurationsResponse"));
        const auto configs = uniqueConfigs<MetadataConfig>(
            m->profiles, [](const MediaProfile &p) { return p.hasMetadata ? &p.metadata
                                                                          : nullptr; });
        for (const MetadataConfig *c : configs)
            writeMetadataConfiguration(*ctx.out, trt("Configurations"), *c);
    });

    op("GetServiceCapabilities", AuthLevel::PreAuth, [this](SoapContext &ctx) {
        XmlWriter::Scope resp(*ctx.out, trt("GetServiceCapabilitiesResponse"));
        writeServiceCapabilities(ctx);
    });

}

void MediaService::registerMulticastOps()
{
    // ---- 多播（只做状态回应，真多播是二期）--------------------------------

    op("StartMulticastStreaming", AuthLevel::Operator, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m || requireProfileIndex(ctx, *m) < 0)
            return;
        ctx.out->emptyElement(trt("StartMulticastStreamingResponse"));
    });

    op("StopMulticastStreaming", AuthLevel::Operator, [](SoapContext &ctx) {
        CameraModel *m = modelOf(ctx);
        if (!m || requireProfileIndex(ctx, *m) < 0)
            return;
        ctx.out->emptyElement(trt("StopMulticastStreamingResponse"));
    });
}


namespace services {

SoapService *createMedia()
{
    return new MediaService;
}

} // namespace services
} // namespace onvifsim
