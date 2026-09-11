// PTZ 服务层单测：能力声明的四档 SupportedPTZSpaces（C3）、Range 退化（C4）、
// ContinuousMove 只给一个轴、GetStatus 的位置输出、SetPreset 的三种返回形态（C9）、
// GetPresets 的三种「不支持」表现（C8）。
//
// 直接调 handler，不经过 HTTP：手工拼一个 SoapContext 就够了。
// 注意 SoapContext::exchange 在这里是空的，所以 C5（畸形响应）与 C10（响应抖动）
// 这两条要接管响应的 quirk 走不到，留给 e2e。

#include <QtTest/QtTest>

#include "core/CameraModel.h"
#include "core/Quirks.h"
#include "core/VirtualCamera.h"
#include "net/HttpTypes.h"
#include "ptz/PtzState.h"
#include "services/ServiceBase.h"
#include "soap/Envelope.h"
#include "soap/XmlWriter.h"

using namespace onvifsim;

namespace {

// 注意：这里一律用普通转义字符串而不是 raw string —— moc 会把 raw string 里的
// "//" 当成行注释，把后面的 Q_OBJECT 一起吃掉，症状是链接期缺 vtable。
const char *kEnvelopeHead =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
    " xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\""
    " xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
    "<s:Body>";
const char *kEnvelopeTail = "</s:Body></s:Envelope>";

// 一次 handler 调用的结果。
struct Result {
    QByteArray xml;          // 无 Fault 时的完整响应信封
    bool fault = false;
    QString subcode;
    QString reason;
    bool takenOver = false;
};

Result call(SoapService *service, VirtualCamera *camera, const QString &operation,
            const QByteArray &bodyElement)
{
    Result result;
    const QByteArray raw = QByteArray(kEnvelopeHead) + bodyElement + kEnvelopeTail;
    const SoapRequest request = soap::parseEnvelope(raw);

    HttpRequest http;
    http.method = "POST";
    http.path = QStringLiteral("/onvif/ptz_service");
    http.body = raw;

    XmlWriter writer(request.soapVersion);
    writer.declareServicePrefixes();
    writer.startEnvelope();

    SoapContext ctx;
    ctx.camera = camera;
    ctx.soap = &request;
    ctx.body = &request.body;
    ctx.http = &http;
    ctx.out = &writer;

    const SoapOperation *op = service->findOperation(operation);
    if (!op)
        return result;
    op->handler(ctx);

    result.takenOver = ctx.responseTakenOver();
    if (ctx.hasFault()) {
        result.fault = true;
        result.subcode = ctx.pendingFault().subcode;
        result.reason = ctx.pendingFault().reason;
        return result;
    }
    writer.endEnvelope();
    result.xml = writer.take();
    return result;
}

} // namespace

class TstPtzService : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void getNodesFullSpaces();
    void configurationOptionsSpacesEmpty();
    void configurationOptionsSpacesDefaultOnly();
    void configurationOptionsSpacesPanOnly();
    void rangeMinEqualsMax();
    void continuousMoveZoomOnly();
    void continuousMovePanTiltOnly();
    void continuousMoveBothAxes();
    void getStatusReportsPosition();
    void setPresetReturnShapes();
    void getPresetsUnsupportedShapes();
    void profileWithoutPtzConfigFaults();

private:
    VirtualCamera *m_camera = nullptr;
    SoapService *m_service = nullptr;
};

void TstPtzService::init()
{
    m_camera = new VirtualCamera(CameraModel::makeDefault(QStringLiteral("cam1"),
                                                          QStringLiteral("generic"), 0),
                                 nullptr);
    m_service = services::createPtz();
}

void TstPtzService::cleanup()
{
    delete m_service;
    m_service = nullptr;
    delete m_camera;
    m_camera = nullptr;
}

