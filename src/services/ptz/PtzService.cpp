#include "services/ptz/PtzService.h"

#include "core/VirtualCamera.h"
#include "ptz/PtzState.h"
#include "services/ProfileQuirks.h"
#include "services/events/EventsService.h"
#include "soap/Namespaces.h"
#include "soap/XmlNode.h"

#include <QtCore/QDateTime>

namespace onvifsim {
namespace {

using namespace profilequirks::ptzspace;

// C3 的四档能力声明。quirk 关着就是 Full。
enum class SpacesMode { Full, Empty, DefaultOnly, PanOnly };

SpacesMode spacesMode(const Quirks &q)
{
    if (!q.isEnabled(QuirkId::PtzSpacesEmpty))
        return SpacesMode::Full;
    const QString value = q.choice(QuirkId::PtzSpacesEmpty, QStringLiteral("empty"));
    if (value == QLatin1String("default_only"))
        return SpacesMode::DefaultOnly;
    if (value == QLatin1String("pan_only"))
        return SpacesMode::PanOnly;
    return SpacesMode::Empty;
}

// C4：Min == Max。真机上见到的是 Min = Max = 1，客户端的判据「Min 或 Max 任一非零」
// 就是为这种退化区间放宽出来的，所以这里也统一塌到 Max 上而不是塌到 0。
void writeRange(XmlWriter &w, const QString &qname, double min, double max, bool degenerate)
{
    w.floatRange(qname, degenerate ? max : min, max);
}

void writeSpace(XmlWriter &w, const QString &qname, const char *uri, double xMin, double xMax,
                bool hasY, double yMin, double yMax, bool degenerate)
{
    XmlWriter::Scope s(w, qname);
    w.element(QStringLiteral("tt:URI"), QString::fromLatin1(uri));
    writeRange(w, QStringLiteral("tt:XRange"), xMin, xMax, degenerate);
    if (hasY)
        writeRange(w, QStringLiteral("tt:YRange"), yMin, yMax, degenerate);
}

// SupportedPTZSpaces（GetNodes）与 Spaces（GetConfigurationOptions）内容相同，
// 只有外层元素名不一样，所以共用这一个写法。
void writeSpaces(XmlWriter &w, const QString &qname, SpacesMode mode, bool degenerate)
{
    XmlWriter::Scope s(w, qname);
    if (mode == SpacesMode::Empty || mode == SpacesMode::DefaultOnly)
        return;   // 完全空的 SupportedPTZSpaces —— TL-IPC652P-A4 就是这样

    if (mode == SpacesMode::PanOnly) {
        // 只有 pan 轴：YRange 塌成 0..0，客户端判 tilt 不可用而 pan 可用，
        // 于是对角键与点画面控制失效但左右键还在。
        writeSpace(w, QStringLiteral("tt:ContinuousPanTiltVelocitySpace"), kContinuousPanTilt,
                   -1.0, 1.0, true, 0.0, 0.0, degenerate);
        return;
    }

    writeSpace(w, QStringLiteral("tt:AbsolutePanTiltPositionSpace"), kAbsolutePanTilt,
               -1.0, 1.0, true, -1.0, 1.0, degenerate);
    writeSpace(w, QStringLiteral("tt:AbsoluteZoomPositionSpace"), kAbsoluteZoom,
               0.0, 1.0, false, 0.0, 0.0, degenerate);
    writeSpace(w, QStringLiteral("tt:RelativePanTiltTranslationSpace"), kRelativePanTilt,
               -1.0, 1.0, true, -1.0, 1.0, degenerate);
    writeSpace(w, QStringLiteral("tt:RelativeZoomTranslationSpace"), kRelativeZoom,
               -1.0, 1.0, false, 0.0, 0.0, degenerate);
    writeSpace(w, QStringLiteral("tt:ContinuousPanTiltVelocitySpace"), kContinuousPanTilt,
               -1.0, 1.0, true, -1.0, 1.0, degenerate);
    writeSpace(w, QStringLiteral("tt:ContinuousZoomVelocitySpace"), kContinuousZoom,
               -1.0, 1.0, false, 0.0, 0.0, degenerate);
    writeSpace(w, QStringLiteral("tt:PanTiltSpeedSpace"), kPanTiltSpeed,
               0.0, 1.0, false, 0.0, 0.0, degenerate);
    writeSpace(w, QStringLiteral("tt:ZoomSpeedSpace"), kZoomSpeed,
               0.0, 1.0, false, 0.0, 0.0, degenerate);
}

void writePanTilt(XmlWriter &w, double x, double y, const char *space)
{
    w.start(QStringLiteral("tt:PanTilt"));
    w.attr(QStringLiteral("x"), QString::number(x, 'f', 6));
    w.attr(QStringLiteral("y"), QString::number(y, 'f', 6));
    w.attr(QStringLiteral("space"), QString::fromLatin1(space));
    w.end();
}

void writeZoom(XmlWriter &w, double x, const char *space)
{
    w.start(QStringLiteral("tt:Zoom"));
    w.attr(QStringLiteral("x"), QString::number(x, 'f', 6));
    w.attr(QStringLiteral("space"), QString::fromLatin1(space));
    w.end();
}

// tt:PTZVector：两个分量都是可选的，按 hasPanTilt / hasZoom 决定写不写。
void writeVector(XmlWriter &w, const QString &qname, const PtzVector &v, const char *panTiltSpace,
                 const char *zoomSpace)
{
    XmlWriter::Scope s(w, qname);
    if (v.hasPanTilt)
        writePanTilt(w, v.pan, v.tilt, panTiltSpace);
    if (v.hasZoom)
        writeZoom(w, v.zoom, zoomSpace);
}

// 请求里的 tt:PTZVector / tt:PTZSpeed。分量缺席时把对应的 has* 置 false ——
// 参照客户端的手写栈只发被命令的那个轴，两种形态都要接住。
PtzVector readVector(const XmlNode *node)
{
    PtzVector v;
    v.hasPanTilt = false;
    v.hasZoom = false;
    if (!node)
        return v;
    // 属性解析必须查 ok。这里的 0 不是中性值：对 AbsoluteMove 而言
    // pan=0 是「把云台开到正中」，对 ContinuousMove 是「这个轴不动」。
    // `<tt:PanTilt y="0.5"/>`（漏了 x）或者 `x="abc"` 如果被当成有效指令，
    // 客户端得到的是一次**成功执行的错误动作**，不是错误码。
    // XmlNode::childDouble / SoapContext::argDouble 都是查 ok 的，属性这条路漏了。
    auto readAttr = [](const XmlNode *n, const char *name, double *out) {
        bool ok = false;
        const double value = n->attribute(QLatin1String(name)).toDouble(&ok);
        if (ok)
            *out = value;
        return ok;
    };
    if (const XmlNode *pt = node->child(QStringLiteral("PanTilt"))) {
        const bool hasX = readAttr(pt, "x", &v.pan);
        const bool hasY = readAttr(pt, "y", &v.tilt);
        v.hasPanTilt = hasX || hasY;
    }
    if (const XmlNode *zoom = node->child(QStringLiteral("Zoom")))
        v.hasZoom = readAttr(zoom, "x", &v.zoom);
    return v;
}


const char *moveStatusName(PtzMoveStatus status)
{
    switch (status) {
    case PtzMoveStatus::Idle:   return "IDLE";
    case PtzMoveStatus::Moving: return "MOVING";
    case PtzMoveStatus::Unknown: break;
    }
    return "UNKNOWN";
}

// 取并校验 ProfileToken。返回 false 表示已经产生 Fault，handler 应立即 return。
bool requirePtzProfile(SoapContext &ctx)
{
    if (!ctx.camera)
        return false;
    const CameraModel &model = ctx.camera->model();
    const QString token = ctx.arg(QStringLiteral("ProfileToken"));
    if (token.isEmpty()) {
        ctx.fault(QString::fromLatin1(ter::NoProfile),
                  QStringLiteral("Missing ProfileToken"));
        return false;
    }
    int index = -1;
    for (int i = 0; i < model.profiles.size(); ++i) {
        if (model.profiles.at(i).token == token) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        ctx.fault(QString::fromLatin1(ter::NoProfile),
                  QStringLiteral("No profile with token %1").arg(token));
        return false;
    }
    if (!profilequirks::acceptsPtz(ctx.quirks(), model, index)) {
        // 措辞照抄真机：客户端就是靠这句话认出「profile 选错了」。
        ctx.fault(QString::fromLatin1(ter::NoPTZProfile),
                  QStringLiteral("The requested profile token does not reference a PTZ "
                                 "configuration"));
        return false;
    }
    return true;
}

void writeConfiguration(SoapContext &ctx, XmlWriter &w, SpacesMode mode, bool degenerate)
{
    const PtzNodeConfig &node = ctx.camera->model().ptzNode;

    w.start(QStringLiteral("tt:PTZConfiguration"));
    w.attr(QStringLiteral("token"), node.configToken);
    w.element(QStringLiteral("tt:Name"), node.configName);
    w.element(QStringLiteral("tt:UseCount"), 1);
    w.element(QStringLiteral("tt:NodeToken"), node.nodeToken);

    // C3 的 default_only 档：SupportedPTZSpaces 空着，但这里给出 Default*Space，
    // 客户端的回落分支（「一条速度空间都没声明就看 Default*」）才有东西可看。
    if (mode == SpacesMode::Full || mode == SpacesMode::DefaultOnly) {
        w.element(QStringLiteral("tt:DefaultAbsolutePantTiltPositionSpace"),
                  QString::fromLatin1(kAbsolutePanTilt));
        w.element(QStringLiteral("tt:DefaultAbsoluteZoomPositionSpace"),
                  QString::fromLatin1(kAbsoluteZoom));
        w.element(QStringLiteral("tt:DefaultRelativePanTiltTranslationSpace"),
                  QString::fromLatin1(kRelativePanTilt));
        w.element(QStringLiteral("tt:DefaultRelativeZoomTranslationSpace"),
                  QString::fromLatin1(kRelativeZoom));
        w.element(QStringLiteral("tt:DefaultContinuousPanTiltVelocitySpace"),
                  QString::fromLatin1(kContinuousPanTilt));
        w.element(QStringLiteral("tt:DefaultContinuousZoomVelocitySpace"),
                  QString::fromLatin1(kContinuousZoom));
    }

    {
        XmlWriter::Scope speed(w, QStringLiteral("tt:DefaultPTZSpeed"));
        writePanTilt(w, 1.0, 1.0, kPanTiltSpeed);
        writeZoom(w, 1.0, kZoomSpeed);
    }
    w.element(QStringLiteral("tt:DefaultPTZTimeout"), QStringLiteral("PT5S"));
    {
        XmlWriter::Scope limits(w, QStringLiteral("tt:PanTiltLimits"));
        XmlWriter::Scope range(w, QStringLiteral("tt:Range"));
        w.element(QStringLiteral("tt:URI"), QString::fromLatin1(kContinuousPanTilt));
        writeRange(w, QStringLiteral("tt:XRange"), -1.0, 1.0, degenerate);
        writeRange(w, QStringLiteral("tt:YRange"), -1.0, 1.0, degenerate);
    }
    {
        XmlWriter::Scope limits(w, QStringLiteral("tt:ZoomLimits"));
        XmlWriter::Scope range(w, QStringLiteral("tt:Range"));
        w.element(QStringLiteral("tt:URI"), QString::fromLatin1(kContinuousZoom));
        writeRange(w, QStringLiteral("tt:XRange"), 0.0, 1.0, degenerate);
    }
    w.end();
}

void writeNode(SoapContext &ctx, XmlWriter &w)
{
    const PtzNodeConfig &node = ctx.camera->model().ptzNode;
    const Quirks &q = ctx.quirks();

    w.start(QStringLiteral("tptz:PTZNode"));
    w.attr(QStringLiteral("token"), node.nodeToken);
    w.attr(QStringLiteral("FixedHomePosition"), QStringLiteral("false"));
    w.attr(QStringLiteral("GeoMove"), QStringLiteral("false"));
    w.element(QStringLiteral("tt:Name"), node.nodeName);
    writeSpaces(w, QStringLiteral("tt:SupportedPTZSpaces"), spacesMode(q),
                q.isEnabled(QuirkId::PtzRangeMinEqualsMax));
    w.element(QStringLiteral("tt:MaximumNumberOfPresets"), node.maxPresets);
    w.element(QStringLiteral("tt:HomeSupported"), node.homeSupported);
    w.element(QStringLiteral("tt:AuxiliaryCommands"), QStringLiteral("tt:Wiper|On"));
    w.element(QStringLiteral("tt:AuxiliaryCommands"), QStringLiteral("tt:Wiper|Off"));
    w.element(QStringLiteral("tt:AuxiliaryCommands"), QStringLiteral("tt:IRLamp|On"));
    w.element(QStringLiteral("tt:AuxiliaryCommands"), QStringLiteral("tt:IRLamp|Off"));
    w.end();
}

// C5：真机在 ContinuousMove 带非零 Zoom 时把请求字节原样回显再挂个 500。
// 这不是 SOAP 层能表达的东西，只能接管响应用 rawOverride 直接写字节流。
bool maybeMalformedZoomResponse(SoapContext &ctx, const PtzVector &velocity)
{
    if (!ctx.quirks().isEnabled(QuirkId::PtzZoomMalformedResponse))
        return false;
    if (!velocity.hasZoom || qFuzzyIsNull(velocity.zoom))
        return false;
    if (!ctx.exchange)
        return false;

    ctx.takeOverResponse();
    HttpResponse response;
    response.rawOverride = QByteArray("HTTP/1.1 500 Internal Server Error\r\n\r\n")
                           + (ctx.http ? ctx.http->body : QByteArray());
    response.forceClose = true;
    ctx.exchange->respond(response);
    return true;
}

} // namespace

PtzService::PtzService()
{
    registerCapabilityOps();
    registerMotionOps();
    registerPresetOps();
    registerHomeOps();
    registerAuxiliaryOps();
}

void PtzService::registerCapabilityOps()
{
    // ---- 能力与拓扑 ----
    cameraOp("GetServiceCapabilities", AuthLevel::PreAuth, [this](SoapContext &ctx) {
        XmlWriter::Scope s(*ctx.out, QStringLiteral("tptz:GetServiceCapabilitiesResponse"));
        writeServiceCapabilities(ctx);
    });

    cameraOp("GetNodes", AuthLevel::User, [](SoapContext &ctx) {
        XmlWriter::Scope s(*ctx.out, QStringLiteral("tptz:GetNodesResponse"));
        writeNode(ctx, *ctx.out);
    });

    cameraOp("GetNode", AuthLevel::User, [](SoapContext &ctx) {
        const QString token = ctx.arg(QStringLiteral("NodeToken"));
        if (!token.isEmpty() && token != ctx.camera->model().ptzNode.nodeToken) {
            ctx.fault(QString::fromLatin1(ter::NoToken),
                      QStringLiteral("No PTZ node with token %1").arg(token));
            return;
        }
        XmlWriter::Scope s(*ctx.out, QStringLiteral("tptz:GetNodeResponse"));
        writeNode(ctx, *ctx.out);
    });

    cameraOp("GetConfigurations", AuthLevel::User, [](SoapContext &ctx) {
        const Quirks &q = ctx.quirks();
        XmlWriter::Scope s(*ctx.out, QStringLiteral("tptz:GetConfigurationsResponse"));
        writeConfiguration(ctx, *ctx.out, spacesMode(q),
                           q.isEnabled(QuirkId::PtzRangeMinEqualsMax));
    });

    cameraOp("GetConfiguration", AuthLevel::User, [](SoapContext &ctx) {
        const QString token = ctx.arg(QStringLiteral("PTZConfigurationToken"));
        if (!token.isEmpty() && token != ctx.camera->model().ptzNode.configToken) {
            ctx.fault(QString::fromLatin1(ter::NoConfig),
                      QStringLiteral("No PTZ configuration with token %1").arg(token));
            return;
        }
        const Quirks &q = ctx.quirks();
        XmlWriter::Scope s(*ctx.out, QStringLiteral("tptz:GetConfigurationResponse"));
        writeConfiguration(ctx, *ctx.out, spacesMode(q),
                           q.isEnabled(QuirkId::PtzRangeMinEqualsMax));
    });

    cameraOp("GetConfigurationOptions", AuthLevel::User, [](SoapContext &ctx) {
        // 参数名在 WSDL 里是 ConfigurationToken，但真机上两种写法都见得到。
        QString token = ctx.arg(QStringLiteral("ConfigurationToken"));
        if (token.isEmpty())
            token = ctx.arg(QStringLiteral("PTZConfigurationToken"));
        if (!token.isEmpty() && token != ctx.camera->model().ptzNode.configToken) {
            ctx.fault(QString::fromLatin1(ter::NoConfig),
                      QStringLiteral("No PTZ configuration with token %1").arg(token));
            return;
        }
        const Quirks &q = ctx.quirks();
        XmlWriter::Scope s(*ctx.out, QStringLiteral("tptz:GetConfigurationOptionsResponse"));
        XmlWriter::Scope options(*ctx.out, QStringLiteral("tptz:PTZConfigurationOptions"));
        writeSpaces(*ctx.out, QStringLiteral("tt:Spaces"), spacesMode(q),
                    q.isEnabled(QuirkId::PtzRangeMinEqualsMax));
        XmlWriter::Scope timeout(*ctx.out, QStringLiteral("tt:PTZTimeout"));
        ctx.out->element(QStringLiteral("tt:Min"), QStringLiteral("PT0S"));
        ctx.out->element(QStringLiteral("tt:Max"), QStringLiteral("PT10S"));
    });

}

void PtzService::registerMotionOps()
{
    // ---- 运动 ----
    cameraOp("ContinuousMove", AuthLevel::Operator, [](SoapContext &ctx) {
        if (!requirePtzProfile(ctx))
            return;
        const PtzVector velocity = readVector(ctx.argNode(QStringLiteral("Velocity")));
        if (!velocity.hasPanTilt && !velocity.hasZoom) {
            ctx.fault(QString::fromLatin1(ter::InvalidArgVal),
                      QStringLiteral("Velocity carries neither PanTilt nor Zoom"));
            return;
        }
        // C5 先于状态变更判：真机是连命令都没执行就把连接搞砸了。
        if (maybeMalformedZoomResponse(ctx, velocity))
            return;

        // Timeout 是可选的 xs:duration；给不出秒数就当「一直转到 Stop」。
        int timeoutMs = 0;
        const QString timeout = ctx.arg(QStringLiteral("Timeout"));
        if (!timeout.isEmpty()) {
            qint64 secs = 0;
            if (events::parseIsoDuration(timeout, &secs))
                timeoutMs = static_cast<int>(qBound<qint64>(0, secs * 1000, 3600000LL));
        }
        ctx.camera->ptz()->continuousMove(velocity, timeoutMs);
        ctx.out->emptyElement(QStringLiteral("tptz:ContinuousMoveResponse"));
    });

    cameraOp("RelativeMove", AuthLevel::Operator, [](SoapContext &ctx) {
        if (!requirePtzProfile(ctx))
            return;
        const PtzVector translation = readVector(ctx.argNode(QStringLiteral("Translation")));
        const PtzVector speed = readVector(ctx.argNode(QStringLiteral("Speed")));
        ctx.camera->ptz()->relativeMove(translation, speed);
        ctx.out->emptyElement(QStringLiteral("tptz:RelativeMoveResponse"));
    });

    cameraOp("AbsoluteMove", AuthLevel::Operator, [](SoapContext &ctx) {
        if (!requirePtzProfile(ctx))
            return;
        const PtzVector target = readVector(ctx.argNode(QStringLiteral("Position")));
        const PtzVector speed = readVector(ctx.argNode(QStringLiteral("Speed")));
        ctx.camera->ptz()->absoluteMove(target, speed);
        ctx.out->emptyElement(QStringLiteral("tptz:AbsoluteMoveResponse"));
    });

    cameraOp("Stop", AuthLevel::Operator, [](SoapContext &ctx) {
        if (!requirePtzProfile(ctx))
            return;
        // 两个标志都是可选的，规范规定缺席即 true。Stop 是绝不能被吞掉的那条命令，
        // 宁可多停一个轴也不能因为参数没解析出来就什么都不做 —— 真机上那意味着云台一直转。
        ctx.camera->ptz()->stop(ctx.argBool(QStringLiteral("PanTilt"), true),
                                ctx.argBool(QStringLiteral("Zoom"), true));
        ctx.out->emptyElement(QStringLiteral("tptz:StopResponse"));
    });

    cameraOp("GetStatus", AuthLevel::User, [](SoapContext &ctx) {
        if (!requirePtzProfile(ctx))
            return;
        PtzState *ptz = ctx.camera->ptz();
        const PtzVector position = ptz->position();

        XmlWriter::Scope s(*ctx.out, QStringLiteral("tptz:GetStatusResponse"));
        XmlWriter::Scope status(*ctx.out, QStringLiteral("tptz:PTZStatus"));
        {
            // Position 一律两个分量都写：客户端的十字准星要的是完整位置。
            PtzVector full = position;
            full.hasPanTilt = true;
            full.hasZoom = true;
            writeVector(*ctx.out, QStringLiteral("tt:Position"), full, kAbsolutePanTilt,
                        kAbsoluteZoom);
        }
        {
            XmlWriter::Scope move(*ctx.out, QStringLiteral("tt:MoveStatus"));
            ctx.out->element(QStringLiteral("tt:PanTilt"),
                             QString::fromLatin1(moveStatusName(ptz->panTiltStatus())));
            ctx.out->element(QStringLiteral("tt:Zoom"),
                             QString::fromLatin1(moveStatusName(ptz->zoomStatus())));
        }
        ctx.out->element(QStringLiteral("tt:UtcTime"),
                         ctx.camera->deviceTimeUtc().toString(Qt::ISODate));
    });

}

void PtzService::registerPresetOps()
{
    // ---- 预置位 ----
    cameraOp("GetPresets", AuthLevel::User, [](SoapContext &ctx) {
        const Quirks &q = ctx.quirks();
        if (q.isEnabled(QuirkId::PtzNoGetPresets)) {
            const QString mode = q.choice(QuirkId::PtzNoGetPresets,
                                          QStringLiteral("action_not_supported"));
            if (mode == QLatin1String("text")) {
                // 客户端认的是 Fault 文本里的 "not implemented"，子码是什么无所谓。
                SoapFault f;
                f.subcode = QString::fromLatin1(ter::Receiver);
                f.reason = QStringLiteral("Method GetPresets is not implemented");
                f.senderFault = false;
                ctx.fault(f);
                return;
            }
            if (mode == QLatin1String("empty_list")) {
                ctx.out->emptyElement(QStringLiteral("tptz:GetPresetsResponse"));
                return;
            }
            ctx.fault(QString::fromLatin1(ter::ActionNotSupported),
                      QStringLiteral("GetPresets is not supported"));
            return;
        }
        if (!requirePtzProfile(ctx))
            return;

        XmlWriter::Scope s(*ctx.out, QStringLiteral("tptz:GetPresetsResponse"));
        const QList<PtzPreset> presets = ctx.camera->ptz()->presets();
        for (const PtzPreset &preset : presets) {
            ctx.out->start(QStringLiteral("tptz:Preset"));
            ctx.out->attr(QStringLiteral("token"), preset.token);
            ctx.out->element(QStringLiteral("tt:Name"), preset.name);
            // 功能槽（巡航扫描 / 远程重启）没有位置，PTZPosition 整个不写。
            if (preset.hasPosition) {
                PtzVector position = preset.position;
                position.hasPanTilt = true;
                position.hasZoom = true;
                writeVector(*ctx.out, QStringLiteral("tt:PTZPosition"), position,
                            kAbsolutePanTilt, kAbsoluteZoom);
            }
            ctx.out->end();
        }
    });

    cameraOp("SetPreset", AuthLevel::Operator, [](SoapContext &ctx) {
        if (!requirePtzProfile(ctx))
            return;
        QString error;
        const QString token = ctx.camera->ptz()->setPreset(ctx.arg(QStringLiteral("PresetName")),
                                                           ctx.arg(QStringLiteral("PresetToken")),
                                                           &error);
        if (token.isEmpty()) {
            ctx.fault(error.isEmpty() ? QString::fromLatin1(ter::TooManyPresets) : error,
                      QStringLiteral("Preset storage is full"));
            return;
        }

        // C9：真机三种形态都见过，客户端必须三种都能接。
        const QString shape = ctx.quirks().isEnabled(QuirkId::PtzSetPresetReturnShape)
                                  ? ctx.quirks().choice(QuirkId::PtzSetPresetReturnShape,
                                                        QStringLiteral("object"))
                                  : QStringLiteral("object");
        if (shape == QLatin1String("bare_string")) {
            // token 直接当响应元素的文本，没有 PresetToken 这一层。
            ctx.out->element(QStringLiteral("tptz:SetPresetResponse"), token);
            return;
        }
        if (shape == QLatin1String("empty")) {
            ctx.out->emptyElement(QStringLiteral("tptz:SetPresetResponse"));
            return;
        }
        XmlWriter::Scope s(*ctx.out, QStringLiteral("tptz:SetPresetResponse"));
        ctx.out->element(QStringLiteral("tptz:PresetToken"), token);
    });

    cameraOp("RemovePreset", AuthLevel::Operator, [](SoapContext &ctx) {
        if (!requirePtzProfile(ctx))
            return;
        const QString token = ctx.arg(QStringLiteral("PresetToken"));
        if (!ctx.camera->ptz()->removePreset(token)) {
            ctx.fault(QString::fromLatin1(ter::NoSuchPreset),
                      QStringLiteral("No preset with token %1").arg(token));
            return;
        }
        ctx.out->emptyElement(QStringLiteral("tptz:RemovePresetResponse"));
    });

    cameraOp("GotoPreset", AuthLevel::Operator, [](SoapContext &ctx) {
        if (!requirePtzProfile(ctx))
            return;
        const QString token = ctx.arg(QStringLiteral("PresetToken"));
        const PtzVector speed = readVector(ctx.argNode(QStringLiteral("Speed")));
        if (!ctx.camera->ptz()->gotoPreset(token, speed)) {
            ctx.fault(QString::fromLatin1(ter::NoSuchPreset),
                      QStringLiteral("No preset with token %1").arg(token));
            return;
        }
        ctx.out->emptyElement(QStringLiteral("tptz:GotoPresetResponse"));
    });

}

void PtzService::registerHomeOps()
{
    // ---- Home ----
    cameraOp("GotoHomePosition", AuthLevel::Operator, [](SoapContext &ctx) {
        if (!requirePtzProfile(ctx))
            return;
        const PtzVector speed = readVector(ctx.argNode(QStringLiteral("Speed")));
        if (!ctx.camera->ptz()->gotoHome(speed)) {
            ctx.fault(QString::fromLatin1(ter::NoHomePosition),
                      QStringLiteral("No home position has been set"));
            return;
        }
        ctx.out->emptyElement(QStringLiteral("tptz:GotoHomePositionResponse"));
    });

    cameraOp("SetHomePosition", AuthLevel::Operator, [](SoapContext &ctx) {
        if (!requirePtzProfile(ctx))
            return;
        ctx.camera->ptz()->setHomePosition();
        ctx.out->emptyElement(QStringLiteral("tptz:SetHomePositionResponse"));
    });

}

void PtzService::registerAuxiliaryOps()
{
    // ---- 辅助命令 ----
    cameraOp("SendAuxiliaryCommand", AuthLevel::Operator, [](SoapContext &ctx) {
        if (!requirePtzProfile(ctx))
            return;
        const QString command = ctx.arg(QStringLiteral("AuxiliaryData"));
        ctx.camera->ptz()->sendAuxiliaryCommand(command);
        XmlWriter::Scope s(*ctx.out, QStringLiteral("tptz:SendAuxiliaryCommandResponse"));
        ctx.out->element(QStringLiteral("tptz:AuxiliaryResponse"), command);
    });
}


const char *PtzService::serviceNamespace() const
{
    return ns::Ptz;
}

const char *PtzService::serviceName() const
{
    return "ptz";
}

QString PtzService::defaultPath() const
{
    return QStringLiteral("/onvif/ptz_service");
}

void PtzService::writeServiceCapabilities(SoapContext &ctx) const
{
    XmlWriter &w = *ctx.out;
    w.start(QStringLiteral("tptz:Capabilities"));
    w.attr(QStringLiteral("EFlip"), QStringLiteral("false"));
    w.attr(QStringLiteral("Reverse"), QStringLiteral("false"));
    w.attr(QStringLiteral("GetCompatibleConfigurations"), QStringLiteral("false"));
    // 移动了但 GetStatus 不动的那条 quirk 打开时，如实告诉客户端「没有状态位置」
    // 反而不像真机 —— 真机是照常声明支持然后骗人，所以这里固定 true。
    w.attr(QStringLiteral("MoveStatus"), QStringLiteral("true"));
    w.attr(QStringLiteral("StatusPosition"), QStringLiteral("true"));
    w.end();
}

namespace services {

SoapService *createPtz()
{
    return new PtzService;
}

} // namespace services
} // namespace onvifsim
