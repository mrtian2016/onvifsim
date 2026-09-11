// rtsp 模块的纯逻辑单测：报文解析、Transport 头、SDP 各变体、RTP 包头与 interleaved 封装。
// 一律不碰 socket、不建 VirtualCamera —— 那些路径归 e2e 测。

#include "core/Quirks.h"
#include "media/MediaTypes.h"
#include "rtsp/RtpPacket_p.h"
#include "rtsp/RtpReceiver.h"
#include "rtsp/RtspInternal_p.h"
#include "rtsp/RtspTypes.h"
#include "rtsp/Sdp.h"

#include <QtTest/QtTest>

using namespace onvifsim;

namespace {

const char kBackchannel[] = "www.onvif.org/ver20/backchannel";

SdpOptions baseOptions()
{
    SdpOptions options;
    options.sessionName = QStringLiteral("cam1");
    options.originAddress = QStringLiteral("192.168.1.50");
    options.controlBase = QStringLiteral("rtsp://192.168.1.50:8554/Streaming/Channels/101");
    options.hasVideo = true;
    options.videoPayloadType = 96;
    options.spropParameterSets = "Z0LAH9oBQBboQAAAAwBAAAAMoeMGSA==,aMuMsg==";
    options.profileLevelId = "42c01f";
    options.hasAudio = true;
    options.audioCodec = AudioCodec::PCMU;
    options.audioPayloadType = 0;
    return options;
}

// 找出 SDP 里第 index 条 m= 段的全文（含它下面的属性行）。
QByteArray mediaSection(const QByteArray &sdp, int index)
{
    const QList<QByteArray> lines = sdp.split('\n');
    QByteArray section;
    int seen = -1;
    for (const QByteArray &raw : lines) {
        QByteArray line = raw;
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.startsWith("m=")) {
            ++seen;
            if (seen > index)
                break;
        }
        if (seen == index)
            section += line + "\n";
    }
    return section;
}

QByteArray rtpPacket(int payloadType, bool marker, quint16 sequence, quint32 timestamp,
                     quint32 ssrc, const QByteArray &payload)
{
    return rtp::buildPacket(payloadType, marker, sequence, timestamp, ssrc, payload);
}

} // namespace

class TestRtsp : public QObject
{
    Q_OBJECT

private slots:
    // ---- 请求解析 ----
    void parseRequestLine();
    void parseIncompleteRequest();
    void parseRequestWithBody();
    void parseFoldedAndRepeatedHeaders();
    void parseLfOnlyRequest();
    void parseMalformedRequestLine();
    void cseqAndSessionId();
    void backchannelRequireDetection();

    // ---- 响应 ----
    void responseSerialization();
    void responseRepeatedHeaders();
    void responseReasons();

    // ---- Transport ----
    void transportParseUdp();
    void transportParseInterleaved();
    void transportParseAlternatives();
    void transportParseUnsupported();
    void transportToHeader();

    // ---- URL 归一 ----
    void requestTargetForms();
    void trackIndexParsing();
    void normalizeDahuaPath();

    // ---- SDP ----
    void sdpBaseline();
    void sdpG722ClockRate();
    void sdpNoRtpmap();
    void sdpSessionLevelControl();
    void sdpBackchannelSingleTrack();
    void sdpBackchannelDualTrack();
    void sdpNoBackchannelWithoutRequire();
    void sdpNonstandardCodec();
    void sdpAacFmtp();
    void sdpSpsPpsInbandOnly();

    // ---- RTP 包头 ----
    void rtpHeaderRoundTrip();
    void rtpHeaderRejectsBadVersion();
    void rtpHeaderSkipsCsrcAndExtension();
    void rtpHeaderStripsPadding();

    // ---- interleaved 封装 ----
    void interleavedFraming();
    void interleavedPartialFrame();

    // ---- 对讲接收 ----
    void receiverCountsLossAndMarker();
    void receiverRequiresMarker();
};

// ---------------------------------------------------------------------------
// 请求解析
// ---------------------------------------------------------------------------

