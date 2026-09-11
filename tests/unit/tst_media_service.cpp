// Media(ver10) / Media2(ver20) 服务单测。
//
// 覆盖参照客户端真正会走的那几条路：GetProfiles → GetStreamUri → GetSnapshotUri，
// 对讲绑定链路（GetCompatibleAudioOutputConfigurations → AddAudioOutputConfiguration），
// 以及落在这两个服务上的 quirk：B3 / B7 / E1~E5 / E16 / SetVideoEncoder 换样片档位。
//
// 不起 HTTP 服务器，直接按 Dispatcher 的方式构造 SoapContext 调 handler：
// 信封解析用真的 soap::parseEnvelope，响应用真的 XmlWriter，
// 再把响应解析回 XmlNode 做结构断言 —— 字符串匹配抓不到层级错位。

#include <QtTest/QtTest>

#include "core/CameraModel.h"
#include "core/Quirks.h"
#include "core/VirtualCamera.h"
#include "net/HttpTypes.h"
#include "services/ServiceBase.h"
#include "soap/Envelope.h"
#include "soap/Fault.h"
#include "soap/Namespaces.h"
#include "soap/XmlNode.h"
#include "soap/XmlWriter.h"

using namespace onvifsim;

namespace {

// 一次 handler 调用的结果：要么是响应元素子树，要么是 Fault。
struct Reply {
    XmlNode response;    // 操作响应元素，如 GetProfilesResponse
    bool faulted = false;
    QString subcode;
    QByteArray raw;
};

QByteArray makeEnvelope(const QByteArray &bodyXml)
{
    return QByteArray("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
                      "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
                      " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\""
                      " xmlns:tr2=\"http://www.onvif.org/ver20/media/wsdl\""
                      " xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
                      "<s:Body>")
           + bodyXml + QByteArray("</s:Body></s:Envelope>");
}

// 复刻 SoapDispatcher::handle 里「调 handler」那一段，跳过鉴权与 HTTP。
Reply invoke(SoapService *service, VirtualCamera *camera, const char *operation,
             const QByteArray &bodyXml)
{
    Reply reply;
    const SoapRequest request = soap::parseEnvelope(makeEnvelope(bodyXml));
    if (!request.isValid()) {
        reply.faulted = true;
        reply.subcode = QStringLiteral("parse-error");
        return reply;
    }
    const SoapOperation *op = service->findOperation(QString::fromLatin1(operation));
    if (!op) {
        reply.faulted = true;
        reply.subcode = QStringLiteral("no-such-operation");
        return reply;
    }

    HttpRequest http;
    http.method = "POST";
    http.path = service->defaultPath();

    XmlWriter writer(request.soapVersion);
    writer.declareServicePrefixes();
    writer.startEnvelope();

    SoapContext ctx;
    ctx.camera = camera;
    ctx.soap = &request;
    ctx.body = &request.body;
    ctx.http = &http;
    ctx.out = &writer;
    op->handler(ctx);

    if (ctx.hasFault()) {
        reply.faulted = true;
        reply.subcode = ctx.pendingFault().subcode;
        return reply;
    }

    writer.endEnvelope();
    reply.raw = writer.take();
    const XmlNode root = xml::parse(reply.raw);
    if (const XmlNode *body = root.child(QStringLiteral("Body"))) {
        if (!body->children.isEmpty())
            reply.response = body->children.first();
    }
    return reply;
}

const char *kGetProfile1StreamUri =
    "<trt:GetStreamUri>"
    "<trt:StreamSetup><tt:Stream>RTP-Unicast</tt:Stream>"
    "<tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport></trt:StreamSetup>"
    "<trt:ProfileToken>Profile_1</trt:ProfileToken></trt:GetStreamUri>";

const char *kGetProfile1SnapshotUri =
    "<trt:GetSnapshotUri><trt:ProfileToken>Profile_1</trt:ProfileToken></trt:GetSnapshotUri>";

const char *kGetDecoderOptions =
    "<trt:GetAudioDecoderConfigurationOptions>"
    "<trt:ProfileToken>Profile_1</trt:ProfileToken>"
    "</trt:GetAudioDecoderConfigurationOptions>";

} // namespace

