#include "rtsp/RtspSession.h"

#include "core/LogBus.h"
#include "core/Quirks.h"
#include "net/NetUtil.h"
#include "core/VirtualCamera.h"
#include "media/AudioSource.h"
#include "media/H264Source.h"
#include "net/HttpAuth.h"
#include "rtsp/RtpPacket_p.h"
#include "rtsp/RtpReceiver.h"
#include "rtsp/RtpSender.h"
#include "rtsp/RtspInternal_p.h"
#include "rtsp/RtspServer.h"
#include "rtsp/Sdp.h"

#include "version.h"

#include <QtCore/QByteArrayList>
#include <QtCore/QElapsedTimer>
#include <QtCore/QLocale>
#include <QtCore/QRandomGenerator>
#include <QtCore/QTimer>
#include <QtNetwork/QTcpSocket>
#include <QtNetwork/QUdpSocket>

namespace onvifsim {
namespace {

// 一条请求的头部上限。超了说明对面在灌垃圾（或者是 F1 那种畸形报文），直接断。
constexpr int kMaxRequestBytes = 64 * 1024;
// RTP 负载上限，留出以太网 MTU 里 IP + UDP + RTP 头的余量；
// interleaved 的长度字段只有 16 位，这个值也远在其下。
constexpr int kRtpMtu = 1400;
// 发送泵的心跳。比一帧（15fps ≈ 66ms）密得多，真正的节奏由 elapsed 决定。
constexpr int kPumpIntervalMs = 5;
// 落后超过这么多就直接对齐到当前时刻：进程被断点 / 挂起之后暴力补发几百帧毫无意义。
constexpr qint64 kMaxCatchupFrames = 30;
constexpr qint64 kMaxCatchupAudioPackets = 100;
// E19 生效时把 Qt 的接收缓冲压到这么小，内核缓冲才会跟着填满。
constexpr int kStopDrainingBufferBytes = 8192;

const char kBackchannelTag[] = "www.onvif.org/ver20/backchannel";

RtspTrackKind kindOf(sdp::TrackRole role)
{
    switch (role) {
    case sdp::TrackRole::Video:
        return RtspTrackKind::Video;
    case sdp::TrackRole::Audio:
        return RtspTrackKind::Audio;
    case sdp::TrackRole::Backchannel:
        return RtspTrackKind::Backchannel;
    }
    return RtspTrackKind::Video;
}

} // namespace

struct RtspSession::Private {
    RtspSession *q = nullptr;
    RtspServer *server = nullptr;
    VirtualCamera *camera = nullptr;
    // nonce 表是 RtspServer 的，一台相机一份（设备级 nonce，见 RtspServer::auth()）。
    HttpAuth *auth = nullptr;
    QTcpSocket *socket = nullptr;

    QByteArray sessionId;
    QByteArray inBuffer;
    QString profileToken;
    bool backchannelRequested = false;
    bool established = false;     // 至少 SETUP 过一条轨
    bool playing = false;
    bool ended = false;
    QDateTime startedAt;
    QDateTime lastActivity;

    QVector<RtspTrack> tracks;
    QVector<sdp::TrackRole> layout;
    SdpOptions sdpOptions;

    RtpReceiver *receiver = nullptr;
    QUdpSocket *backchannelUdp = nullptr;

    // 媒体源按会话独立：每条会话有自己的读位置，互不干扰。
    H264Source *video = nullptr;
    AudioSource *audio = nullptr;
    QTimer *pump = nullptr;
    QElapsedTimer clock;
    // 已经发出去的帧 / 包数是单调计数器，PAUSE → PLAY 之后接着往下走：
    // 归零会让 RTP 时间戳倒退，客户端的抖动缓冲会把之后一段全丢掉。
    qint64 videoFrames = 0;
    qint64 audioPackets = 0;
    qint64 videoFrameBase = 0;
    qint64 audioPacketBase = 0;
    int freezeFrame = -1;

    bool stopDraining = false;
    QTimer *stopDrainTimer = nullptr;
    void resumeDraining();

    // 三个相机访问器里只有这个原来不判空，而隔壁 handleRequest() 里明写着
    // camera 可能为空。返回一个静态空 Quirks 比解引用空指针好 ——
    // 所有 isEnabled() 都会返回 false，也就是「没开任何怪癖」，正是安全的默认。
    const Quirks &quirks() const
    {
        static const Quirks kNone;
        return camera ? camera->quirks() : kNone;
    }
    LogBus *bus() const { return camera ? camera->logBus() : LogBus::global(); }
    QString cameraId() const { return camera ? camera->id() : QString(); }
    void log(LogLevel level, const QString &summary, const QString &detail = QString(),
             const QString &quirkKey = QString());

    void onReadyRead();
    void processBuffer();
    void handleInterleaved(int channel, const QByteArray &payload);
    void handleRequest(const RtspRequest &request);
    void send(const RtspResponse &response);

    bool authorize(const RtspRequest &request, RtspResponse *challenge);
    bool rejectUnsupportedRequire(const RtspRequest &request, RtspResponse *response);
    bool wantsBackchannel(const RtspRequest &request) const;

    RtspResponse handleOptions(const RtspRequest &request);
    RtspResponse handleDescribe(const RtspRequest &request);
    RtspResponse handleSetup(const RtspRequest &request);
    RtspResponse handlePlay(const RtspRequest &request);
    RtspResponse handlePause(const RtspRequest &request);
    RtspResponse handleTeardown(const RtspRequest &request);