void TestRtsp::parseRequestLine()
{
    const QByteArray raw =
        "DESCRIBE rtsp://192.168.1.50:554/Streaming/Channels/101 RTSP/1.0\r\n"
        "CSeq: 3\r\n"
        "Accept: application/sdp\r\n"
        "User-Agent: onvifsim-test\r\n"
        "\r\n";

    RtspRequest request;
    bool malformed = true;
    const qsizetype consumed = rtspInternal::parseRequest(raw, &request, &malformed);

    QCOMPARE(consumed, raw.size());
    QVERIFY(!malformed);
    QCOMPARE(request.method, QByteArray("DESCRIBE"));
    QCOMPARE(request.uri, QStringLiteral("rtsp://192.168.1.50:554/Streaming/Channels/101"));
    QCOMPARE(request.version, QByteArray("RTSP/1.0"));
    QCOMPARE(request.cseq(), 3);
    // 头名大小写不敏感，存的是小写 key。
    QCOMPARE(request.header("Accept"), QByteArray("application/sdp"));
    QCOMPARE(request.header("USER-AGENT"), QByteArray("onvifsim-test"));
    QVERIFY(request.hasHeader("cseq"));
    QVERIFY(!request.hasHeader("session"));
}

void TestRtsp::parseIncompleteRequest()
{
    const QByteArray partial = "OPTIONS rtsp://cam/ RTSP/1.0\r\nCSeq: 1\r\n";
    QCOMPARE(rtspInternal::parseRequest(partial, nullptr, nullptr), qsizetype(0));
}

void TestRtsp::parseRequestWithBody()
{
    const QByteArray head =
        "SET_PARAMETER rtsp://cam/ RTSP/1.0\r\n"
        "CSeq: 7\r\n"
        "Content-Length: 11\r\n"
        "\r\n";
    const QByteArray raw = head + "hello world" + "OPTIONS rtsp://cam/ RTSP/1.0\r\n";

    RtspRequest request;
    const qsizetype consumed = rtspInternal::parseRequest(raw, &request, nullptr);
    QCOMPARE(consumed, head.size() + 11);
    QCOMPARE(request.body, QByteArray("hello world"));

    // 体还没收全时不能吃掉缓冲。
    QCOMPARE(rtspInternal::parseRequest(head + "hello", nullptr, nullptr), qsizetype(0));
}

void TestRtsp::parseFoldedAndRepeatedHeaders()
{
    const QByteArray raw =
        "DESCRIBE rtsp://cam/ RTSP/1.0\r\n"
        "CSeq: 2\r\n"
        "Require: www.onvif.org/ver20/backchannel\r\n"
        "Require: some.other/tag\r\n"
        "X-Long: first\r\n"
        "  second\r\n"
        "\r\n";

    RtspRequest request;
    QVERIFY(rtspInternal::parseRequest(raw, &request, nullptr) > 0);
    // 同名头并成逗号列表，否则 wantsBackchannel 会漏看。
    QCOMPARE(request.header("require"),
             QByteArray("www.onvif.org/ver20/backchannel, some.other/tag"));
    QCOMPARE(request.header("x-long"), QByteArray("first second"));
    QVERIFY(request.wantsBackchannel());
}

void TestRtsp::parseLfOnlyRequest()
{
    const QByteArray raw = "OPTIONS rtsp://cam/ RTSP/1.0\nCSeq: 5\n\n";
    RtspRequest request;
    QCOMPARE(rtspInternal::parseRequest(raw, &request, nullptr), raw.size());
    QCOMPARE(request.cseq(), 5);
}

void TestRtsp::parseMalformedRequestLine()
{
    const QByteArray raw = "GARBAGE\r\n\r\n";
    bool malformed = false;
    QVERIFY(rtspInternal::parseRequest(raw, nullptr, &malformed) > 0);
    QVERIFY(malformed);
}

void TestRtsp::cseqAndSessionId()
{
    RtspRequest request;
    QCOMPARE(request.cseq(), -1);              // 没写 CSeq 与写了 0 要能区分
    request.headers.insert("cseq", "0");
    QCOMPARE(request.cseq(), 0);

    request.headers.insert("session", "12345678;timeout=60");
    QCOMPARE(request.sessionId(), QByteArray("12345678"));
    request.headers.insert("session", " 87654321 ");
    QCOMPARE(request.sessionId(), QByteArray("87654321"));
}

void TestRtsp::backchannelRequireDetection()
{
    RtspRequest request;
    QVERIFY(!request.wantsBackchannel());
    request.headers.insert("require", kBackchannel);
    QVERIFY(request.wantsBackchannel());
    // option-tag 的大小写在真机上什么写法都有。
    request.headers.insert("require", "WWW.ONVIF.ORG/VER20/BACKCHANNEL");
    QVERIFY(request.wantsBackchannel());
    request.headers.insert("require", "implicit-play");
    QVERIFY(!request.wantsBackchannel());
}

// ---------------------------------------------------------------------------
// 响应
// ---------------------------------------------------------------------------