class TstMediaService : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void getProfiles();
    void getProfileByToken();
    void streamUriPlain();
    void streamUriNested();          // B3
    void streamUriRejectsMulticast();
    void snapshotUri();
    void snapshotUriNotSupported();  // B7

    void decoderOptionsDefaultShape();
    void decoderOptionsShapeA();     // E4 形态 A
    void decoderOptionsShapeB();     // E4 形态 B
    void g722SampleRateLie();        // E2
    void bitrateUnitVariant();       // E3
    void sampleRateShapeRange();     // E5
    void audioCapabilityLie();       // E1

    void setVideoEncoderSwitchesAsset();
    void addAudioOutputConfiguration();
    void addAudioDecoderConfiguration();
    void audioOutputConfigUnsupported();   // E16

    void createAndDeleteProfile();
    void resolutionMismatch();

    void media2Profiles();
    void media2StreamUri();
    void media2SnapshotUriNotSupported();

private:
    Quirks &quirks() { return m_camera->mutableQuirks(); }

    VirtualCamera *m_camera = nullptr;
    SoapService *m_media = nullptr;
    SoapService *m_media2 = nullptr;
};

// 每个用例都重建相机，quirk 不会互相串味。
void TstMediaService::init()
{
    const CameraModel model =
        CameraModel::makeDefault(QStringLiteral("cam1"), QStringLiteral("generic"), 0);
    m_camera = new VirtualCamera(model, nullptr);
    m_media = services::createMedia();
    m_media2 = services::createMedia2();
}

void TstMediaService::cleanup()
{
    delete m_media;
    m_media = nullptr;
    delete m_media2;
    m_media2 = nullptr;
    delete m_camera;
    m_camera = nullptr;
}

// ------------------------------------------------------------------ profile

void TstMediaService::getProfiles()
{
    const Reply r = invoke(m_media, m_camera, "GetProfiles", "<trt:GetProfiles/>");
    QVERIFY(!r.faulted);

    const QVector<const XmlNode *> profiles =
        r.response.childrenNamed(QStringLiteral("Profiles"));
    QCOMPARE(profiles.size(), 3);

    // 客户端判定主 / 子码流纯看 Name 字串，所以这三个名字是承重的。
    QCOMPARE(profiles.at(0)->attribute(QStringLiteral("token")), QStringLiteral("Profile_1"));
    QCOMPARE(profiles.at(0)->childText(QStringLiteral("Name")), QStringLiteral("MainStream"));
    QCOMPARE(profiles.at(1)->childText(QStringLiteral("Name")), QStringLiteral("SubStream"));
    QCOMPARE(profiles.at(2)->childText(QStringLiteral("Name")), QStringLiteral("ThirdStream"));

    // 分辨率照实写。
    const XmlNode *encoder =
        profiles.at(0)->child(QStringLiteral("VideoEncoderConfiguration"));
    QVERIFY(encoder);
    const XmlNode *resolution = encoder->child(QStringLiteral("Resolution"));
    QVERIFY(resolution);
    QCOMPARE(resolution->childText(QStringLiteral("Width")), QStringLiteral("1920"));
    QCOMPARE(resolution->childText(QStringLiteral("Height")), QStringLiteral("1080"));

    // has_ptz 判定看的是这个元素在不在。
    QVERIFY(profiles.at(0)->child(QStringLiteral("PTZConfiguration")) != nullptr);
    // 对讲还没绑，Extension 不该出现。
    QVERIFY(profiles.at(0)->child(QStringLiteral("Extension")) == nullptr);
}

void TstMediaService::getProfileByToken()
{
    const Reply r = invoke(m_media, m_camera, "GetProfile",
                           "<trt:GetProfile><trt:ProfileToken>Profile_2</trt:ProfileToken>"
                           "</trt:GetProfile>");
    QVERIFY(!r.faulted);
    const XmlNode *profile = r.response.child(QStringLiteral("Profile"));
    QVERIFY(profile);
    QCOMPARE(profile->childText(QStringLiteral("Name")), QStringLiteral("SubStream"));

    const Reply bad = invoke(m_media, m_camera, "GetProfile",
                             "<trt:GetProfile><trt:ProfileToken>nope</trt:ProfileToken>"
                             "</trt:GetProfile>");
    QVERIFY(bad.faulted);
    QCOMPARE(bad.subcode, QString::fromLatin1(ter::NoProfile));
}

// ---------------------------------------------------------------------- URI

