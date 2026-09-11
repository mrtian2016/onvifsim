#pragma once

// SOAP 信封解析。SOAP 1.2（application/soap+xml）为主，1.1（text/xml）也收。
// 解析结果里同时带上 WS-Security UsernameToken 的原始字段，
// 校验在 WsSecurity 里做（要用到相机的用户表与时间窗 quirk）。

#include "soap/XmlNode.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

namespace onvifsim {

struct SoapRequest {
    int soapVersion = 12;        // 11 或 12
    QString action;              // wsa:Action，可空
    QString to;                  // wsa:To
    QString messageId;
    QString replyTo;

    QString bodyNamespace;       // Body 首元素的命名空间 —— 分发依据之一
    QString bodyName;            // Body 首元素的 localName —— 操作名
    XmlNode body;                // Body 首元素子树

    // WS-Security UsernameToken 原始字段（未校验）
    bool hasSecurity = false;
    QString username;
    QString password;            // PasswordText 时是明文，Digest 时是 Base64 摘要
    bool passwordIsDigest = false;
    QString nonceBase64;
    QString created;             // xs:dateTime 原文

    QByteArray raw;              // 原始报文，进日志用
    QString parseError;
    bool isValid() const { return parseError.isEmpty() && !bodyName.isEmpty(); }
};

namespace soap {

// 解析一整段 SOAP 报文。失败时 parseError 非空。
SoapRequest parseEnvelope(const QByteArray &data);

// 按 Content-Type 猜 SOAP 版本；解析时以信封实际命名空间为准。
int soapVersionFromContentType(const QByteArray &contentType);
const char *contentTypeForVersion(int soapVersion);

} // namespace soap
} // namespace onvifsim