void TestRtsp::responseSerialization()
{
    RtspResponse response = RtspResponse::make(200, 4);
    response.setHeader("Transport", "RTP/AVP/TCP;unicast;interleaved=0-1");
    response.setBody("v=0\r\n", "application/sdp");

    const QByteArray raw = response.serialize();
    QVERIFY(raw.startsWith("RTSP/1.0 200 OK\r\n"));
    QVERIFY(raw.contains("\r\nCSeq: 4\r\n"));
    QVERIFY(raw.contains("\r\nContent-Type: application/sdp\r\n"));
    QVERIFY(raw.contains("\r\nContent-Length: 5\r\n"));   // 有体就必须自动补上
    QVERIFY(raw.endsWith("\r\n\r\nv=0\r\n"));

    // 无体的响应不硬塞 Content-Length。
    const QByteArray empty = RtspResponse::make(200, 1).serialize();
    QVERIFY(!empty.contains("Content-Length"));

    // setHeader 覆盖时不区分大小写，不能出现两条同名头。
    RtspResponse dup = RtspResponse::make(200, 1);
    dup.setHeader("Session", "1111");
    dup.setHeader("session", "2222");
    QCOMPARE(dup.serialize().count("2222"), 1);
    QVERIFY(!dup.serialize().contains("1111"));
}

void TestRtsp::responseRepeatedHeaders()
{
    // E8：同时发 Basic + Digest 两条 WWW-Authenticate。headers 是 QMap，
    // 多个值用 '\n' 串在一个 key 上，serialize() 拆成两行（与 HttpResponse 同一套约定）。
    RtspResponse response = RtspResponse::make(401, 1);
    response.addHeader("WWW-Authenticate", "Digest realm=\"onvifsim\", nonce=\"abc\"");
    response.addHeader("WWW-Authenticate", "Basic realm=\"onvifsim\"");

    const QByteArray raw = response.serialize();
    QCOMPARE(raw.count("WWW-Authenticate: "), 2);
    QVERIFY(raw.contains("\r\nWWW-Authenticate: Digest realm=\"onvifsim\", nonce=\"abc\"\r\n"));
    QVERIFY(raw.contains("\r\nWWW-Authenticate: Basic realm=\"onvifsim\"\r\n"));
    QVERIFY(!raw.contains("\n\n"));                  // 拆行别拆出空行来

    // addHeader 之后 setHeader 仍然是整体覆盖。
    response.setHeader("www-authenticate", "Digest realm=\"x\"");
    QCOMPARE(response.serialize().count("WWW-Authenticate: "), 1);

    // 第一次 addHeader 等价于 setHeader。
    RtspResponse single = RtspResponse::make(401, 1);
    single.addHeader("WWW-Authenticate", "Digest realm=\"onvifsim\"");
    QCOMPARE(single.serialize().count("WWW-Authenticate: "), 1);
}