void TstMediaService::streamUriPlain()
{
    const Reply r = invoke(m_media, m_camera, "GetStreamUri", kGetProfile1StreamUri);
    QVERIFY(!r.faulted);
    const XmlNode *mediaUri = r.response.child(QStringLiteral("MediaUri"));
    QVERIFY(mediaUri);
    // 正常形态：Uri 就挂在 MediaUri 下，没有再套一层。
    QVERIFY(mediaUri->child(QStringLiteral("MediaUri")) == nullptr);
    const QString uri = mediaUri->childText(QStringLiteral("Uri"));
    QVERIFY2(uri.startsWith(QLatin1String("rtsp://")), qPrintable(uri));
    QVERIFY(uri.endsWith(QLatin1String("/profile1")));
}

void TstMediaService::streamUriNested()
{
    // B3：URI 套在 MediaUri/Uri 下多一层，只认顶层 Uri 的客户端会取空。
    quirks().setEnabled(QuirkId::StreamUriNested, true);
    const Reply r = invoke(m_media, m_camera, "GetStreamUri", kGetProfile1StreamUri);
    QVERIFY(!r.faulted);
    const XmlNode *outer = r.response.child(QStringLiteral("MediaUri"));
    QVERIFY(outer);
    QVERIFY(outer->childText(QStringLiteral("Uri")).isEmpty());
    const XmlNode *inner = outer->child(QStringLiteral("MediaUri"));
    QVERIFY(inner);
    QVERIFY(inner->childText(QStringLiteral("Uri")).startsWith(QLatin1String("rtsp://")));
}

void TstMediaService::streamUriRejectsMulticast()
{
    const Reply r = invoke(m_media, m_camera, "GetStreamUri",
                           "<trt:GetStreamUri><trt:StreamSetup>"
                           "<tt:Stream>RTP-Multicast</tt:Stream>"
                           "<tt:Transport><tt:Protocol>UDP</tt:Protocol></tt:Transport>"
                           "</trt:StreamSetup>"
                           "<trt:ProfileToken>Profile_1</trt:ProfileToken></trt:GetStreamUri>");
    QVERIFY(r.faulted);
    QCOMPARE(r.subcode, QString::fromLatin1(ter::InvalidArgVal));
}

void TstMediaService::snapshotUri()
{
    const Reply r = invoke(m_media, m_camera, "GetSnapshotUri", kGetProfile1SnapshotUri);
    QVERIFY(!r.faulted);
    const XmlNode *mediaUri = r.response.child(QStringLiteral("MediaUri"));
    QVERIFY(mediaUri);
    const QString uri = mediaUri->childText(QStringLiteral("Uri"));
    QVERIFY2(uri.startsWith(QLatin1String("http://")), qPrintable(uri));
    QVERIFY(uri.contains(QLatin1String("/onvif/snapshot")));
    QVERIFY(uri.contains(QLatin1String("token=Profile_1")));
}

void TstMediaService::snapshotUriNotSupported()
{
    // B7：整个操作不实现，客户端应退避而不是一直重试。
    quirks().setEnabled(QuirkId::NoGetSnapshotUri, true);
    const Reply r = invoke(m_media, m_camera, "GetSnapshotUri", kGetProfile1SnapshotUri);
    QVERIFY(r.faulted);
    QCOMPARE(r.subcode, QString::fromLatin1(ter::ActionNotSupported));
}

// -------------------------------------------------------------- 音频能力探测

void TstMediaService::decoderOptionsDefaultShape()
{
    // quirk 全关：走规范的 G711DecOptions / G722DecOptions + IntList（Items）。
    const Reply r = invoke(m_media, m_camera, "GetAudioDecoderConfigurationOptions",
                           kGetDecoderOptions);
    QVERIFY(!r.faulted);
    const XmlNode *opts = r.response.child(QStringLiteral("Options"));
    QVERIFY(opts);
    const XmlNode *g711 = opts->child(QStringLiteral("G711DecOptions"));
    QVERIFY(g711);
    QCOMPARE(g711->child(QStringLiteral("Bitrate"))->childText(QStringLiteral("Items")),
             QStringLiteral("64"));
    QCOMPARE(g711->child(QStringLiteral("SampleRateRange"))->childText(QStringLiteral("Items")),
             QStringLiteral("8000"));
    // E1 关着时不该出现 G726 / AAC。
    QVERIFY(opts->child(QStringLiteral("G726DecOptions")) == nullptr);
    QVERIFY(opts->child(QStringLiteral("AACDecOptions")) == nullptr);
}