void TstPtzService::getNodesFullSpaces()
{
    const Result r = call(m_service, m_camera, QStringLiteral("GetNodes"),
                          "<tptz:GetNodes/>");
    QVERIFY(!r.fault);
    QVERIFY(r.xml.contains("tptz:PTZNode"));
    QVERIFY(r.xml.contains("token=\"PTZNode_1\""));
    QVERIFY(r.xml.contains("<tt:SupportedPTZSpaces>"));
    // 四类空间全在：客户端靠 Continuous*VelocitySpace 判方向键与变焦。
    QVERIFY(r.xml.contains("tt:ContinuousPanTiltVelocitySpace"));
    QVERIFY(r.xml.contains("tt:ContinuousZoomVelocitySpace"));
    QVERIFY(r.xml.contains("tt:AbsolutePanTiltPositionSpace"));
    QVERIFY(r.xml.contains("tt:ZoomSpeedSpace"));
    QVERIFY(r.xml.contains("<tt:MaximumNumberOfPresets>300</tt:MaximumNumberOfPresets>"));
    QVERIFY(r.xml.contains("<tt:HomeSupported>true</tt:HomeSupported>"));
}

void TstPtzService::configurationOptionsSpacesEmpty()
{
    Quirks q = m_camera->quirks();
    q.setEnabled(QuirkId::PtzSpacesEmpty, true);
    q.setParam(QuirkId::PtzSpacesEmpty, QStringLiteral("value"), QStringLiteral("empty"));
    m_camera->setQuirks(q);

    const Result options = call(m_service, m_camera, QStringLiteral("GetConfigurationOptions"),
                                "<tptz:GetConfigurationOptions>"
                                "<tptz:ConfigurationToken>PTZConfig_1</tptz:ConfigurationToken>"
                                "</tptz:GetConfigurationOptions>");
    QVERIFY(!options.fault);
    // 完全空的 Spaces —— TL-IPC652P-A4 的真机表现。
    QVERIFY(options.xml.contains("<tt:Spaces/>"));

    // GetNodes 同样为空，而且配置里也不给 Default*Space：客户端两条路都摸不到，
    // 只能回落到「保守全支持」。
    const Result nodes = call(m_service, m_camera, QStringLiteral("GetNodes"),
                              "<tptz:GetNodes/>");
    QVERIFY(nodes.xml.contains("<tt:SupportedPTZSpaces/>"));
    const Result config = call(m_service, m_camera, QStringLiteral("GetConfigurations"),
                               "<tptz:GetConfigurations/>");
    QVERIFY(!config.xml.contains("DefaultContinuousPanTiltVelocitySpace"));
}

void TstPtzService::configurationOptionsSpacesDefaultOnly()
{
    Quirks q = m_camera->quirks();
    q.setEnabled(QuirkId::PtzSpacesEmpty, true);
    q.setParam(QuirkId::PtzSpacesEmpty, QStringLiteral("value"), QStringLiteral("default_only"));
    m_camera->setQuirks(q);

    const Result options = call(m_service, m_camera, QStringLiteral("GetConfigurationOptions"),
                                "<tptz:GetConfigurationOptions/>");
    QVERIFY(options.xml.contains("<tt:Spaces/>"));

    // 这一档的关键：空间表是空的，但 PTZConfiguration 里给出 Default*Space。
    const Result config = call(m_service, m_camera, QStringLiteral("GetConfigurations"),
                               "<tptz:GetConfigurations/>");
    QVERIFY(config.xml.contains("tt:DefaultContinuousPanTiltVelocitySpace"));
    QVERIFY(config.xml.contains("tt:DefaultContinuousZoomVelocitySpace"));
}

void TstPtzService::configurationOptionsSpacesPanOnly()
{
    Quirks q = m_camera->quirks();
    q.setEnabled(QuirkId::PtzSpacesEmpty, true);
    q.setParam(QuirkId::PtzSpacesEmpty, QStringLiteral("value"), QStringLiteral("pan_only"));
    m_camera->setQuirks(q);

    const Result options = call(m_service, m_camera, QStringLiteral("GetConfigurationOptions"),
                                "<tptz:GetConfigurationOptions/>");
    QVERIFY(!options.fault);
    QVERIFY(options.xml.contains("tt:ContinuousPanTiltVelocitySpace"));
    // 只有 pan：没有 zoom 速度空间，tilt 的 YRange 塌成 0..0。
    QVERIFY(!options.xml.contains("tt:ContinuousZoomVelocitySpace"));
    QVERIFY(options.xml.contains("<tt:YRange><tt:Min>0</tt:Min><tt:Max>0</tt:Max></tt:YRange>"));
    QVERIFY(options.xml.contains("<tt:XRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:XRange>"));
}