void TestRtsp::responseReasons()
{
    // RTSP 特有的状态码，客户端的诊断分支全靠它们。
    QCOMPARE(QByteArray(RtspResponse::defaultReason(453)), QByteArray("Not Enough Bandwidth"));
    QCOMPARE(QByteArray(RtspResponse::defaultReason(454)), QByteArray("Session Not Found"));
    QCOMPARE(QByteArray(RtspResponse::defaultReason(461)), QByteArray("Unsupported Transport"));
    QCOMPARE(QByteArray(RtspResponse::defaultReason(551)), QByteArray("Option Not Supported"));
    QCOMPARE(QByteArray(RtspResponse::defaultReason(401)), QByteArray("Unauthorized"));
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

void TestRtsp::transportParseUdp()
{
    bool ok = false;
    const RtspTransport transport =
        RtspTransport::parse("RTP/AVP;unicast;client_port=8000-8001", &ok);
    QVERIFY(ok);
    QVERIFY(!transport.tcpInterleaved);
    QVERIFY(!transport.multicast);
    QCOMPARE(transport.clientRtpPort, quint16(8000));
    QCOMPARE(transport.clientRtcpPort, quint16(8001));

    // 只给一个端口时按惯例补 +1。
    bool single = false;
    const RtspTransport one = RtspTransport::parse("RTP/AVP/UDP;unicast;client_port=9000", &single);
    QVERIFY(single);
    QCOMPARE(one.clientRtpPort, quint16(9000));
    QCOMPARE(one.clientRtcpPort, quint16(9001));
}

void TestRtsp::transportParseInterleaved()
{
    // 参照客户端的对讲 SETUP 就是这一条。
    bool ok = false;
    const RtspTransport transport =
        RtspTransport::parse("RTP/AVP/TCP;unicast;interleaved=0-1", &ok);
    QVERIFY(ok);
    QVERIFY(transport.tcpInterleaved);
    QCOMPARE(transport.interleavedRtp, 0);
    QCOMPARE(transport.interleavedRtcp, 1);

    const RtspTransport second = RtspTransport::parse("RTP/AVP/TCP;unicast;interleaved=2-3", &ok);
    QVERIFY(ok);
    QCOMPARE(second.interleavedRtp, 2);
    QCOMPARE(second.interleavedRtcp, 3);
}

void TestRtsp::transportParseAlternatives()
{
    // RFC 2326 允许一次列多个候选，服务器挑第一条支持的。
    bool ok = false;
    const RtspTransport transport = RtspTransport::parse(
        "RTP/AVP/TCP;unicast;interleaved=0-1,RTP/AVP;unicast;client_port=6000-6001", &ok);
    QVERIFY(ok);
    QVERIFY(transport.tcpInterleaved);
    QCOMPARE(transport.interleavedRtp, 0);
}

void TestRtsp::transportParseUnsupported()
{
    bool ok = true;
    RtspTransport::parse("RTP/SAVP;unicast;client_port=8000-8001", &ok);
    QVERIFY(!ok);
    ok = true;
    RtspTransport::parse("", &ok);
    QVERIFY(!ok);
}

void TestRtsp::transportToHeader()
{
    RtspTransport tcp;
    tcp.tcpInterleaved = true;
    tcp.interleavedRtp = 0;
    tcp.interleavedRtcp = 1;
    QCOMPARE(tcp.toHeader(), QByteArray("RTP/AVP/TCP;unicast;interleaved=0-1"));

    RtspTransport udp;
    udp.clientRtpPort = 8000;
    udp.clientRtcpPort = 8001;
    udp.serverRtpPort = 20000;
    udp.serverRtcpPort = 20001;
    QCOMPARE(udp.toHeader(),
             QByteArray("RTP/AVP;unicast;client_port=8000-8001;server_port=20000-20001"));

    // 回写出去的头要能被自己再解析回来。
    bool ok = false;
    const RtspTransport again = RtspTransport::parse(udp.toHeader(), &ok);
    QVERIFY(ok);
    QCOMPARE(again.clientRtpPort, quint16(8000));
    QCOMPARE(again.serverRtpPort, quint16(20000));
}

// ---------------------------------------------------------------------------
// URL 归一
// ---------------------------------------------------------------------------

void TestRtsp::requestTargetForms()
{
    QCOMPARE(rtspInternal::requestTarget(QStringLiteral("rtsp://1.2.3.4:554/Streaming/Channels/101")),
             QStringLiteral("/Streaming/Channels/101"));
    QCOMPARE(rtspInternal::requestTarget(QStringLiteral("/stream1")), QStringLiteral("/stream1"));
    QCOMPARE(rtspInternal::requestTarget(QStringLiteral("*")), QStringLiteral("/"));
    QCOMPARE(rtspInternal::requestTarget(QStringLiteral("rtsp://1.2.3.4")), QStringLiteral("/"));
    // 大华的 query 是路径的一部分，绝不能丢。
    QCOMPARE(rtspInternal::requestTarget(
                 QStringLiteral("rtsp://1.2.3.4/cam/realmonitor?channel=1&subtype=0")),
             QStringLiteral("/cam/realmonitor?channel=1&subtype=0"));
}

void TestRtsp::trackIndexParsing()
{
    QCOMPARE(rtspInternal::trackIndexOf(QStringLiteral("/Streaming/Channels/101/trackID=1")), 1);
    QCOMPARE(rtspInternal::trackIndexOf(QStringLiteral("/stream1/trackID=0")), 0);
    QCOMPARE(rtspInternal::trackIndexOf(QStringLiteral("/stream1/track=2")), 2);
    QCOMPARE(rtspInternal::trackIndexOf(QStringLiteral("/stream1")), -1);
    QCOMPARE(rtspInternal::trackIndexOf(
                 QStringLiteral("/cam/realmonitor?channel=1&subtype=0/trackID=3")), 3);
}

void TestRtsp::normalizeDahuaPath()
{
    QCOMPARE(rtspInternal::normalizeStreamPath(
                 QStringLiteral("/cam/realmonitor?channel=1&subtype=0/trackID=0")),
             QStringLiteral("/cam/realmonitor?channel=1&subtype=0"));
    QCOMPARE(rtspInternal::normalizeStreamPath(QStringLiteral("/Streaming/Channels/101/")),
             QStringLiteral("/Streaming/Channels/101"));
    QCOMPARE(rtspInternal::normalizeStreamPath(QStringLiteral("stream1")),
             QStringLiteral("/stream1"));
}

// ---------------------------------------------------------------------------
// SDP
// ---------------------------------------------------------------------------

void TestRtsp::sdpBaseline()
{
    Quirks quirks;
    const QByteArray sdp = sdp::generate(baseOptions(), quirks);

    QVERIFY(sdp.startsWith("v=0\r\n"));
    QVERIFY(sdp.contains("\r\ns=cam1\r\n"));
    QVERIFY(sdp.contains("IN IP4 192.168.1.50"));
    QVERIFY(sdp.contains("\r\nc=IN IP4 0.0.0.0\r\n"));
    QVERIFY(sdp.contains("\r\nt=0 0\r\n"));
    QVERIFY(sdp.contains("a=control:rtsp://192.168.1.50:8554/Streaming/Channels/101\r\n"));

    const QByteArray video = mediaSection(sdp, 0);
    QVERIFY(video.startsWith("m=video 0 RTP/AVP 96"));
    QVERIFY(video.contains("a=rtpmap:96 H264/90000"));
    QVERIFY(video.contains("packetization-mode=1"));
    QVERIFY(video.contains("profile-level-id=42c01f"));
    QVERIFY(video.contains("sprop-parameter-sets=Z0LAH9oBQBboQAAAAwBAAAAMoeMGSA==,aMuMsg=="));
    QVERIFY(video.contains("a=control:trackID=0"));
    QVERIFY(video.contains("a=recvonly"));

    const QByteArray audio = mediaSection(sdp, 1);
    QVERIFY(audio.startsWith("m=audio 0 RTP/AVP 0"));
    QVERIFY(audio.contains("a=rtpmap:0 PCMU/8000/1"));
    QVERIFY(audio.contains("a=control:trackID=1"));
    QVERIFY(audio.contains("a=recvonly"));
    // 不带 Require 就不该出现 sendonly 轨。
    QVERIFY(!sdp.contains("a=sendonly"));

    QCOMPARE(sdp::trackControl(baseOptions(), 1, quirks), QStringLiteral("trackID=1"));
}

void TestRtsp::sdpG722ClockRate()
{
    SdpOptions options = baseOptions();
    options.audioCodec = AudioCodec::G722;
    options.audioPayloadType = 9;

    const QByteArray sdp = sdp::generate(options, Quirks());
    // RFC 3551 §4.5.2：G.722 真实采 16 kHz，但 RTP 时钟率必须写 8000。
    // 写成 16000 会让一大批客户端放成两倍速，是真机上的高发互操作坑。
    QVERIFY(sdp.contains("m=audio 0 RTP/AVP 9\r\n"));
    QVERIFY(sdp.contains("a=rtpmap:9 G722/8000/1"));
    QVERIFY(!sdp.contains("G722/16000"));
}

void TestRtsp::sdpNoRtpmap()
{
    Quirks quirks;
    quirks.setEnabled(QuirkId::SdpNoRtpmap, true);          // E11
    const QByteArray sdp = sdp::generate(baseOptions(), quirks);

    QVERIFY(sdp.contains("m=audio 0 RTP/AVP 0\r\n"));
    QVERIFY(!sdp.contains("a=rtpmap:0 "));                  // 静态 PT 才省掉 rtpmap
    QVERIFY(sdp.contains("a=rtpmap:96 H264/90000"));        // 动态 PT 没它就没法解码
}

void TestRtsp::sdpSessionLevelControl()
{
    Quirks quirks;
    quirks.setEnabled(QuirkId::SdpSessionLevelControl, true);   // E12
    const QByteArray sdp = sdp::generate(baseOptions(), quirks);

    QVERIFY(sdp.contains("a=control:rtsp://192.168.1.50:8554/Streaming/Channels/101\r\n"));
    QVERIFY(!sdp.contains("a=control:trackID="));
    QVERIFY(sdp::trackControl(baseOptions(), 0, quirks).isEmpty());
}

void TestRtsp::sdpBackchannelSingleTrack()
{
    // 海康风格：开了 backchannel 就只给一条 sendonly 对讲轨，麦克风轨不出现。
    SdpOptions options = baseOptions();
    options.hasBackchannel = true;
    options.backchannelCodec = AudioCodec::PCMU;
    options.backchannelPayloadType = 0;

    const QByteArray sdp = sdp::generate(options, Quirks());
    QCOMPARE(sdp.count("m=audio"), 1);
    const QByteArray audio = mediaSection(sdp, 1);
    QVERIFY(audio.contains("a=sendonly"));
    QVERIFY(!audio.contains("a=recvonly"));
    QVERIFY(audio.contains("a=control:trackID=1"));

    const QVector<sdp::TrackRole> layout = sdp::trackLayout(options);
    QCOMPARE(layout.size(), 2);
    QCOMPARE(layout.at(0), sdp::TrackRole::Video);
    QCOMPARE(layout.at(1), sdp::TrackRole::Backchannel);
}

void TestRtsp::sdpBackchannelDualTrack()
{
    // 大华风格（E6）：麦克风 recvonly 在前 + 对讲 sendonly 在后。
    // 客户端不挑轨就会推到麦克风轨上，相机那头一点声音都没有。
    SdpOptions options = baseOptions();
    options.hasBackchannel = true;
    options.dualTrackLayout = true;
    options.backchannelCodec = AudioCodec::PCMA;
    options.backchannelPayloadType = 8;

    const QByteArray sdp = sdp::generate(options, Quirks());
    QCOMPARE(sdp.count("m=audio"), 2);

    const QByteArray mic = mediaSection(sdp, 1);
    QVERIFY(mic.startsWith("m=audio 0 RTP/AVP 0"));
    QVERIFY(mic.contains("a=recvonly"));
    QVERIFY(mic.contains("a=control:trackID=1"));

    const QByteArray talk = mediaSection(sdp, 2);
    QVERIFY(talk.startsWith("m=audio 0 RTP/AVP 8"));
    QVERIFY(talk.contains("a=rtpmap:8 PCMA/8000/1"));
    QVERIFY(talk.contains("a=sendonly"));
    QVERIFY(talk.contains("a=control:trackID=2"));

    const QVector<sdp::TrackRole> layout = sdp::trackLayout(options);
    QCOMPARE(layout.size(), 3);
    QCOMPARE(layout.at(1), sdp::TrackRole::Audio);
    QCOMPARE(layout.at(2), sdp::TrackRole::Backchannel);
}

void TestRtsp::sdpNoBackchannelWithoutRequire()
{
    SdpOptions options = baseOptions();
    options.hasBackchannel = false;
    const QByteArray sdp = sdp::generate(options, Quirks());
    QVERIFY(!sdp.contains("a=sendonly"));
    QCOMPARE(sdp::trackLayout(options).size(), 2);
}

void TestRtsp::sdpNonstandardCodec()
{
    // E10：SDP 只声明 G7221 这类不规范 codec。PT 必须落在动态区，
    // 否则客户端按 RFC 3551 静态表一反查又对上了 PCMU，这条 quirk 就白开了。
    Quirks quirks;
    quirks.setEnabled(QuirkId::SdpNonstandardCodec, true);
    quirks.setParam(QuirkId::SdpNonstandardCodec, QStringLiteral("name"),
                    QStringLiteral("G7221"));

    const QByteArray sdp = sdp::generate(baseOptions(), quirks);
    QVERIFY(sdp.contains("m=audio 0 RTP/AVP 98\r\n"));
    QVERIFY(sdp.contains("a=rtpmap:98 G7221/16000/1"));
    QVERIFY(!sdp.contains("PCMU"));
    QCOMPARE(sdp::effectiveAudioPayloadType(0, quirks), 98);
    QCOMPARE(sdp::effectiveAudioPayloadType(0, Quirks()), 0);
}

void TestRtsp::sdpAacFmtp()
{
    SdpOptions options = baseOptions();
    options.audioCodec = AudioCodec::AAC;
    options.audioPayloadType = 97;
    options.aacConfig = "1408";

    const QByteArray sdp = sdp::generate(options, Quirks());
    QVERIFY(sdp.contains("m=audio 0 RTP/AVP 97\r\n"));
    QVERIFY(sdp.contains("a=rtpmap:97 mpeg4-generic/16000/1"));
    QVERIFY(sdp.contains("mode=AAC-hbr"));
    QVERIFY(sdp.contains("sizelength=13;indexlength=3;indexdeltalength=3"));
    QVERIFY(sdp.contains("config=1408"));
}

void TestRtsp::sdpSpsPpsInbandOnly()
{
    Quirks quirks;
    quirks.setEnabled(QuirkId::SpsPpsPlacement, true);
    quirks.setParam(QuirkId::SpsPpsPlacement, QStringLiteral("value"),
                    QStringLiteral("inband_only"));

    const QByteArray sdp = sdp::generate(baseOptions(), quirks);
    QVERIFY(!sdp.contains("sprop-parameter-sets"));
    QVERIFY(sdp.contains("profile-level-id=42c01f"));   // 只去参数集，别的 fmtp 照旧
}

// ---------------------------------------------------------------------------
// RTP 包头
// ---------------------------------------------------------------------------

void TestRtsp::rtpHeaderRoundTrip()
{
    const QByteArray payload("\x01\x02\x03\x04", 4);
    const QByteArray packet = rtpPacket(96, true, 0xFFFE, 0x11223344, 0xDEADBEEF, payload);

    QCOMPARE(packet.size(), rtp::kHeaderSize + 4);
    QCOMPARE(quint8(packet.at(0)), quint8(0x80));       // V=2，无 P / X / CC
    QCOMPARE(quint8(packet.at(1)), quint8(0x80 | 96));  // marker + PT

    rtp::Header header;
    QVERIFY(rtp::parseHeader(packet, &header));
    QCOMPARE(header.version, 2);
    QVERIFY(header.marker);
    QCOMPARE(header.payloadType, 96);
    QCOMPARE(header.sequence, quint16(0xFFFE));
    QCOMPARE(header.timestamp, quint32(0x11223344));
    QCOMPARE(header.ssrc, quint32(0xDEADBEEF));
    QCOMPARE(header.headerBytes, rtp::kHeaderSize);
    QCOMPARE(rtp::payloadOf(packet, header), payload);

    // marker=0 的普通包。
    const QByteArray plain = rtpPacket(0, false, 1, 160, 1, QByteArray(160, '\xff'));
    QCOMPARE(quint8(plain.at(1)), quint8(0));
    rtp::Header plainHeader;
    QVERIFY(rtp::parseHeader(plain, &plainHeader));
    QVERIFY(!plainHeader.marker);
    QCOMPARE(plainHeader.payloadType, 0);
}

void TestRtsp::rtpHeaderRejectsBadVersion()
{
    QByteArray packet = rtpPacket(0, false, 1, 0, 0, QByteArray(4, 'a'));
    packet[0] = char(0x40);                             // V=1
    QVERIFY(!rtp::parseHeader(packet, nullptr));
    QVERIFY(!rtp::parseHeader(QByteArray(11, '\0'), nullptr));   // 头都不够长
}

void TestRtsp::rtpHeaderSkipsCsrcAndExtension()
{
    // 对讲侧收的是客户端发的包，CSRC 与头扩展都可能出现，
    // 跳不干净就会把这些字节当成音频采样去算电平。
    QByteArray packet = rtpPacket(0, false, 7, 0, 0, QByteArray());
    packet[0] = static_cast<char>(static_cast<unsigned char>(0x80 | 0x10 | 0x02));               // X=1，CC=2
    packet += QByteArray(8, '\x01');                    // 两个 CSRC
    packet += QByteArray("\x00\x01\x00\x01", 4);        // 扩展头：1 个 32 位字
    packet += QByteArray(4, '\x02');
    const QByteArray payload("audio", 5);
    packet += payload;

    rtp::Header header;
    QVERIFY(rtp::parseHeader(packet, &header));
    QCOMPARE(header.csrcCount, 2);
    QVERIFY(header.extension);
    QCOMPARE(header.headerBytes, rtp::kHeaderSize + 8 + 8);
    QCOMPARE(rtp::payloadOf(packet, header), payload);
}

void TestRtsp::rtpHeaderStripsPadding()
{
    QByteArray packet = rtpPacket(0, false, 1, 0, 0, QByteArray("abc", 3));
    packet[0] = static_cast<char>(static_cast<unsigned char>(0x80 | 0x20));                      // P=1
    packet += QByteArray(4, '\0');
    packet[packet.size() - 1] = char(4);                // 填充 4 字节，长度字节把自己也算进去

    rtp::Header header;
    QVERIFY(rtp::parseHeader(packet, &header));
    QVERIFY(header.padding);
    QCOMPARE(rtp::payloadOf(packet, header), QByteArray("abc", 3));
}

// ---------------------------------------------------------------------------
// interleaved 封装
// ---------------------------------------------------------------------------

void TestRtsp::interleavedFraming()
{
    const QByteArray rtpData = rtpPacket(0, true, 100, 0, 0x12345678, QByteArray(160, '\xff'));
    const QByteArray frame = rtp::interleavedFrame(0, rtpData);

    QCOMPARE(frame.at(0), '$');
    QCOMPARE(quint8(frame.at(1)), quint8(0));                        // 对讲走 channel 0
    QCOMPARE(quint8(frame.at(2)), quint8((rtpData.size() >> 8) & 0xff));
    QCOMPARE(quint8(frame.at(3)), quint8(rtpData.size() & 0xff));
    QCOMPARE(frame.size(), rtpData.size() + 4);

    // 两帧粘在一起 + 一截没收全的尾巴：前两帧要能依次拆出来，尾巴留在缓冲里。
    QByteArray buffer = frame + rtp::interleavedFrame(1, QByteArray("rtcp", 4));
    buffer += QByteArray("$\x00\x00", 3);

    int channel = -1;
    QByteArray payload;
    QVERIFY(rtp::takeInterleavedFrame(buffer, &channel, &payload));
    QCOMPARE(channel, 0);
    QCOMPARE(payload, rtpData);

    QVERIFY(rtp::takeInterleavedFrame(buffer, &channel, &payload));
    QCOMPARE(channel, 1);
    QCOMPARE(payload, QByteArray("rtcp", 4));

    QVERIFY(!rtp::takeInterleavedFrame(buffer, &channel, &payload));
    QCOMPARE(buffer.size(), qsizetype(3));
}

void TestRtsp::interleavedPartialFrame()
{
    const QByteArray full = rtp::interleavedFrame(2, QByteArray(50, 'x'));
    QByteArray buffer = full.left(full.size() - 1);
    int channel = -1;
    QByteArray payload;
    QVERIFY(!rtp::takeInterleavedFrame(buffer, &channel, &payload));
    QCOMPARE(buffer.size(), full.size() - 1);           // 收不全就别动缓冲区

    buffer += full.right(1);
    QVERIFY(rtp::takeInterleavedFrame(buffer, &channel, &payload));
    QCOMPARE(channel, 2);
    QCOMPARE(payload.size(), qsizetype(50));
    QVERIFY(buffer.isEmpty());
}

// ---------------------------------------------------------------------------
// 对讲接收
// ---------------------------------------------------------------------------

void TestRtsp::receiverCountsLossAndMarker()
{
    RtpReceiver receiver;
    receiver.setCodec(AudioCodec::PCMU);
    const QByteArray payload(160, '\x00');              // μ-law 的 0x00 是满幅

    receiver.feed(rtpPacket(0, true, 1000, 0, 1, payload));    // talkspurt 首包带 marker
    receiver.feed(rtpPacket(0, false, 1001, 160, 1, payload));
    receiver.feed(rtpPacket(0, false, 1003, 480, 1, payload)); // 1002 丢了

    const TalkbackStats stats = receiver.stats();
    QCOMPARE(stats.packetsReceived, qint64(3));
    QCOMPARE(stats.packetsDropped, qint64(1));
    QCOMPARE(stats.markerCount, qint64(1));
    QCOMPARE(stats.markerMissing, qint64(0));
    QCOMPARE(stats.codec, AudioCodec::PCMU);
    QVERIFY(stats.bytesReceived >= qint64(3 * (rtp::kHeaderSize + 160)));
    QVERIFY(stats.peakLevel > 0.0);
    QVERIFY(stats.firstPacketAt.isValid());

    // 版本不对的包一律不进统计。
    QByteArray bad = rtpPacket(0, false, 1004, 640, 1, payload);
    bad[0] = char(0x00);
    receiver.feed(bad);
    QCOMPARE(receiver.stats().packetsReceived, qint64(3));
}

void TestRtsp::receiverRequiresMarker()
{
    // E13：相机要求每段话首包置 marker，缺了整段静音丢弃。
    Quirks quirks;
    quirks.setEnabled(QuirkId::TalkbackRequireMarker, true);

    RtpReceiver receiver;
    receiver.setCodec(AudioCodec::PCMU);
    receiver.setQuirks(&quirks);
    const QByteArray payload(160, '\x00');

    receiver.feed(rtpPacket(0, false, 200, 0, 1, payload));    // 首包没有 marker
    receiver.feed(rtpPacket(0, false, 201, 160, 1, payload));
    QCOMPARE(receiver.stats().markerMissing, qint64(1));
    QCOMPARE(receiver.stats().currentLevel, 0.0);
    QCOMPARE(receiver.stats().peakLevel, 0.0);

    // 下一个带 marker 的包把这段恢复回来。
    receiver.feed(rtpPacket(0, true, 202, 320, 1, payload));
    QCOMPARE(receiver.stats().markerCount, qint64(1));
    QVERIFY(receiver.stats().currentLevel > 0.0);

    // 不开这条 quirk 时只记账，不丢音。
    RtpReceiver lenient;
    lenient.setCodec(AudioCodec::PCMU);
    lenient.feed(rtpPacket(0, false, 300, 0, 1, payload));
    QCOMPARE(lenient.stats().markerMissing, qint64(1));
    QVERIFY(lenient.stats().currentLevel > 0.0);
}

QTEST_GUILESS_MAIN(TestRtsp)
#include "tst_rtsp.moc"