void TstMediaService::decoderOptionsShapeA()
{
    // E4 形态 A：G711 / G722 子元素直挂 Options，各含 Bitrate + SampleRateRange。
    quirks().setEnabled(QuirkId::AudioDecoderOptionsShape, true);
    quirks().setParam(QuirkId::AudioDecoderOptionsShape, QStringLiteral("value"),
                      QStringLiteral("a"));
    const Reply r = invoke(m_media, m_camera, "GetAudioDecoderConfigurationOptions",
                           kGetDecoderOptions);
    QVERIFY(!r.faulted);
    const XmlNode *opts = r.response.child(QStringLiteral("Options"));
    QVERIFY(opts);

    const XmlNode *g711 = opts->child(QStringLiteral("G711"));
    QVERIFY(g711);
    QVERIFY(g711->child(QStringLiteral("Bitrate")) != nullptr);
    QVERIFY(g711->child(QStringLiteral("SampleRateRange")) != nullptr);

    const XmlNode *g722 = opts->child(QStringLiteral("G722"));
    QVERIFY(g722);
    QVERIFY(g722->child(QStringLiteral("SampleRateRange")) != nullptr);

    // 形态 A 里不该出现形态 B 的列表元素。
    QVERIFY(opts->child(QStringLiteral("AudioDecoderOptions")) == nullptr);
}

void TstMediaService::decoderOptionsShapeB()
{
    // E4 形态 B：AudioDecoderOptions 列表，每项带 Encoding。
    quirks().setEnabled(QuirkId::AudioDecoderOptionsShape, true);
    quirks().setParam(QuirkId::AudioDecoderOptionsShape, QStringLiteral("value"),
                      QStringLiteral("b"));
    const Reply r = invoke(m_media, m_camera, "GetAudioDecoderConfigurationOptions",
                           kGetDecoderOptions);
    QVERIFY(!r.faulted);
    const XmlNode *opts = r.response.child(QStringLiteral("Options"));
    QVERIFY(opts);

    const QVector<const XmlNode *> items =
        opts->childrenNamed(QStringLiteral("AudioDecoderOptions"));
    QCOMPARE(items.size(), 2);   // E1 关着，只有 G711 与 G722
    QCOMPARE(items.at(0)->childText(QStringLiteral("Encoding")), QStringLiteral("G711"));
    QCOMPARE(items.at(1)->childText(QStringLiteral("Encoding")), QStringLiteral("G722"));
    // E5 默认形态是 list，字段名应是 *List 而不是 *Range。
    QVERIFY(items.at(0)->child(QStringLiteral("SampleRateList")) != nullptr);
    QVERIFY(items.at(0)->child(QStringLiteral("BitrateList")) != nullptr);
    // 形态 B 里不该再出现形态 A 的子元素。
    QVERIFY(opts->child(QStringLiteral("G711")) == nullptr);
    QVERIFY(opts->child(QStringLiteral("G711DecOptions")) == nullptr);
}

void TstMediaService::g722SampleRateLie()
{
    // 默认：G.722 老老实实报 16000。
    {
        const Reply r = invoke(m_media, m_camera, "GetAudioDecoderConfigurationOptions",
                               kGetDecoderOptions);
        const XmlNode *g722 =
            r.response.child(QStringLiteral("Options"))->child(QStringLiteral("G722DecOptions"));
        QVERIFY(g722);
        QCOMPARE(g722->child(QStringLiteral("SampleRateRange"))
                     ->childText(QStringLiteral("Items")),
                 QStringLiteral("16000"));
    }
    // E2：错写成 8000，客户端必须自己强制改回 16000。
    quirks().setEnabled(QuirkId::G722SampleRate8000, true);
    {
        const Reply r = invoke(m_media, m_camera, "GetAudioDecoderConfigurationOptions",
                               kGetDecoderOptions);
        const XmlNode *g722 =
            r.response.child(QStringLiteral("Options"))->child(QStringLiteral("G722DecOptions"));
        QVERIFY(g722);
        QCOMPARE(g722->child(QStringLiteral("SampleRateRange"))
                     ->childText(QStringLiteral("Items")),
                 QStringLiteral("8000"));
    }
}