void TstPtzService::rangeMinEqualsMax()
{
    Quirks q = m_camera->quirks();
    q.setEnabled(QuirkId::PtzRangeMinEqualsMax, true);
    m_camera->setQuirks(q);

    const Result nodes = call(m_service, m_camera, QStringLiteral("GetNodes"),
                              "<tptz:GetNodes/>");
    QVERIFY(!nodes.fault);
    // C4：Min == Max == 1，客户端「Min 或 Max 任一非零」的放宽判据就是为它写的。
    QVERIFY(nodes.xml.contains("<tt:XRange><tt:Min>1</tt:Min><tt:Max>1</tt:Max></tt:XRange>"));
    QVERIFY(nodes.xml.contains("<tt:YRange><tt:Min>1</tt:Min><tt:Max>1</tt:Max></tt:YRange>"));
    QVERIFY(!nodes.xml.contains("<tt:Min>-1</tt:Min>"));
}

void TstPtzService::continuousMoveZoomOnly()
{
    // 手写栈的形态：非零 Zoom 时只发 <tt:Zoom>，一个 PanTilt 都没有。
    const Result r = call(m_service, m_camera, QStringLiteral("ContinuousMove"),
                          "<tptz:ContinuousMove>"
                          "<tptz:ProfileToken>Profile_1</tptz:ProfileToken>"
                          "<tptz:Velocity><tt:Zoom x=\"0.5\"/></tptz:Velocity>"
                          "</tptz:ContinuousMove>");
    QVERIFY(!r.fault);
    QVERIFY(r.xml.contains("tptz:ContinuousMoveResponse"));
    QCOMPARE(m_camera->ptz()->zoomStatus(), PtzMoveStatus::Moving);
    QCOMPARE(m_camera->ptz()->panTiltStatus(), PtzMoveStatus::Idle);
}

void TstPtzService::continuousMovePanTiltOnly()
{
    const Result r = call(m_service, m_camera, QStringLiteral("ContinuousMove"),
                          "<tptz:ContinuousMove>"
                          "<tptz:ProfileToken>Profile_1</tptz:ProfileToken>"
                          "<tptz:Velocity><tt:PanTilt x=\"-1\" y=\"0.25\"/></tptz:Velocity>"
                          "</tptz:ContinuousMove>");
    QVERIFY(!r.fault);
    QCOMPARE(m_camera->ptz()->panTiltStatus(), PtzMoveStatus::Moving);
    QCOMPARE(m_camera->ptz()->zoomStatus(), PtzMoveStatus::Idle);
}

void TstPtzService::continuousMoveBothAxes()
{
    // zeep 栈的形态：PanTilt 与 Zoom 一起发。
    const Result r = call(m_service, m_camera, QStringLiteral("ContinuousMove"),
                          "<tptz:ContinuousMove>"
                          "<tptz:ProfileToken>Profile_1</tptz:ProfileToken>"
                          "<tptz:Velocity>"
                          // zoom 用正向：zoom 起点就是下限 0，往负方向走会立刻收敛回 Idle。
                          "<tt:PanTilt x=\"1\" y=\"1\"/><tt:Zoom x=\"0.5\"/>"
                          "</tptz:Velocity>"
                          "</tptz:ContinuousMove>");
    QVERIFY(!r.fault);
    QCOMPARE(m_camera->ptz()->panTiltStatus(), PtzMoveStatus::Moving);
    QCOMPARE(m_camera->ptz()->zoomStatus(), PtzMoveStatus::Moving);

    // 两个分量都缺席是无意义的命令，要挡下来而不是当成「全停」。
    const Result empty = call(m_service, m_camera, QStringLiteral("ContinuousMove"),
                              "<tptz:ContinuousMove>"
                              "<tptz:ProfileToken>Profile_1</tptz:ProfileToken>"
                              "<tptz:Velocity/>"
                              "</tptz:ContinuousMove>");
    QVERIFY(empty.fault);
    QCOMPARE(empty.subcode, QStringLiteral("ter:InvalidArgVal"));
}