    QString absoluteBase(const RtspRequest &request) const;
    bool prepareProfile(const QString &target, bool backchannel, const RtspRequest &request);
    void ensureVideoSource(const MediaProfile &profile);
    void ensureAudioSource(AudioCodec codec);
    AudioCodec backchannelCodecFor(const MediaProfile &profile) const;

    RtspTrack *trackByKind(RtspTrackKind kind);
    bool hasBackchannelTrack() const;
    bool maxSessionsReached() const;

    void startPump();
    void stopPump();
    void onPumpTick();
    void pumpVideo(qint64 elapsedUs);
    void pumpAudio(qint64 elapsedUs);
    double effectiveFps() const;
    void finish();
};

void RtspSession::Private::log(LogLevel level, const QString &summary, const QString &detail,
                               const QString &quirkKey)
{
    LogBus *logBus = bus();
    if (!logBus)
        return;
    LogRecord record;
    record.level = level;
    record.cameraId = cameraId();
    record.category = QString::fromLatin1(logcat::Rtsp);
    record.peer = q->peerString();
    record.summary = summary;
    record.detail = detail;
    record.ok = level != LogLevel::Error && level != LogLevel::Warning;
    record.quirkKey = quirkKey;
    logBus->post(record);
}

// ---------------------------------------------------------------------------
// 收数据：同一条 TCP 连接上 RTSP 文本请求与 interleaved 二进制帧是混在一起的。
// ---------------------------------------------------------------------------

// E19 关掉之后要能恢复：quirk 是可以随时开关的，关掉之后行为必须立刻回到正常，
// 这是本项目所有 quirk 的通用约定。这里尤其要紧 —— 读缓冲被压小之后 Qt 会停掉读通知，
// 连对端已经断开都发现不了，会话就一直挂在那儿占着并发槽位。
void RtspSession::Private::resumeDraining()
{
    stopDrainTimer->stop();                   // 还没到点的话连触发都免了
    if (!socket)
        return;
    // 读缓冲的恢复必须在 stopDraining 判断**之前**：handlePlay() 在 PLAY 当时
    // 就把缓冲压到了 kStopDrainingBufferBytes，而 stopDraining 要等 after_ms
    // 的定时器到点才置 true。用户在这段窗口里关掉 E19 的话，老写法只停了定时器
    // 就 return，setReadBufferSize(0) 永远不会执行 —— 会话从此挂死占着槽位，
    // 正是这个函数上面那段注释在警告的情形。
    socket->setReadBufferSize(0);             // 0 = 不限，恢复正常排空
    if (!stopDraining)
        return;
    stopDraining = false;
    log(LogLevel::Info, QStringLiteral("Resumed draining the TCP receive buffer"), QString(),
        QStringLiteral("rtsp.talkback_stop_draining"));
    onReadyRead();                            // 先把积压的吃掉，之后交回读通知
}

void RtspSession::Private::onReadyRead()
{
    // E19：不再排空接收缓冲。Qt 的读缓冲已经被压到很小，于是内核缓冲很快填满、
    // TCP 窗口关到 0，客户端的 sendall 就真的卡死在那儿。
    if (stopDraining)
        return;
    inBuffer += socket->readAll();
    processBuffer();
}

void RtspSession::Private::processBuffer()
{
    while (!inBuffer.isEmpty()) {
        if (inBuffer.at(0) == rtp::kInterleavedMagic) {
            int channel = 0;
            QByteArray payload;
            if (!rtp::takeInterleavedFrame(inBuffer, &channel, &payload))
                return;                       // 帧还没收全
            handleInterleaved(channel, payload);
            continue;
        }

        RtspRequest request;
        bool malformed = false;
        const qsizetype consumed = rtspInternal::parseRequest(inBuffer, &request, &malformed);
        if (consumed == 0) {
            if (inBuffer.size() > kMaxRequestBytes) {
                log(LogLevel::Warning, QStringLiteral("Request headers over %1 bytes, closing the connection")
                                           .arg(kMaxRequestBytes));
                q->teardown();
            }
            return;
        }
        inBuffer.remove(0, consumed);
        if (malformed) {
            send(RtspResponse::make(400, -1));
            q->teardown();
            return;
        }
        handleRequest(request);
        if (ended)
            return;
    }
}

void RtspSession::Private::handleInterleaved(int channel, const QByteArray &payload)
{
    q->touch();
    for (RtspTrack &track : tracks) {
        if (!track.transport.tcpInterleaved)
            continue;
        if (track.kind == RtspTrackKind::Backchannel) {
            if (channel == track.transport.interleavedRtp) {
                if (receiver)
                    receiver->feed(payload);
                emit q->backchannelDataReceived(payload.size());
                return;
            }
            if (channel == track.transport.interleavedRtcp)
                return;                       // 对讲侧的 RTCP，收下不处理
        } else if (track.sender && channel == track.transport.interleavedRtcp) {
            track.sender->feedRtcp(payload);
            return;
        }
    }
}

void RtspSession::Private::send(const RtspResponse &response)
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState)
        return;
    RtspResponse out = response;
    out.setHeader("Server", QByteArray("onvifsim/") + ONVIFSIM_VERSION);
    out.setHeader("Date", netutil::httpDate());
    socket->write(out.serialize());
}