void TstMediaService::bitrateUnitVariant()
{
    // E3：单位从 kbps 换成 bps，同一条能力差 1000 倍。
    quirks().setEnabled(QuirkId::BitrateUnitVariant, true);
    quirks().setParam(QuirkId::BitrateUnitVariant, QStringLiteral("value"),
                      QStringLiteral("bps"));

    const Reply r = invoke(m_media, m_camera, "GetAudioDecoderConfigurationOptions",
                           kGetDecoderOptions);
    const XmlNode *g711 =
        r.response.child(QStringLiteral("Options"))->child(QStringLiteral("G711DecOptions"));
    QVERIFY(g711);
    QCOMPARE(g711->child(QStringLiteral("Bitrate"))->childText(QStringLiteral("Items")),
             QStringLiteral("64000"));

    // 编码侧的 Options 走同一套换算。
    const Reply enc = invoke(m_media, m_camera, "GetAudioEncoderConfigurationOptions",
                             "<trt:GetAudioEncoderConfigurationOptions/>");
    QVERIFY(!enc.faulted);
    const XmlNode *first =
        enc.response.child(QStringLiteral("Options"))->child(QStringLiteral("Options"));
    QVERIFY(first);
    QCOMPARE(first->child(QStringLiteral("BitrateList"))->childText(QStringLiteral("Items")),
             QStringLiteral("64000"));
}

void TstMediaService::sampleRateShapeRange()
{
    // E5：采样率写成 Min/Max 而不是 Items 列表。
    quirks().setEnabled(QuirkId::SampleRateShape, true);
    quirks().setParam(QuirkId::SampleRateShape, QStringLiteral("value"),
                      QStringLiteral("range"));

    const Reply r = invoke(m_media, m_camera, "GetAudioDecoderConfigurationOptions",
                           kGetDecoderOptions);
    const XmlNode *g711 =
        r.response.child(QStringLiteral("Options"))->child(QStringLiteral("G711DecOptions"));
    QVERIFY(g711);
    const XmlNode *range = g711->child(QStringLiteral("SampleRateRange"));
    QVERIFY(range);
    QCOMPARE(range->childText(QStringLiteral("Min")), QStringLiteral("8000"));
    QCOMPARE(range->childText(QStringLiteral("Max")), QStringLiteral("8000"));
    QVERIFY(range->child(QStringLiteral("Items")) == nullptr);
}

void TstMediaService::audioCapabilityLie()
{
    // E1：能力里多列 G726 / AAC，而 backchannel SDP 只会开 PCMU。
    quirks().setEnabled(QuirkId::AudioCapabilityLie, true);
    const Reply r = invoke(m_media, m_camera, "GetAudioDecoderConfigurationOptions",
                           kGetDecoderOptions);
    const XmlNode *opts = r.response.child(QStringLiteral("Options"));
    QVERIFY(opts);
    QVERIFY(opts->child(QStringLiteral("G726DecOptions")) != nullptr);
    QVERIFY(opts->child(QStringLiteral("AACDecOptions")) != nullptr);
}

// -------------------------------------------------------------------- 写操作

