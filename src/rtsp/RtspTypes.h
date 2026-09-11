#pragma once

// RTSP 的报文类型与传输描述：请求 / 响应 / Transport 头的解析与序列化。
//
// 只放「纯数据 + 纯解析」，不碰 socket、不碰相机状态 —— RtspServer 与
// RtspSession 两边都要用同一套规则（一边按路径反查 profile、一边按 trackID
// 定位轨），把它们摘到这里，两处才不会各写一份迟早对不上。

#include <QtCore/QByteArray>
#include <QtCore/QMap>
#include <QtCore/QString>

namespace onvifsim {

struct RtspRequest {
    QByteArray method;        // OPTIONS DESCRIBE SETUP PLAY PAUSE TEARDOWN GET_PARAMETER SET_PARAMETER
    QString uri;
    QByteArray version = "RTSP/1.0";
    QMap<QByteArray, QByteArray> headers;   // key 小写
    QByteArray body;

    QByteArray header(const char *name) const;
    bool hasHeader(const char *name) const;
    int cseq() const;
    QByteArray sessionId() const;
    // Require: www.onvif.org/ver20/backchannel
    bool wantsBackchannel() const;
};

struct RtspResponse {
    int status = 200;
    QByteArray reason;
    QMap<QByteArray, QByteArray> headers;
    QByteArray body;

    void setHeader(const QByteArray &name, const QByteArray &value);
    // 追加一条同名头而不是覆盖。quirk E8 要发两条 WWW-Authenticate，
    // 而 headers 是 QMap 装不下重复键 —— 这里把多个值用 '\n' 串起来，
    // serialize() 时拆成多行，与 HttpResponse 的约定一致（见 net/HttpTypes.h）。
    void addHeader(const QByteArray &name, const QByteArray &value);
    void setBody(const QByteArray &data, const QByteArray &contentType);
    QByteArray serialize() const;
    static const char *defaultReason(int status);
    static RtspResponse make(int status, int cseq);
};

// SETUP 协商出来的传输方式。
struct RtspTransport {
    bool tcpInterleaved = false;
    int interleavedRtp = 0;
    int interleavedRtcp = 1;
    quint16 clientRtpPort = 0;
    quint16 clientRtcpPort = 0;
    quint16 serverRtpPort = 0;
    quint16 serverRtcpPort = 0;
    bool multicast = false;
    QString destination;

    static RtspTransport parse(const QByteArray &transportHeader, bool *ok = nullptr);
    QByteArray toHeader() const;
};

} // namespace onvifsim