// ---------------------------------------------------------------------------
// 鉴权与 Require 头
// ---------------------------------------------------------------------------

bool RtspSession::Private::authorize(const RtspRequest &request, RtspResponse *challenge)
{
    if (!camera || !auth || camera->model().users.isEmpty())
        return true;                          // 没配用户表就是开放相机
    // OPTIONS 不挑战：客户端拿它探活，真机也基本都放行。
    if (request.method == "OPTIONS")
        return true;

    const AuthResult result = auth->verify(request.header("authorization"), request.method,
                                           request.uri.toUtf8(), camera->model(), quirks());
    if (result.authenticated)
        return true;

    *challenge = RtspResponse::make(401, request.cseq());
    // E8 会返两条（Basic + Digest），addHeader 负责让它们序列化成两行。
    for (const QByteArray &value : auth->challenges(quirks(), result.stale))
        challenge->addHeader("WWW-Authenticate", value);

    // 客户端第一次总是裸发（它还不知道 nonce），这一次 401 是协议规定的握手，
    // 不是故障 —— 记成 WARN 会让日志看起来全是错误，实际紧跟着就是 200。
    // 带了 Authorization 却过不去（口令错、nonce 重放、E9 的严格参数）才值得报警。
    const bool handshake = request.header("authorization").isEmpty();
    log(handshake ? LogLevel::Debug : LogLevel::Warning,
        handshake ? QStringLiteral("%1 → 401 challenge (client has no nonce yet)")
                        .arg(QString::fromLatin1(request.method))
                  : QStringLiteral("%1 authentication failed: %2")
                        .arg(QString::fromLatin1(request.method), result.failureReason));
    return false;
}

bool RtspSession::Private::rejectUnsupportedRequire(const RtspRequest &request,
                                                    RtspResponse *response)
{
    const QByteArray require = request.header("require").trimmed();
    if (require.isEmpty())
        return false;

    const bool quirkOn = quirks().isEnabled(QuirkId::IgnoreBackchannelRequire);
    const QString mode = quirkOn
        ? quirks().choice(QuirkId::IgnoreBackchannelRequire, QStringLiteral("ignore"))
        : QString();
    // E15 的 ignore 档：相机压根不看 Require，照样回 200（TL-IPC 真机就是这样，
    // 按 RFC 2326 本该回 551）。
    if (mode == QLatin1String("ignore"))
        return false;

    const bool strict = mode == QLatin1String("strict_551");
    QByteArrayList unsupported;
    for (const QByteArray &tag : require.split(',')) {
        const QByteArray trimmed = tag.trimmed();
        if (trimmed.isEmpty())
            continue;
        const bool backchannel = trimmed.toLower() == kBackchannelTag;
        // strict 档下连 backchannel 都不认；否则只要相机声明有对讲就接受。
        if (backchannel && !strict && camera->model().hasAudioBackchannel)
            continue;
        unsupported.append(trimmed);
    }
    if (unsupported.isEmpty())
        return false;

    *response = RtspResponse::make(551, request.cseq());
    response->setHeader("Unsupported", unsupported.join(", "));
    log(LogLevel::Info,
        QStringLiteral("Unsupported option-tag in Require, answering 551: %1")
            .arg(QString::fromUtf8(unsupported.join(", "))),
        QString(),
        strict ? QStringLiteral("rtsp.ignore_backchannel_require") : QString());
    return true;
}

bool RtspSession::Private::wantsBackchannel(const RtspRequest &request) const
{
    if (!camera->model().hasAudioBackchannel)
        return false;
    if (quirks().isEnabled(QuirkId::IgnoreBackchannelRequire)
        && quirks().choice(QuirkId::IgnoreBackchannelRequire, QStringLiteral("ignore"))
            == QLatin1String("ignore")) {
        return true;                          // E15：不看 Require 照给 sendonly 轨
    }
    return request.wantsBackchannel();
}

// ---------------------------------------------------------------------------
// 方法分发
// ---------------------------------------------------------------------------

void RtspSession::Private::handleRequest(const RtspRequest &request)
{
    q->touch();
    if (!camera) {
        send(RtspResponse::make(500, request.cseq()));   // 没挂相机的服务器，理论上到不了
        return;
    }
    log(LogLevel::Debug,
        QStringLiteral("%1 %2").arg(QString::fromLatin1(request.method), request.uri),
        QString::fromUtf8(request.headers.value("user-agent")));

    if (!request.version.startsWith("RTSP/1")) {
        send(RtspResponse::make(505, request.cseq()));
        return;
    }

    RtspResponse challenge;
    if (!authorize(request, &challenge)) {
        send(challenge);
        return;
    }

    RtspResponse response;
    if (request.method == "OPTIONS")
        response = handleOptions(request);
    else if (request.method == "DESCRIBE")
        response = handleDescribe(request);
    else if (request.method == "SETUP")
        response = handleSetup(request);
    else if (request.method == "PLAY")
        response = handlePlay(request);
    else if (request.method == "PAUSE")
        response = handlePause(request);
    else if (request.method == "TEARDOWN")
        response = handleTeardown(request);
    else if (request.method == "GET_PARAMETER" || request.method == "SET_PARAMETER") {
        // 客户端拿 GET_PARAMETER 当 keepalive，上面的 touch() 已经把会话续上了。
        response = RtspResponse::make(200, request.cseq());
        if (established)
            response.setHeader("Session", sessionId);
    } else {
        response = RtspResponse::make(501, request.cseq());
    }

    send(response);

    // TEARDOWN 的 200 得先落到 socket 上，再收摊。
    if (request.method == "TEARDOWN" && response.status == 200)
        QTimer::singleShot(0, q, [this] { q->teardown(); });
}