void TstMediaService::setVideoEncoderSwitchesAsset()
{
    QCOMPARE(m_camera->model().profiles.at(0).mediaAsset, QStringLiteral("1080p"));

    const Reply down =
        invoke(m_media, m_camera, "SetVideoEncoderConfiguration",
               "<trt:SetVideoEncoderConfiguration>"
               "<trt:Configuration token=\"VideoEncoder_1\">"
               "<tt:Name>VideoEncoder_1</tt:Name><tt:UseCount>1</tt:UseCount>"
               "<tt:Encoding>H264</tt:Encoding>"
               "<tt:Resolution><tt:Width>640</tt:Width><tt:Height>360</tt:Height></tt:Resolution>"
               "<tt:Quality>4</tt:Quality>"
               "<tt:RateControl><tt:FrameRateLimit>15</tt:FrameRateLimit>"
               "<tt:EncodingInterval>1</tt:EncodingInterval>"
               "<tt:BitrateLimit>512</tt:BitrateLimit></tt:RateControl>"
               "</trt:Configuration>"
               "<trt:ForcePersistence>true</trt:ForcePersistence>"
               "</trt:SetVideoEncoderConfiguration>");
    QVERIFY(!down.faulted);

    const MediaProfile &p = m_camera->model().profiles.at(0);
    QCOMPARE(p.videoEncoder.width, 640);
    QCOMPARE(p.videoEncoder.height, 360);
    QCOMPARE(p.videoEncoder.bitrateKbps, 512);
    // 换了分辨率就得换样片档位，否则 ffprobe 一探就跟声明对不上。
    QCOMPARE(p.mediaAsset, QStringLiteral("360p"));

    const Reply up =
        invoke(m_media, m_camera, "SetVideoEncoderConfiguration",
               "<trt:SetVideoEncoderConfiguration>"
               "<trt:Configuration token=\"VideoEncoder_1\">"
               "<tt:Resolution><tt:Width>1280</tt:Width><tt:Height>720</tt:Height></tt:Resolution>"
               "</trt:Configuration></trt:SetVideoEncoderConfiguration>");
    QVERIFY(!up.faulted);
    QCOMPARE(m_camera->model().profiles.at(0).mediaAsset, QStringLiteral("720p"));

    // 不存在的配置 token 要如实报错。
    const Reply bad = invoke(m_media, m_camera, "SetVideoEncoderConfiguration",
                             "<trt:SetVideoEncoderConfiguration>"
                             "<trt:Configuration token=\"nope\"/>"
                             "</trt:SetVideoEncoderConfiguration>");
    QVERIFY(bad.faulted);
    QCOMPARE(bad.subcode, QString::fromLatin1(ter::NoConfig));
}

void TstMediaService::addAudioOutputConfiguration()
{
    // 客户端先拿兼容列表里的第一条 token。
    const Reply compatible =
        invoke(m_media, m_camera, "GetCompatibleAudioOutputConfigurations",
               "<trt:GetCompatibleAudioOutputConfigurations>"
               "<trt:ProfileToken>Profile_1</trt:ProfileToken>"
               "</trt:GetCompatibleAudioOutputConfigurations>");
    QVERIFY(!compatible.faulted);
    const XmlNode *config = compatible.response.child(QStringLiteral("Configurations"));
    QVERIFY(config);
    const QString token = config->attribute(QStringLiteral("token"));
    QCOMPARE(token, QStringLiteral("AudioOutputConfig"));

    QVERIFY(!m_camera->model().profiles.at(0).hasAudioOutput);
    const Reply add = invoke(m_media, m_camera, "AddAudioOutputConfiguration",
                             "<trt:AddAudioOutputConfiguration>"
                             "<trt:ProfileToken>Profile_1</trt:ProfileToken>"
                             "<trt:ConfigurationToken>AudioOutputConfig</trt:ConfigurationToken>"
                             "</trt:AddAudioOutputConfiguration>");
    QVERIFY(!add.faulted);
    // 必须真的改状态 —— 客户端靠 GetProfiles 回读来确认对讲绑上了。
    QVERIFY(m_camera->model().profiles.at(0).hasAudioOutput);

    const Reply profiles = invoke(m_media, m_camera, "GetProfiles", "<trt:GetProfiles/>");
    const XmlNode *ext = profiles.response.childrenNamed(QStringLiteral("Profiles"))
                             .at(0)
                             ->child(QStringLiteral("Extension"));
    QVERIFY(ext);
    QVERIFY(ext->child(QStringLiteral("AudioOutputConfiguration")) != nullptr);

    const Reply remove = invoke(m_media, m_camera, "RemoveAudioOutputConfiguration",
                                "<trt:RemoveAudioOutputConfiguration>"
                                "<trt:ProfileToken>Profile_1</trt:ProfileToken>"
                                "</trt:RemoveAudioOutputConfiguration>");
    QVERIFY(!remove.faulted);
    QVERIFY(!m_camera->model().profiles.at(0).hasAudioOutput);
}

