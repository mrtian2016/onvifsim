#pragma once

// 所有 ONVIF / WS-* 命名空间常量。分发、解析、生成响应都从这里取，
// 不要在各模块里散落字面量。
//
// 注意两处易错点：
//   - WS-Discovery 用的 addressing 是 2004/08，SOAP 消息里的 wsa 是 2005/08，不是同一个。
//   - Media(ver10) 与 Media2(ver20) 的命名空间都含 "/media/"，
//     分发必须按带版本的完整串匹配（见 quirk A6）。

namespace onvifsim {
namespace ns {

// ---- SOAP / XML ----
inline constexpr const char *Soap11 = "http://schemas.xmlsoap.org/soap/envelope/";
inline constexpr const char *Soap12 = "http://www.w3.org/2003/05/soap-envelope";
inline constexpr const char *Soap12Enc = "http://www.w3.org/2003/05/soap-encoding";
inline constexpr const char *XmlSchema = "http://www.w3.org/2001/XMLSchema";
inline constexpr const char *XmlSchemaInstance = "http://www.w3.org/2001/XMLSchema-instance";

// ---- WS-Addressing ----
inline constexpr const char *WsAddressing2005 = "http://www.w3.org/2005/08/addressing";
inline constexpr const char *WsAddressing2004 = "http://schemas.xmlsoap.org/ws/2004/08/addressing";

// ---- WS-Discovery ----
inline constexpr const char *Discovery2005 = "http://schemas.xmlsoap.org/ws/2005/04/discovery";
inline constexpr const char *Discovery2009 = "http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01";
inline constexpr const char *DiscoveryNetworkVideo = "http://www.onvif.org/ver10/network/wsdl";

// ---- WS-Security ----
inline constexpr const char *WsseSecext =
    "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd";
inline constexpr const char *WsuUtility =
    "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd";
inline constexpr const char *WssePasswordText =
    "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0"
    "#PasswordText";
inline constexpr const char *WssePasswordDigest =
    "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0"
    "#PasswordDigest";
inline constexpr const char *WsseBase64Binary =
    "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0"
    "#Base64Binary";

// ---- WS-BaseNotification / WS-Topics ----
inline constexpr const char *Wsnt = "http://docs.oasis-open.org/wsn/b-2";
inline constexpr const char *Wsntw = "http://docs.oasis-open.org/wsn/bw-2";
inline constexpr const char *Wstop = "http://docs.oasis-open.org/wsn/t-1";
inline constexpr const char *Wsrfbf = "http://docs.oasis-open.org/wsrf/bf-2";
inline constexpr const char *Wsrfr = "http://docs.oasis-open.org/wsrf/r-2";

// ---- ONVIF 服务 ----
inline constexpr const char *Device = "http://www.onvif.org/ver10/device/wsdl";
inline constexpr const char *Media = "http://www.onvif.org/ver10/media/wsdl";
inline constexpr const char *Media2 = "http://www.onvif.org/ver20/media/wsdl";
inline constexpr const char *Ptz = "http://www.onvif.org/ver20/ptz/wsdl";
inline constexpr const char *Imaging = "http://www.onvif.org/ver20/imaging/wsdl";
inline constexpr const char *Events = "http://www.onvif.org/ver10/events/wsdl";
inline constexpr const char *Analytics = "http://www.onvif.org/ver20/analytics/wsdl";
inline constexpr const char *DeviceIo = "http://www.onvif.org/ver10/deviceIO/wsdl";
inline constexpr const char *Replay = "http://www.onvif.org/ver10/replay/wsdl";
inline constexpr const char *Recording = "http://www.onvif.org/ver10/recording/wsdl";
inline constexpr const char *Search = "http://www.onvif.org/ver10/search/wsdl";

// ---- ONVIF 通用 schema ----
inline constexpr const char *Tt = "http://www.onvif.org/ver10/schema";
inline constexpr const char *Ter = "http://www.onvif.org/ver10/error";
inline constexpr const char *Tns1 = "http://www.onvif.org/ver10/topics";

// ---- 厂商 ----
inline constexpr const char *Axis = "http://www.axis.com/2009/event/topics";

// ---- 常用前缀（生成响应时用）----
namespace prefix {
inline constexpr const char *Soap = "s";
inline constexpr const char *Tds = "tds";
inline constexpr const char *Trt = "trt";
inline constexpr const char *Tr2 = "tr2";
inline constexpr const char *Tptz = "tptz";
inline constexpr const char *Timg = "timg";
inline constexpr const char *Tev = "tev";
inline constexpr const char *Tan = "tan";
inline constexpr const char *Tmd = "tmd";
inline constexpr const char *Tt = "tt";
inline constexpr const char *Ter = "ter";
inline constexpr const char *Tns1 = "tns1";
inline constexpr const char *Wsa = "wsa";
inline constexpr const char *Wsnt = "wsnt";
inline constexpr const char *Wstop = "wstop";
inline constexpr const char *Wsse = "wsse";
inline constexpr const char *Wsu = "wsu";
inline constexpr const char *Xsi = "xsi";
inline constexpr const char *Xsd = "xsd";
} // namespace prefix

// 服务命名空间 → 该服务在 GetServices / GetCapabilities 里的短名。
const char *serviceShortName(const char *serviceNamespace);

} // namespace ns
} // namespace onvifsim