RtspResponse RtspSession::Private::handleOptions(const RtspRequest &request)
{
    RtspResponse response = RtspResponse::make(200, request.cseq());
    response.setHeader("Public",
                       "OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, "
                       "GET_PARAMETER, SET_PARAMETER");
    if (established)
        response.setHeader("Session", sessionId);
    return response;
}

QString RtspSession::Private::absoluteBase(const RtspRequest &request) const
{
    if (request.uri.startsWith(QLatin1String("rtsp://"), Qt::CaseInsensitive))
        return request.uri;
    // 客户端只发了绝对路径时，用它连上来的那个地址回填 —— 独立 IP 模式下
    // 必须是相机自己的别名地址，不能是绑定用的 0.0.0.0。
    const QString host = socket ? socket->localAddress().toString() : QStringLiteral("0.0.0.0");
    const quint16 port = server ? server->serverPort() : quint16(554);
    return QStringLiteral("rtsp://%1:%2%3").arg(host).arg(port).arg(rtspInternal::requestTarget(request.uri));
}

void RtspSession::Private::ensureVideoSource(const MediaProfile &profile)
{
    // 黑屏 quirk 直接换样片：内容真的是全黑，客户端的黑屏检测才有得测。
    const QString asset = quirks().isEnabled(QuirkId::VideoBlack)
        ? QStringLiteral("black")
        : profile.mediaAsset;
    if (video && video->isLoaded() && video->assetName() == asset)
        return;
    delete video;
    video = new H264Source;
    QString error;
    if (!video->load(asset, &error)) {
        log(LogLevel::Error, QStringLiteral("Failed to load clip %1: %2").arg(asset, error));
        delete video;
        video = nullptr;
    }
}

void RtspSession::Private::ensureAudioSource(AudioCodec codec)
{
    if (codec == AudioCodec::None)
        return;
    if (audio && audio->codec() == codec)
        return;
    delete audio;
    audio = new AudioSource;
    QString error;
    if (!audio->configure(codec, ToneKind::Sine440, &error)) {
        log(LogLevel::Error, QStringLiteral("Failed to configure the audio source: %1").arg(error));
        delete audio;
        audio = nullptr;
    }
}

AudioCodec RtspSession::Private::backchannelCodecFor(const MediaProfile &profile) const
{
    // 真机上 backchannel SDP 往往只开一个 codec，而且多半就是 PCMU。
    // profile 声明的是 G.711 / G.722 家族时跟着它，AAC 之类一律退回 PCMU
    // （没有客户端会往 backchannel 推 AAC）。
    const AudioCodec declared = codec::audioFromName(profile.audioEncoder.encoding);
    switch (declared) {
    case AudioCodec::PCMA:
    case AudioCodec::G722:
        return declared;
    default:
        break;
    }
    return AudioCodec::PCMU;
}

bool RtspSession::Private::prepareProfile(const QString &target, bool backchannel,
                                          const RtspRequest &request)
{
    const QString token = server->profileTokenForPath(target);
    if (token.isEmpty())
        return false;
    const MediaProfile *profile = camera->model().profileByToken(token);
    if (!profile)
        return false;

    profileToken = token;
    backchannelRequested = backchannel;

    SdpOptions options;
    options.sessionName = camera->model().displayName;
    options.originAddress = socket ? socket->localAddress().toString() : QStringLiteral("0.0.0.0");
    options.controlBase = absoluteBase(request);

    options.hasVideo = true;
    options.videoCodec = VideoCodec::H264;
    options.videoPayloadType = 96;
    ensureVideoSource(*profile);
    if (video) {
        options.spropParameterSets = video->spropParameterSets();
        options.profileLevelId = video->profileLevelId();
    }

    options.audioCodec = codec::audioFromName(profile->audioEncoder.encoding);
    options.hasAudio = profile->hasAudio && options.audioCodec != AudioCodec::None;
    options.audioPayloadType = codec::audioPayloadType(options.audioCodec);
    if (options.hasAudio) {
        ensureAudioSource(options.audioCodec);
        if (audio && options.audioCodec == AudioCodec::AAC)
            options.aacConfig = audio->aacConfig();
    }

    options.hasBackchannel = backchannel;
    options.backchannelCodec = backchannelCodecFor(*profile);
    options.backchannelPayloadType = codec::audioPayloadType(options.backchannelCodec);
    // 大华风格的双轨布局：预设里带，或者单独开 E6。
    options.dualTrackLayout = camera->persona().talkbackDualTrack
        || quirks().isEnabled(QuirkId::TalkbackDualTrack);

    sdpOptions = options;
    layout = sdp::trackLayout(options);
    return true;
}