void TstMediaService::addAudioDecoderConfiguration()
{
    QVERIFY(!m_camera->model().profiles.at(0).hasAudioDecoder);
    const Reply add = invoke(m_media, m_camera, "AddAudioDecoderConfiguration",
                             "<trt:AddAudioDecoderConfiguration>"
                             "<trt:ProfileToken>Profile_1</trt:ProfileToken>"
                             "<trt:ConfigurationToken>AudioDecoderConfig</trt:ConfigurationToken>"
                             "</trt:AddAudioDecoderConfiguration>");
    QVERIFY(!add.faulted);
    QVERIFY(m_camera->model().profiles.at(0).hasAudioDecoder);

    // token 对不上要报 NoConfig，而不是默默成功。
    const Reply bad = invoke(m_media, m_camera, "AddAudioDecoderConfiguration",
                             "<trt:AddAudioDecoderConfiguration>"
                             "<trt:ProfileToken>Profile_2</trt:ProfileToken>"
                             "<trt:ConfigurationToken>nope</trt:ConfigurationToken>"
                             "</trt:AddAudioDecoderConfiguration>");
    QVERIFY(bad.faulted);
    QCOMPARE(bad.subcode, QString::fromLatin1(ter::NoConfig));
    QVERIFY(!m_camera->model().profiles.at(1).hasAudioDecoder);
}

void TstMediaService::audioOutputConfigUnsupported()
{
    // E16：这一族操作整体不实现，客户端只能静默跳过再直接开 backchannel 试。
    quirks().setEnabled(QuirkId::NoAudioOutputConfig, true);
    const QByteArray ops[] = {
        "<trt:GetAudioOutputs/>",
        "<trt:GetAudioOutputConfigurations/>",
        "<trt:GetAudioDecoderConfigurations/>",
        "<trt:GetCompatibleAudioOutputConfigurations>"
        "<trt:ProfileToken>Profile_1</trt:ProfileToken>"
        "</trt:GetCompatibleAudioOutputConfigurations>",
        "<trt:AddAudioOutputConfiguration>"
        "<trt:ProfileToken>Profile_1</trt:ProfileToken>"
        "<trt:ConfigurationToken>AudioOutputConfig</trt:ConfigurationToken>"
        "</trt:AddAudioOutputConfiguration>",
        "<trt:AddAudioDecoderConfiguration>"
        "<trt:ProfileToken>Profile_1</trt:ProfileToken>"
        "<trt:ConfigurationToken>AudioDecoderConfig</trt:ConfigurationToken>"
        "</trt:AddAudioDecoderConfiguration>",
    };
    const char *names[] = { "GetAudioOutputs",
                            "GetAudioOutputConfigurations",
                            "GetAudioDecoderConfigurations",
                            "GetCompatibleAudioOutputConfigurations",
                            "AddAudioOutputConfiguration",
                            "AddAudioDecoderConfiguration" };
    for (int i = 0; i < 6; ++i) {
        const Reply r = invoke(m_media, m_camera, names[i], ops[i]);
        QVERIFY2(r.faulted, names[i]);
        QCOMPARE(r.subcode, QString::fromLatin1(ter::ActionNotSupported));
    }
    // 状态一点没动。
    QVERIFY(!m_camera->model().profiles.at(0).hasAudioOutput);
    QVERIFY(!m_camera->model().profiles.at(0).hasAudioDecoder);
}

void TstMediaService::createAndDeleteProfile()
{
    const Reply created = invoke(m_media, m_camera, "CreateProfile",
                                 "<trt:CreateProfile><trt:Name>Extra</trt:Name>"
                                 "<trt:Token>Profile_X</trt:Token></trt:CreateProfile>");
    QVERIFY(!created.faulted);
    QCOMPARE(m_camera->model().profiles.size(), 4);
    const XmlNode *profile = created.response.child(QStringLiteral("Profile"));
    QVERIFY(profile);
    QCOMPARE(profile->attribute(QStringLiteral("fixed")), QStringLiteral("false"));

    // 出厂 profile 是 fixed 的，删不掉。
    const Reply fixedDelete = invoke(m_media, m_camera, "DeleteProfile",
                                     "<trt:DeleteProfile>"
                                     "<trt:ProfileToken>Profile_1</trt:ProfileToken>"
                                     "</trt:DeleteProfile>");
    QVERIFY(fixedDelete.faulted);
    QCOMPARE(m_camera->model().profiles.size(), 4);

    const Reply removed = invoke(m_media, m_camera, "DeleteProfile",
                                 "<trt:DeleteProfile>"
                                 "<trt:ProfileToken>Profile_X</trt:ProfileToken>"
                                 "</trt:DeleteProfile>");
    QVERIFY(!removed.faulted);
    QCOMPARE(m_camera->model().profiles.size(), 3);
}