void TstPtzService::getStatusReportsPosition()
{
    PtzVector position;
    position.pan = 0.5;
    position.tilt = -0.25;
    position.zoom = 0.75;
    m_camera->ptz()->setPosition(position);

    const Result r = call(m_service, m_camera, QStringLiteral("GetStatus"),
                          "<tptz:GetStatus>"
                          "<tptz:ProfileToken>Profile_1</tptz:ProfileToken>"
                          "</tptz:GetStatus>");
    QVERIFY(!r.fault);
    QVERIFY(r.xml.contains("tptz:PTZStatus"));
    QVERIFY(r.xml.contains("x=\"0.500000\""));
    QVERIFY(r.xml.contains("y=\"-0.250000\""));
    QVERIFY(r.xml.contains("x=\"0.750000\""));
    QVERIFY(r.xml.contains("<tt:PanTilt>IDLE</tt:PanTilt>"));
    QVERIFY(r.xml.contains("<tt:Zoom>IDLE</tt:Zoom>"));
    QVERIFY(r.xml.contains("tt:UtcTime"));
}

void TstPtzService::setPresetReturnShapes()
{
    const QByteArray body = "<tptz:SetPreset>"
                            "<tptz:ProfileToken>Profile_1</tptz:ProfileToken>"
                            "<tptz:PresetName>Gate</tptz:PresetName>"
                            "<tptz:PresetToken>7</tptz:PresetToken>"
                            "</tptz:SetPreset>";

    // 默认（quirk 关）：带 PresetToken 的对象形态。
    const Result object = call(m_service, m_camera, QStringLiteral("SetPreset"), body);
    QVERIFY(!object.fault);
    QVERIFY(object.xml.contains("<tptz:PresetToken>7</tptz:PresetToken>"));

    // C9 裸字符串：token 直接当响应元素的文本。
    Quirks q = m_camera->quirks();
    q.setEnabled(QuirkId::PtzSetPresetReturnShape, true);
    q.setParam(QuirkId::PtzSetPresetReturnShape, QStringLiteral("value"),
               QStringLiteral("bare_string"));
    m_camera->setQuirks(q);
    const Result bare = call(m_service, m_camera, QStringLiteral("SetPreset"), body);
    QVERIFY(!bare.fault);
    QVERIFY(bare.xml.contains("<tptz:SetPresetResponse>7</tptz:SetPresetResponse>"));
    QVERIFY(!bare.xml.contains("tptz:PresetToken"));

    // C9 空响应：客户端拿不到 token，只能自己重新 GetPresets 对一遍。
    q.setParam(QuirkId::PtzSetPresetReturnShape, QStringLiteral("value"),
               QStringLiteral("empty"));
    m_camera->setQuirks(q);
    const Result empty = call(m_service, m_camera, QStringLiteral("SetPreset"), body);
    QVERIFY(!empty.fault);
    QVERIFY(empty.xml.contains("<tptz:SetPresetResponse/>"));

    // 三次都打的是同一个槽位，预置位表里只该有一条。
    QCOMPARE(m_camera->ptz()->presets().size(), 1);
    QCOMPARE(m_camera->ptz()->presets().first().token, QStringLiteral("7"));
}