RtspResponse RtspSession::Private::handleDescribe(const RtspRequest &request)
{
    RtspResponse rejected;
    if (rejectUnsupportedRequire(request, &rejected))
        return rejected;

    const bool backchannel = wantsBackchannel(request);

    // E7 忙槽位：上一条会话刚 TEARDOWN，槽位还没释放。真机（海康球机）在这几秒里
    // 对新的 backchannel DESCRIBE 回 401，客户端必须重试而不是判定凭据错了。
    if (backchannel && server->backchannelBusy()) {
        RtspResponse response = RtspResponse::make(401, request.cseq());
        if (auth) {
            for (const QByteArray &value : auth->challenges(quirks(), false))
                response.addHeader("WWW-Authenticate", value);
        }
        log(LogLevel::Warning, QStringLiteral("Talkback slot busy, DESCRIBE → 401"), QString(),
            QStringLiteral("rtsp.talkback_busy_slot"));
        return response;
    }

    const QString target = rtspInternal::requestTarget(request.uri);
    if (!prepareProfile(target, backchannel, request)) {
        log(LogLevel::Warning, QStringLiteral("No profile matches path %1").arg(target));
        return RtspResponse::make(404, request.cseq());
    }

    const QByteArray body = sdp::generate(sdpOptions, quirks());
    RtspResponse response = RtspResponse::make(200, request.cseq());
    // Content-Base 带尾斜杠：客户端把媒体级的 a=control:trackID=N 直接拼在后面。
    QString base = sdpOptions.controlBase;
    if (!base.endsWith(QLatin1Char('/')))
        base += QLatin1Char('/');
    response.setHeader("Content-Base", base.toUtf8());
    response.setHeader("Cache-Control", "no-cache");
    response.setBody(body, "application/sdp");
    log(LogLevel::Info,
        QStringLiteral("DESCRIBE %1 → %2 track(s)%3")
            .arg(profileToken)
            .arg(layout.size())
            .arg(backchannel ? QStringLiteral(" (including the sendonly talkback track)") : QString()),
        QString::fromUtf8(body));
    return response;
}

bool RtspSession::Private::maxSessionsReached() const
{
    if (!quirks().isEnabled(QuirkId::RtspMaxSessions))
        return false;
    const int max = quirks().paramInt(QuirkId::RtspMaxSessions, QStringLiteral("max"));
    int active = 0;
    // 只数「已经 SETUP 过」的会话：刚连上还没握手的连接不占码流槽位。
    for (RtspSession *session : server->sessions()) {
        if (session != q && !session->tracks().isEmpty())
            ++active;
    }
    return active >= max;
}

RtspTrack *RtspSession::Private::trackByKind(RtspTrackKind kind)
{
    for (RtspTrack &track : tracks) {
        if (track.kind == kind)
            return &track;
    }
    return nullptr;
}

bool RtspSession::Private::hasBackchannelTrack() const
{
    for (const RtspTrack &track : tracks) {
        if (track.kind == RtspTrackKind::Backchannel)
            return true;
    }
    return false;
}