void TstMediaService::resolutionMismatch()
{
    // 声明的分辨率比实际码流高一档，用来测客户端信声明还是信 ffprobe。
    quirks().setEnabled(QuirkId::ResolutionMismatch, true);
    const Reply r = invoke(m_media, m_camera, "GetVideoEncoderConfigurations",
                           "<trt:GetVideoEncoderConfigurations/>");
    QVERIFY(!r.faulted);
    const XmlNode *first = r.response.child(QStringLiteral("Configurations"));
    QVERIFY(first);
    const XmlNode *resolution = first->child(QStringLiteral("Resolution"));
    QVERIFY(resolution);
    QCOMPARE(resolution->childText(QStringLiteral("Width")), QStringLiteral("2560"));
    QCOMPARE(resolution->childText(QStringLiteral("Height")), QStringLiteral("1440"));
    // 模型本身没被改，改的只是嘴上说的。
    QCOMPARE(m_camera->model().profiles.at(0).videoEncoder.width, 1920);
}

// -------------------------------------------------------------------- Media2

void TstMediaService::media2Profiles()
{
    const Reply r = invoke(m_media2, m_camera, "GetProfiles",
                           "<tr2:GetProfiles><tr2:Type>All</tr2:Type></tr2:GetProfiles>");
    QVERIFY(!r.faulted);
    const QVector<const XmlNode *> profiles =
        r.response.childrenNamed(QStringLiteral("Profiles"));
    QCOMPARE(profiles.size(), 3);
    QCOMPARE(profiles.at(0)->childText(QStringLiteral("Name")), QStringLiteral("MainStream"));

    const XmlNode *set = profiles.at(0)->child(QStringLiteral("Configurations"));
    QVERIFY(set);
    QVERIFY(set->child(QStringLiteral("VideoSource")) != nullptr);
    const XmlNode *encoder = set->child(QStringLiteral("VideoEncoder"));
    QVERIFY(encoder);
    // ver20 把 GovLength / Profile 挪成了属性，这是最容易写错的一处。
    QCOMPARE(encoder->attribute(QStringLiteral("GovLength")), QStringLiteral("15"));
    QVERIFY(set->child(QStringLiteral("Analytics")) != nullptr);
    QVERIFY(set->child(QStringLiteral("PTZ")) != nullptr);
}

void TstMediaService::media2StreamUri()
{
    const Reply r = invoke(m_media2, m_camera, "GetStreamUri",
                           "<tr2:GetStreamUri><tr2:Protocol>RtspUnicast</tr2:Protocol>"
                           "<tr2:ProfileToken>Profile_2</tr2:ProfileToken></tr2:GetStreamUri>");
    QVERIFY(!r.faulted);
    // ver20 返回的是一个裸 Uri，不是 ver10 那种 MediaUri 结构。
    const QString uri = r.response.childText(QStringLiteral("Uri"));
    QVERIFY2(uri.startsWith(QLatin1String("rtsp://")), qPrintable(uri));
    QVERIFY(uri.endsWith(QLatin1String("/profile2")));

    const Reply missing = invoke(m_media2, m_camera, "GetStreamUri",
                                 "<tr2:GetStreamUri><tr2:Protocol>RtspUnicast</tr2:Protocol>"
                                 "</tr2:GetStreamUri>");
    QVERIFY(missing.faulted);
    QCOMPARE(missing.subcode, QString::fromLatin1(ter::NoProfile));
}

void TstMediaService::media2SnapshotUriNotSupported()
{
    // B7 在 ver20 这边也要一致，否则客户端会拿到互相矛盾的结论。
    quirks().setEnabled(QuirkId::NoGetSnapshotUri, true);
    const Reply r = invoke(m_media2, m_camera, "GetSnapshotUri",
                           "<tr2:GetSnapshotUri>"
                           "<tr2:ProfileToken>Profile_1</tr2:ProfileToken>"
                           "</tr2:GetSnapshotUri>");
    QVERIFY(r.faulted);
    QCOMPARE(r.subcode, QString::fromLatin1(ter::ActionNotSupported));
}

QTEST_GUILESS_MAIN(TstMediaService)

#include "tst_media_service.moc"