void TstPtzService::getPresetsUnsupportedShapes()
{
    call(m_service, m_camera, QStringLiteral("SetPreset"),
         "<tptz:SetPreset>"
         "<tptz:ProfileToken>Profile_1</tptz:ProfileToken>"
         "<tptz:PresetName>Gate</tptz:PresetName>"
         "</tptz:SetPreset>");

    const QByteArray body = "<tptz:GetPresets>"
                            "<tptz:ProfileToken>Profile_1</tptz:ProfileToken>"
                            "</tptz:GetPresets>";

    const Result normal = call(m_service, m_camera, QStringLiteral("GetPresets"), body);
    QVERIFY(!normal.fault);
    QVERIFY(normal.xml.contains("tptz:Preset "));
    QVERIFY(normal.xml.contains("<tt:Name>Gate</tt:Name>"));
    QVERIFY(normal.xml.contains("tt:PTZPosition"));

    // C8 第一档：ActionNotSupported。
    Quirks q = m_camera->quirks();
    q.setEnabled(QuirkId::PtzNoGetPresets, true);
    q.setParam(QuirkId::PtzNoGetPresets, QStringLiteral("value"),
               QStringLiteral("action_not_supported"));
    m_camera->setQuirks(q);
    const Result notSupported = call(m_service, m_camera, QStringLiteral("GetPresets"), body);
    QVERIFY(notSupported.fault);
    QCOMPARE(notSupported.subcode, QStringLiteral("ter:ActionNotSupported"));

    // C8 第二档：Fault 文本里带 "not implemented"，客户端靠正则认出来。
    q.setParam(QuirkId::PtzNoGetPresets, QStringLiteral("value"), QStringLiteral("text"));
    m_camera->setQuirks(q);
    const Result text = call(m_service, m_camera, QStringLiteral("GetPresets"), body);
    QVERIFY(text.fault);
    QVERIFY(text.reason.contains(QStringLiteral("not implemented")));

    // C8 第三档：不报错，就是空列表。
    q.setParam(QuirkId::PtzNoGetPresets, QStringLiteral("value"), QStringLiteral("empty_list"));
    m_camera->setQuirks(q);
    const Result emptyList = call(m_service, m_camera, QStringLiteral("GetPresets"), body);
    QVERIFY(!emptyList.fault);
    QVERIFY(emptyList.xml.contains("<tptz:GetPresetsResponse/>"));
    // 状态里其实还有那条预置位，只是不肯说。
    QCOMPARE(m_camera->ptz()->presets().size(), 1);
}

void TstPtzService::profileWithoutPtzConfigFaults()
{
    // C1：只有子码流挂 PTZConfiguration，客户端拿主码流 token 来调就吃 Fault，
    // 措辞正是真机那句 "does not reference a PTZ configuration"。
    Quirks q = m_camera->quirks();
    q.setEnabled(QuirkId::PtzConfigOnSubOnly, true);
    m_camera->setQuirks(q);

    const Result main = call(m_service, m_camera, QStringLiteral("ContinuousMove"),
                             "<tptz:ContinuousMove>"
                             "<tptz:ProfileToken>Profile_1</tptz:ProfileToken>"
                             "<tptz:Velocity><tt:PanTilt x=\"1\" y=\"0\"/></tptz:Velocity>"
                             "</tptz:ContinuousMove>");
    QVERIFY(main.fault);
    QCOMPARE(main.subcode, QStringLiteral("ter:NoPTZProfile"));
    QVERIFY(main.reason.contains(QStringLiteral("does not reference a PTZ configuration")));

    // C2 打开就一切照旧：一个 profile 都不挂声明，但 PTZ 照常可用。
    q.setEnabled(QuirkId::PtzUsableButUnadvertised, true);
    m_camera->setQuirks(q);
    const Result usable = call(m_service, m_camera, QStringLiteral("ContinuousMove"),
                               "<tptz:ContinuousMove>"
                               "<tptz:ProfileToken>Profile_1</tptz:ProfileToken>"
                               "<tptz:Velocity><tt:PanTilt x=\"1\" y=\"0\"/></tptz:Velocity>"
                               "</tptz:ContinuousMove>");
    QVERIFY(!usable.fault);

    // 不存在的 profile 一律 NoProfile。
    const Result unknown = call(m_service, m_camera, QStringLiteral("GetStatus"),
                                "<tptz:GetStatus>"
                                "<tptz:ProfileToken>NoSuchProfile</tptz:ProfileToken>"
                                "</tptz:GetStatus>");
    QVERIFY(unknown.fault);
    QCOMPARE(unknown.subcode, QStringLiteral("ter:NoProfile"));
}

QTEST_GUILESS_MAIN(TstPtzService)

#include "tst_ptz_service.moc"