RtspResponse RtspSession::Private::handleSetup(const RtspRequest &request)
{
    RtspResponse rejected;
    if (rejectUnsupportedRequire(request, &rejected))
        return rejected;

    const QString target = rtspInternal::requestTarget(request.uri);
    // 有的客户端不 DESCRIBE 直接 SETUP，这时按路径把布局补算出来。
    if (layout.isEmpty() && !prepareProfile(target, wantsBackchannel(request), request))
        return RtspResponse::make(404, request.cseq());

    if (!established && maxSessionsReached()) {
        log(LogLevel::Warning, QStringLiteral("Concurrent session limit reached, SETUP → 453"), QString(),
            QStringLiteral("rtsp.max_sessions"));
        return RtspResponse::make(453, request.cseq());
    }

    int index = rtspInternal::trackIndexOf(target);
    if (index < 0 || index >= layout.size()) {
        // E12 档下 SDP 里没有媒体级 control，客户端只能拿 session 级的去 SETUP。
        // 对讲优先：客户端开 backchannel 时要的就是那条轨。
        index = -1;
        // 先在还没 SETUP 的轨里挑对讲轨（客户端开 backchannel 时要的就是它），
        // 挑不到再退回第一条空闲轨。
        for (int pass = 0; pass < 2 && index < 0; ++pass) {
            const bool backchannelOnly = pass == 0 && backchannelRequested;
            for (int i = 0; i < layout.size(); ++i) {
                const RtspTrackKind candidate = kindOf(layout.at(i));
                if (backchannelOnly && candidate != RtspTrackKind::Backchannel)
                    continue;
                if (!trackByKind(candidate)) {
                    index = i;
                    break;
                }
            }
            if (!backchannelOnly)
                break;
        }
        if (index < 0)
            return RtspResponse::make(455, request.cseq());
    }

    const RtspTrackKind kind = kindOf(layout.at(index));
    if (trackByKind(kind))
        return RtspResponse::make(455, request.cseq());   // 同一条轨 SETUP 两次

    bool parsed = false;
    RtspTransport transport = RtspTransport::parse(request.header("transport"), &parsed);
    if (!parsed) {
        log(LogLevel::Warning, QStringLiteral("Unsupported Transport: %1")
                                   .arg(QString::fromUtf8(request.header("transport"))));
        return RtspResponse::make(461, request.cseq());
    }

    RtspTrack track;
    track.kind = kind;
    track.index = index;

    if (transport.tcpInterleaved) {
        // 客户端没自报 interleaved 通道时按轨序分配（0-1、2-3 …）。
        if (!request.header("transport").toLower().contains("interleaved")) {
            transport.interleavedRtp = 2 * int(tracks.size());
            transport.interleavedRtcp = transport.interleavedRtp + 1;
        }
    } else if (transport.clientRtpPort == 0) {
        return RtspResponse::make(461, request.cseq());   // UDP 却没给 client_port
    }

    if (kind == RtspTrackKind::Backchannel) {
        if (!receiver) {
            receiver = new RtpReceiver(q);
            receiver->setCodec(sdpOptions.backchannelCodec);
            receiver->setQuirks(&quirks());
        }
        if (!transport.tcpInterleaved) {
            // UDP 对讲很罕见，但规范允许。RtpReceiver 不持 socket，由会话自己收。
            delete backchannelUdp;
            backchannelUdp = new QUdpSocket(q);
            if (!backchannelUdp->bind(camera->model().bindAddress, 0)) {
                delete backchannelUdp;
                backchannelUdp = nullptr;
                return RtspResponse::make(461, request.cseq());
            }
            QObject::connect(backchannelUdp, &QUdpSocket::readyRead, q, [this] {
                while (backchannelUdp->hasPendingDatagrams()) {
                    QByteArray datagram;
                    datagram.resize(int(backchannelUdp->pendingDatagramSize()));
                    backchannelUdp->readDatagram(datagram.data(), datagram.size());
                    if (receiver)
                        receiver->feed(datagram);
                    emit q->backchannelDataReceived(datagram.size());
                    q->touch();
                }
            });
            transport.serverRtpPort = backchannelUdp->localPort();
            transport.serverRtcpPort = quint16(transport.serverRtpPort + 1);
        }
    } else {
        RtpSender *sender = new RtpSender(q);
        sender->setQuirks(&quirks());
        if (kind == RtspTrackKind::Video) {
            sender->setPayloadType(sdpOptions.videoPayloadType);
            sender->setClockRate(90000);
        } else {
            sender->setPayloadType(
                sdp::effectiveAudioPayloadType(sdpOptions.audioPayloadType, quirks()));
            // 必须和 SDP 里 a=rtpmap 声明的那个数一致，否则客户端按声明的
            // 时钟率还原时间戳，播放速度就错了（E10 开着时尤其明显）。
            sender->setClockRate(
                sdp::effectiveAudioClockRate(sdpOptions.audioCodec, quirks()));
        }

        if (transport.tcpInterleaved) {
            sender->setInterleaved(socket, transport.interleavedRtp, transport.interleavedRtcp);
        } else {
            QString error;
            if (!sender->setUdpDestination(socket->peerAddress(), transport.clientRtpPort,
                                           transport.clientRtcpPort,
                                           camera->model().bindAddress, &error)) {
                delete sender;
                log(LogLevel::Error, QStringLiteral("Cannot allocate RTP ports: %1").arg(error));
                return RtspResponse::make(500, request.cseq());
            }
            transport.serverRtpPort = sender->localRtpPort();
            transport.serverRtcpPort = sender->localRtcpPort();
            transport.destination = socket->peerAddress().toString();
        }
        track.sender = sender;
    }

    track.transport = transport;
    tracks.append(track);
    established = true;

    RtspResponse response = RtspResponse::make(200, request.cseq());
    response.setHeader("Transport", transport.toHeader());
    // E14：违反 RFC 2326 不回 Session 头，客户端后续 PLAY 就没有 Session 可用。
    if (quirks().isEnabled(QuirkId::SetupNoSessionHeader)) {
        log(LogLevel::Warning, QStringLiteral("SETUP reply deliberately omits the Session header"), QString(),
            QStringLiteral("rtsp.setup_no_session_header"));
    } else {
        response.setHeader("Session",
                           sessionId + ";timeout=" + QByteArray::number(server->sessionTimeout()));
    }
    log(LogLevel::Info, QStringLiteral("SETUP track %1 (%2)%3")
                            .arg(index)
                            .arg(kind == RtspTrackKind::Backchannel
                                     ? QStringLiteral("talkback")
                                     : kind == RtspTrackKind::Audio ? QStringLiteral("audio")
                                                                    : QStringLiteral("video"))
                            .arg(QString::fromUtf8(transport.toHeader())));
    return response;
}

RtspResponse RtspSession::Private::handlePlay(const RtspRequest &request)
{
    RtspResponse rejected;
    if (rejectUnsupportedRequire(request, &rejected))
        return rejected;

    if (!established)
        return RtspResponse::make(455, request.cseq());
    const QByteArray requested = request.sessionId();
    if (!requested.isEmpty() && !sessionId.isEmpty() && requested != sessionId)
        return RtspResponse::make(454, request.cseq());

    QByteArrayList rtpInfo;
    QString base = sdpOptions.controlBase;
    if (!base.endsWith(QLatin1Char('/')))
        base += QLatin1Char('/');

    for (RtspTrack &track : tracks) {
        track.playing = true;
        if (!track.sender)
            continue;
        track.sender->start();
        rtpInfo.append("url=" + (base + QStringLiteral("trackID=%1").arg(track.index)).toUtf8()
                       + ";seq=" + QByteArray::number(track.sender->stats().lastSequence)
                       + ";rtptime=0");
    }

    startPump();
    if (!playing) {
        playing = true;
        emit q->playingChanged(true);
    }

    // E19：推流开始 N 毫秒后停止排空 TCP 接收缓冲。
    if (hasBackchannelTrack() && quirks().isEnabled(QuirkId::TalkbackStopDraining)) {
        const int afterMs = quirks().paramInt(QuirkId::TalkbackStopDraining,
                                              QStringLiteral("after_ms"));
        // 先把 Qt 的读缓冲压小：不这么做的话 Qt 会一直替我们把内核缓冲搬空，
        // 客户端永远不会被阻塞，quirk 就没有效果。
        socket->setReadBufferSize(kStopDrainingBufferBytes);
        stopDrainTimer->start(qMax(0, afterMs));
    }

    RtspResponse response = RtspResponse::make(200, request.cseq());
    response.setHeader("Session", sessionId);
    response.setHeader("Range", "npt=now-");
    if (!rtpInfo.isEmpty())
        response.setHeader("RTP-Info", rtpInfo.join(","));
    log(LogLevel::Info, QStringLiteral("PLAY %1, streaming %2 track(s)")
                            .arg(profileToken)
                            .arg(tracks.size()));
    return response;
}

RtspResponse RtspSession::Private::handlePause(const RtspRequest &request)
{
    stopPump();
    for (RtspTrack &track : tracks) {
        track.playing = false;
        if (track.sender)
            track.sender->stop();
    }
    if (playing) {
        playing = false;
        emit q->playingChanged(false);
    }
    RtspResponse response = RtspResponse::make(200, request.cseq());
    response.setHeader("Session", sessionId);
    return response;
}

RtspResponse RtspSession::Private::handleTeardown(const RtspRequest &request)
{
    RtspResponse response = RtspResponse::make(200, request.cseq());
    response.setHeader("Session", sessionId);
    response.setHeader("Connection", "close");
    return response;
}

// ---------------------------------------------------------------------------
// 发送泵：按 elapsed 累计补发，抖动不累积
// ---------------------------------------------------------------------------

double RtspSession::Private::effectiveFps() const
{
    if (quirks().isEnabled(QuirkId::VideoFpsChange))
        return quirks().paramDouble(QuirkId::VideoFpsChange, QStringLiteral("fps"));
    if (video && video->frameRate() > 0.0)
        return video->frameRate();
    return 15.0;
}

void RtspSession::Private::startPump()
{
    if (pump->isActive())
        return;
    // 重新计时，但计数器不清零：elapsed 只用来算「从这次 PLAY 起又该发多少」。
    videoFrameBase = videoFrames;
    audioPacketBase = audioPackets;
    clock.start();
    pump->start();
}

void RtspSession::Private::stopPump()
{
    pump->stop();
}

void RtspSession::Private::onPumpTick()
{
    const qint64 elapsedUs = clock.nsecsElapsed() / 1000;
    pumpVideo(elapsedUs);
    pumpAudio(elapsedUs);
}

void RtspSession::Private::pumpVideo(qint64 elapsedUs)
{
    RtspTrack *track = trackByKind(RtspTrackKind::Video);
    if (!track || !track->playing || !track->sender || !video || !video->isLoaded())
        return;

    const double fps = effectiveFps();
    if (fps <= 0.0)
        return;
    const int frameCount = qMax(1, video->frameCount());

    // 「按真实经过时间本该发到第几帧」，一次补齐。这样即便定时器被别的槽拖慢，
    // 误差也不会一路累加下去。
    const qint64 target = videoFrameBase + qint64(double(elapsedUs) * fps / 1000000.0) + 1;
    if (target - videoFrames > kMaxCatchupFrames)
        videoFrames = target - 1;

    while (videoFrames < target) {
        int index = int(videoFrames % frameCount);
        // 冻结：画面内容锁死在某一帧，但时间戳照常往前走 —— 客户端看到的就是
        // 「有码流、有帧率、画面不动」，正是冻结检测要测的场景。
        if (quirks().isEnabled(QuirkId::VideoFreeze)) {
            if (freezeFrame < 0)
                freezeFrame = index;
            index = freezeFrame;
        }

        const EncodedFrame frame = video->frame(index);
        const quint32 timestamp = quint32(qint64(double(videoFrames) * 90000.0 / fps));

        const QString placement = quirks().isEnabled(QuirkId::SpsPpsPlacement)
            ? quirks().choice(QuirkId::SpsPpsPlacement, QStringLiteral("both"))
            : QStringLiteral("both");

        QVector<QByteArray> packets;
        const QVector<QByteArray> nals = H264Source::splitNalUnits(frame.data);
        for (const QByteArray &nal : nals) {
            if (nal.isEmpty())
                continue;
            const int nalType = int(uchar(nal.at(0))) & 0x1f;
            // sdp_only：带内不再重复 SPS(7)/PPS(8)，只认带内参数集的解码器就会黑屏。
            if (placement == QLatin1String("sdp_only") && (nalType == 7 || nalType == 8))
                continue;
            packets += H264Source::packetize(nal, kRtpMtu);
        }
        // marker 只置在一帧的最后一个包上：客户端靠它判定「这一帧收全了」。
        for (int i = 0; i < packets.size(); ++i)
            track->sender->sendPacket(packets.at(i), timestamp, i == packets.size() - 1);

        ++videoFrames;
    }
}

void RtspSession::Private::pumpAudio(qint64 elapsedUs)
{
    RtspTrack *track = trackByKind(RtspTrackKind::Audio);
    if (!track || !track->playing || !track->sender || !audio)
        return;
    const qint64 packetUs = audio->packetDurationUs();
    if (packetUs <= 0)
        return;

    const qint64 target = audioPacketBase + elapsedUs / packetUs + 1;
    if (target - audioPackets > kMaxCatchupAudioPackets)
        audioPackets = target - 1;

    const int samples = audio->samplesPerPacket();
    while (audioPackets < target) {
        QByteArray payload = audio->packet(int(audioPackets & 0x3fffffff));
        if (audio->codec() == AudioCodec::AAC)
            payload.prepend(AudioSource::aacAuHeader(int(payload.size())));
        const quint32 timestamp = quint32(audioPackets * samples);
        track->sender->sendPacket(payload, timestamp, audioPackets == 0);
        ++audioPackets;
    }
}

void RtspSession::Private::finish()
{
    if (ended)
        return;
    ended = true;
    stopPump();
    for (RtspTrack &track : tracks) {
        track.playing = false;
        if (track.sender)
            track.sender->stop();
    }
    if (playing) {
        playing = false;
        emit q->playingChanged(false);
    }
    // E7：这条会话占过对讲槽位，释放要等几秒 —— 期间新的 DESCRIBE 回 401。
    if (hasBackchannelTrack() && server)
        server->markBackchannelBusy();
    if (socket && socket->state() != QAbstractSocket::UnconnectedState)
        socket->disconnectFromHost();
    emit q->endedByPeer();
}

// ---------------------------------------------------------------------------
// 公共接口
// ---------------------------------------------------------------------------

RtspSession::RtspSession(RtspServer *server, QTcpSocket *socket, QObject *parent)
    : QObject(parent), d(new Private)
{
    d->q = this;
    d->server = server;
    d->socket = socket;
    d->camera = server ? server->camera() : nullptr;
    d->auth = server ? server->auth() : nullptr;
    // 会话 id 连上来就定：sessionStarted 信号、teardownSession(id)、REST 的会话列表
    // 都要用它，等到 SETUP 才生成的话前面那一段全是空 id。真机用的也是纯数字。
    d->sessionId = QByteArray::number(QRandomGenerator::global()->bounded(10000000, 99999999));
    d->startedAt = QDateTime::currentDateTimeUtc();
    d->lastActivity = d->startedAt;

    if (socket) {
        socket->setParent(this);
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);   // 控制交互别被 Nagle 拖住
        connect(socket, &QTcpSocket::readyRead, this, [this] { d->onReadyRead(); });
        connect(socket, &QTcpSocket::disconnected, this, [this] { d->finish(); });
    }

    d->pump = new QTimer(this);
    // PreciseTimer + 「按 elapsed 补发」是计划里定死的组合：单个定时器管所有轨，
    // 不是「每帧一个定时器」，抖动才不会累积。
    d->pump->setTimerType(Qt::PreciseTimer);
    d->pump->setInterval(kPumpIntervalMs);
    connect(d->pump, &QTimer::timeout, this, [this] { d->onPumpTick(); });

    d->stopDrainTimer = new QTimer(this);
    d->stopDrainTimer->setSingleShot(true);
    connect(d->stopDrainTimer, &QTimer::timeout, this, [this] {
        d->stopDraining = true;
        d->log(LogLevel::Warning, QStringLiteral("Stopped draining the TCP receive buffer; talkback senders will block"),
               QString(), QStringLiteral("rtsp.talkback_stop_draining"));
    });

    // quirk 改动是热生效的：E19 被关掉时立刻恢复排空（见 resumeDraining）。
    if (d->camera) {
        connect(d->camera, &VirtualCamera::quirksChanged, this, [this] {
            if (!d->quirks().isEnabled(QuirkId::TalkbackStopDraining))
                d->resumeDraining();
        });
    }
}

RtspSession::~RtspSession()
{
    d->stopPump();
    delete d->video;
    delete d->audio;
    delete d;
}

QByteArray RtspSession::sessionId() const
{
    return d->sessionId;
}

QString RtspSession::profileToken() const
{
    return d->profileToken;
}

QString RtspSession::peerString() const
{
    if (!d->socket)
        return QString();
    return d->socket->peerAddress().toString() + QLatin1Char(':')
        + QString::number(d->socket->peerPort());
}

QDateTime RtspSession::startedAt() const
{
    return d->startedAt;
}

QDateTime RtspSession::lastActivity() const
{
    return d->lastActivity;
}

bool RtspSession::isPlaying() const
{
    return d->playing;
}

bool RtspSession::hasBackchannel() const
{
    return d->hasBackchannelTrack();
}

QVector<RtspTrack> RtspSession::tracks() const
{
    return d->tracks;
}

RtpReceiver *RtspSession::backchannelReceiver() const
{
    return d->receiver;
}

void RtspSession::teardown()
{
    d->finish();
}

void RtspSession::touch()
{
    d->lastActivity = QDateTime::currentDateTimeUtc();
}


} // namespace onvifsim
