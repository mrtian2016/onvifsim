// SOAP 层单测：XML 解析 / 路径查询、畸形属性修补、XmlWriter 正常与 D5 畸形输出、
// SOAP 1.1 与 1.2 信封解析、UsernameToken 字段提取与摘要校验、Fault 措辞与 HTTP 状态。
//
// 只依赖 core（Quirks / CameraModel）与 soap，不碰 net / services / rtsp。

#include <QtTest/QtTest>

#include "core/CameraModel.h"
#include "core/Quirks.h"
#include "soap/Envelope.h"
#include "soap/Fault.h"
#include "soap/Namespaces.h"
#include "soap/WsSecurity.h"
#include "soap/XmlNode.h"
#include "soap/XmlWriter.h"

using namespace onvifsim;

namespace {

// XML 样本一律用普通字符串字面量而不是 R"..."：moc 把 raw string 里的 "//" 当行注释，
// URL 一多就把后面的 Q_OBJECT 一起吃掉，链接时表现为 vtable 缺失。

// 参照客户端发出来的那种 1.2 报文：所有前缀声明在 Envelope 上，
// wsa 是 2005/08，UsernameToken 走 PasswordDigest。
const char *kRequest12 =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<SOAP-ENV:Envelope xmlns:SOAP-ENV=\"http://www.w3.org/2003/05/soap-envelope\"\n"
    "                   xmlns:wsa=\"http://www.w3.org/2005/08/addressing\"\n"
    "                   xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\"\n"
    "                   xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\"\n"
    "                   xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\"\n"
    "                   xmlns:tt=\"http://www.onvif.org/ver10/schema\">\n"
    "  <SOAP-ENV:Header>\n"
    "    <wsa:Action>http://www.onvif.org/ver10/media/wsdl/GetStreamUri</wsa:Action>\n"
    "    <wsa:To>http://192.168.1.10:2020/onvif/service</wsa:To>\n"
    "    <wsa:MessageID>urn:uuid:1f0e3dad-1111-4222-8333-000000000001</wsa:MessageID>\n"
    "    <wsa:ReplyTo>\n"
    "      <wsa:Address>http://www.w3.org/2005/08/addressing/anonymous</wsa:Address>\n"
    "    </wsa:ReplyTo>\n"
    "    <wsse:Security SOAP-ENV:mustUnderstand=\"1\">\n"
    "      <wsse:UsernameToken>\n"
    "        <wsse:Username>admin</wsse:Username>\n"
    "        <wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">tuOSpGlFlIXsozq4HFNeeGeFLEI=</wsse:Password>\n"
    "        <wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">LKqI6G/AikKCQrN0zqZFlg==</wsse:Nonce>\n"
    "        <wsu:Created>2010-09-16T07:50:45Z</wsu:Created>\n"
    "      </wsse:UsernameToken>\n"
    "    </wsse:Security>\n"
    "  </SOAP-ENV:Header>\n"
    "  <SOAP-ENV:Body>\n"
    "    <trt:GetStreamUri>\n"
    "      <trt:StreamSetup>\n"
    "        <tt:Stream>RTP-Unicast</tt:Stream>\n"
    "        <tt:Transport>\n"
    "          <tt:Protocol>RTSP</tt:Protocol>\n"
    "        </tt:Transport>\n"
    "      </trt:StreamSetup>\n"
    "      <trt:ProfileToken>Profile_1</trt:ProfileToken>\n"
    "    </trt:GetStreamUri>\n"
    "  </SOAP-ENV:Body>\n"
    "</SOAP-ENV:Envelope>\n";

// 1.1 报文：信封 ns 不同，PasswordText，没有 wsa 头。
const char *kRequest11 =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"\n"
    "            xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\"\n"
    "            xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\">\n"
    "  <s:Header>\n"
    "    <wsse:Security>\n"
    "      <wsse:UsernameToken>\n"
    "        <wsse:Username>operator</wsse:Username>\n"
    "        <wsse:Password>plain-secret</wsse:Password>\n"
    "      </wsse:UsernameToken>\n"
    "    </wsse:Security>\n"
    "  </s:Header>\n"
    "  <s:Body>\n"
    "    <tds:GetDeviceInformation/>\n"
    "  </s:Body>\n"
    "</s:Envelope>\n";

// TP-Link TL-IPC 真机的 GetEventProperties：属性值不加引号，标准解析器直接报错。
const char *kBadXml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<wstop:TopicSet xmlns:wstop=\"http://docs.oasis-open.org/wsn/t-1\"\n"
    "                xmlns:tns1=\"http://www.onvif.org/ver10/topics\">\n"
    "  <tns1:RuleEngine>\n"
    "    <CellMotionDetector>\n"
    "      <Motion wstop:topic=true kept=\"yes\">\n"
    "        <MessageDescription IsProperty=true/>\n"
    "      </Motion>\n"
    "    </CellMotionDetector>\n"
    "  </tns1:RuleEngine>\n"
    "</wstop:TopicSet>\n";

// 客户端用来判定「这是认证错误」的关键词，authFault 的 Unknown 档必须一个都不含。
const char *kAuthKeywords[] = { "notauthorized", "not authorized",         "unauthorized",
                                "authentication", "sender not authorized", "failedauthentication" };

bool containsAuthKeyword(const QByteArray &fault)
{
    const QString lower = QString::fromUtf8(fault).toLower();
    for (const char *kw : kAuthKeywords) {
        if (lower.contains(QLatin1String(kw)))
            return true;
    }
    return false;
}

CameraModel makeModel()
{
    CameraModel m;
    m.users = { User{ QStringLiteral("admin"), QStringLiteral("admin-pw"),
                      UserLevel::Administrator },
                User{ QStringLiteral("guest"), QStringLiteral("guest-pw"), UserLevel::User } };
    return m;
}

SoapRequest makeToken(const QString &user, const QString &password, bool digest,
                      const QString &nonceB64, const QString &created)
{
    SoapRequest req;
    req.hasSecurity = true;
    req.username = user;
    req.passwordIsDigest = digest;
    req.nonceBase64 = nonceB64;
    req.created = created;
    req.password = digest ? WsSecurity::computePasswordDigest(
                                QByteArray::fromBase64(nonceB64.toLatin1()), created, password)
                          : password;
    req.bodyName = QStringLiteral("GetProfiles");
    return req;
}

QDateTime utc(const QString &iso)
{
    return QDateTime::fromString(iso, Qt::ISODate).toUTC();
}

} // namespace

class TstSoap : public QObject
{
    Q_OBJECT

private slots:
    // ---- XmlNode ----
    void xmlParseTree();
    void xmlPathQuery();
    void xmlAccessors();
    void xmlParseFailure();
    void xmlRepairUnquotedAttributes();
    void xmlRepairLeavesGoodXmlAlone();
    void xmlParseFallsBackToRepair();
    void xmlEscaping();

    // ---- XmlWriter ----
    void writerEnvelope12();
    void writerEnvelope11();
    void writerHelpers();
    void writerMalformedAttributes();
    void writerFragmentAndTake();

    // ---- Envelope ----
    void envelope12();
    void envelope11();
    void envelopeVersionFromContentType();
    void envelopeErrors();

    // ---- WS-Security ----
    void passwordDigestVectors_data();
    void passwordDigestVectors();
    void verifyDigestAndText();
    void verifyRejects();
    void verifyTimeWindow();
    void verifyPasswordTextOnlyQuirk();
    void verifyNonceStrictOnce();
    void levelGating();

    // ---- Fault ----
    void faultWording_data();
    void faultWording();
    void faultDefaultWording();
    void faultHttpStatus();
    void faultStructure12();
    void faultStructure11();
    void faultHelpers();

    // ---- Namespaces ----
    void serviceShortNames();
};

// ---------------------------------------------------------------- XmlNode

void TstSoap::xmlParseTree()
{
    QString error;
    const XmlNode root = xml::parse(QByteArray(kRequest12), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(root.name, QStringLiteral("Envelope"));
    QCOMPARE(root.ns, QLatin1String(ns::Soap12));
    QCOMPARE(root.children.size(), 2);

    const XmlNode *body = root.child(QStringLiteral("Body"));
    QVERIFY(body);
    QCOMPARE(body->children.size(), 1);
    QCOMPARE(body->children.first().ns, QLatin1String(ns::Media));
    QCOMPARE(body->children.first().name, QStringLiteral("GetStreamUri"));
}

void TstSoap::xmlPathQuery()
{
    const XmlNode root = xml::parse(QByteArray(kRequest12));
    const XmlNode *protocol = root.path(QStringLiteral(
        "Body/GetStreamUri/StreamSetup/Transport/Protocol"));
    QVERIFY(protocol);
    QCOMPARE(protocol->text, QStringLiteral("RTSP"));

    // 首尾与重复斜杠都要吃得下
    QVERIFY(root.path(QStringLiteral("/Body/GetStreamUri//ProfileToken")));
    QCOMPARE(root.path(QStringLiteral("Body/GetStreamUri/ProfileToken"))->text,
             QStringLiteral("Profile_1"));
    QVERIFY(!root.path(QStringLiteral("Body/GetStreamUri/Nope")));
    QVERIFY(!root.path(QString()));

    // 带 ns 的查找：Body 首元素必须按完整 ns 匹配（分发就靠它）
    QVERIFY(root.child(QLatin1String(ns::Soap12), QStringLiteral("Body")));
    QVERIFY(!root.child(QLatin1String(ns::Soap11), QStringLiteral("Body")));
}

void TstSoap::xmlAccessors()
{
    const QByteArray doc =
        "<Root a=\"1\"><Count>42</Count><Speed>0.75</Speed><Flag>TRUE</Flag>"
        "<Off>0</Off><Junk>abc</Junk><Item>x</Item><Item>y</Item></Root>";
    const XmlNode root = xml::parse(doc);
    QCOMPARE(root.attribute(QStringLiteral("a")), QStringLiteral("1"));
    QCOMPARE(root.attribute(QStringLiteral("b"), QStringLiteral("def")), QStringLiteral("def"));
    QCOMPARE(root.childInt(QStringLiteral("Count")), 42);
    QCOMPARE(root.childInt(QStringLiteral("Junk"), 7), 7);
    QCOMPARE(root.childInt(QStringLiteral("Missing"), 7), 7);
    QCOMPARE(root.childDouble(QStringLiteral("Speed")), 0.75);
    QCOMPARE(root.childBool(QStringLiteral("Flag")), true);
    QCOMPARE(root.childBool(QStringLiteral("Off"), true), false);
    QCOMPARE(root.childBool(QStringLiteral("Junk"), true), true);
    QCOMPARE(root.childText(QStringLiteral("Item")), QStringLiteral("x"));
    QCOMPARE(root.childrenNamed(QStringLiteral("Item")).size(), 2);
    QVERIFY(root.hasChild(QStringLiteral("Count")));
    QVERIFY(!root.hasChild(QStringLiteral("Nope")));
    QVERIFY(XmlNode().isNull());
    QVERIFY(!root.toDebugString().isEmpty());
}

void TstSoap::xmlParseFailure()
{
    QString error;
    const XmlNode root = xml::parse(QByteArray("<a><b></a>"), &error);
    QVERIFY(root.isNull());
    QVERIFY(!error.isEmpty());

    error.clear();
    QVERIFY(xml::parse(QByteArray("not xml at all"), &error).isNull());
    QVERIFY(!error.isEmpty());

    error.clear();
    QVERIFY(xml::parse(QByteArray(), &error).isNull());
    QVERIFY(!error.isEmpty());
}

void TstSoap::xmlRepairUnquotedAttributes()
{
    const QByteArray repaired = xml::repairUnquotedAttributes(QByteArray(kBadXml));
    QVERIFY(repaired.contains("wstop:topic=\"true\""));
    QVERIFY(repaired.contains("kept=\"yes\""));
    // "IsProperty=true/>" 里的 '/' 属于标签而不属于值
    QVERIFY(repaired.contains("IsProperty=\"true\"/>"));

    QString error;
    const XmlNode root = xml::parse(repaired, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    const XmlNode *motion = root.path(QStringLiteral("RuleEngine/CellMotionDetector/Motion"));
    QVERIFY(motion);
    QCOMPARE(motion->attribute(QStringLiteral("topic")), QStringLiteral("true"));
    QCOMPARE(motion->attribute(QStringLiteral("kept")), QStringLiteral("yes"));
}

void TstSoap::xmlRepairLeavesGoodXmlAlone()
{
    const QByteArray good = QByteArray(kRequest12);
    QCOMPARE(xml::repairUnquotedAttributes(good), good);

    // 注释 / CDATA / 处理指令里的 '=' 不是属性，不许动
    const QByteArray tricky = "<a x=\"1\"><!-- y=2 --><![CDATA[z=3]]><b/>text=4</a>";
    QCOMPARE(xml::repairUnquotedAttributes(tricky), tricky);

    // 单引号与值里带空格的引号值原样保留
    const QByteArray quoted = "<a x='1 2' y=\"3 4\"/>";
    QCOMPARE(xml::repairUnquotedAttributes(quoted), quoted);
}

void TstSoap::xmlParseFallsBackToRepair()
{
    // parse() 本身要容错：直接喂真机的非法 XML 也得出树。
    QString error;
    const XmlNode root = xml::parse(QByteArray(kBadXml), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(root.name, QStringLiteral("TopicSet"));
    QCOMPARE(root.path(QStringLiteral("RuleEngine/CellMotionDetector/Motion"))
                 ->attribute(QStringLiteral("topic")),
             QStringLiteral("true"));
}

void TstSoap::xmlEscaping()
{
    QCOMPARE(xml::escape(QStringLiteral("a<b>&c")), QStringLiteral("a&lt;b&gt;&amp;c"));
    QCOMPARE(xml::escapeAttribute(QStringLiteral("a\"b'c&d")),
             QStringLiteral("a&quot;b&apos;c&amp;d"));
}

// -------------------------------------------------------------- XmlWriter

void TstSoap::writerEnvelope12()
{
    XmlWriter w(12);
    w.declareServicePrefixes();
    w.startEnvelope();
    {
        XmlWriter::Scope s(w, QStringLiteral("tds:GetDeviceInformationResponse"));
        w.element(QStringLiteral("tds:Manufacturer"), QStringLiteral("A&B<C>"));
        w.element(QStringLiteral("tds:Model"), QStringLiteral("Virtual"));
        w.emptyElement(QStringLiteral("tds:HardwareId"));
    }
    w.endEnvelope();
    const QByteArray out = w.take();

    QVERIFY(out.startsWith("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"));
    // 所有前缀声明在 Envelope 上
    QVERIFY(out.contains("<s:Envelope "));
    QVERIFY(out.contains("xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""));
    QVERIFY(out.contains("xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\""));
    QVERIFY(out.contains("xmlns:tr2=\"http://www.onvif.org/ver20/media/wsdl\""));
    QVERIFY(out.contains("<s:Body>"));
    QVERIFY(out.contains("<tds:Manufacturer>A&amp;B&lt;C&gt;</tds:Manufacturer>"));
    QVERIFY(out.contains("<tds:HardwareId/>"));
    QVERIFY(out.endsWith("</s:Envelope>"));
    QVERIFY(w.isEmpty());

    QString error;
    const XmlNode root = xml::parse(out, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(root.ns, QLatin1String(ns::Soap12));
    QCOMPARE(root.path(QStringLiteral("Body/GetDeviceInformationResponse/Model"))->text,
             QStringLiteral("Virtual"));
}

void TstSoap::writerEnvelope11()
{
    XmlWriter w(11);
    QCOMPARE(w.soapVersion(), 11);
    w.declareServicePrefixes();
    w.startEnvelope();
    w.element(QStringLiteral("tds:GetWsdlUrlResponse"), QStringLiteral("http://x/wsdl"));
    w.endEnvelope();
    const QByteArray out = w.take();

    QVERIFY(out.contains("xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""));
    QCOMPARE(xml::parse(out).ns, QLatin1String(ns::Soap11));

    // 非 11 的任何取值都归到 1.2
    QCOMPARE(XmlWriter(12).soapVersion(), 12);
    QCOMPARE(XmlWriter(0).soapVersion(), 12);
}

void TstSoap::writerHelpers()
{
    XmlWriter w;
    w.startFragment();
    w.start(QStringLiteral("tt:Options"));
    // 片段模式只写调用方要求的东西：要自带命名空间就自己声明，不像信封那样代劳。
    w.attr(QStringLiteral("xmlns:tt"), QLatin1String(ns::Tt));
    w.attr(QStringLiteral("token"), QStringLiteral("a\"b"));
    w.resolution(QStringLiteral("tt:Resolution"), 1920, 1080);
    w.intRange(QStringLiteral("tt:Quality"), 1, 6);
    w.floatRange(QStringLiteral("tt:Speed"), -1.0, 1.5);
    w.element(QStringLiteral("tt:FrameRate"), 15.0);
    w.element(QStringLiteral("tt:Fine"), 0.0625);
    w.element(QStringLiteral("tt:Count"), 3);
    w.element(QStringLiteral("tt:On"), true);
    w.raw(QByteArray("<tt:Raw>&amp;</tt:Raw>"));
    w.end();
    const QByteArray out = w.take();

    QVERIFY(out.contains("token=\"a&quot;b\""));
    QVERIFY(out.contains("<tt:Resolution><tt:Width>1920</tt:Width><tt:Height>1080</tt:Height>"
                         "</tt:Resolution>"));
    QVERIFY(out.contains("<tt:Quality><tt:Min>1</tt:Min><tt:Max>6</tt:Max></tt:Quality>"));
    QVERIFY(out.contains("<tt:Speed><tt:Min>-1</tt:Min><tt:Max>1.5</tt:Max></tt:Speed>"));
    // 浮点定点输出，不要 "15.000000" 也不要科学计数
    QVERIFY(out.contains("<tt:FrameRate>15</tt:FrameRate>"));
    QVERIFY(out.contains("<tt:Fine>0.0625</tt:Fine>"));
    QVERIFY(out.contains("<tt:On>true</tt:On>"));
    QVERIFY(out.contains("<tt:Raw>&amp;</tt:Raw>"));
    QVERIFY(!out.contains("Envelope"));

    QString error;
    xml::parse(out, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
}

void TstSoap::writerMalformedAttributes()
{
    XmlWriter w(12);
    w.declarePrefix(QStringLiteral("wstop"), QLatin1String(ns::Wstop));
    w.declarePrefix(QStringLiteral("tev"), QLatin1String(ns::Events));
    // Dispatcher 是在 startEnvelope 之前就把开关打开的，信封的 xmlns 仍须带引号。
    w.setUnquotedAttributes(true);
    QVERIFY(w.unquotedAttributes());
    w.startEnvelope();
    {
        XmlWriter::Scope s(w, QStringLiteral("tev:GetEventPropertiesResponse"));
        w.start(QStringLiteral("wstop:TopicSet"));
        w.attr(QStringLiteral("wstop:topic"), QStringLiteral("true"));
        w.emptyElement(QStringLiteral("Motion"));
        w.end();
    }
    w.endEnvelope();
    const QByteArray out = w.take();

    QVERIFY(out.contains("wstop:topic=true"));
    QVERIFY(!out.contains("wstop:topic=\"true\""));
    QVERIFY(out.contains("xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""));
    QVERIFY(out.contains("xmlns:wstop=\"http://docs.oasis-open.org/wsn/t-1\""));

    // 补上引号之后才解析得动 —— 这正是参照客户端的修补路径。
    QString error;
    const XmlNode root = xml::parse(xml::repairUnquotedAttributes(out), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(root.path(QStringLiteral("Body/GetEventPropertiesResponse/TopicSet"))
                 ->attribute(QStringLiteral("topic")),
             QStringLiteral("true"));
}

void TstSoap::writerFragmentAndTake()
{
    XmlWriter w;
    QVERIFY(w.isEmpty());
    w.startFragment();
    w.element(QStringLiteral("a"), QStringLiteral("1"));
    QVERIFY(!w.isEmpty());
    QCOMPARE(w.peek(), w.peek());
    const QByteArray first = w.take();
    QVERIFY(first.contains("<a>1</a>"));
    QVERIFY(w.isEmpty());

    // take() 之后 writer 还能接着用，前缀声明留着
    w.startEnvelope();
    w.endEnvelope();
    const QByteArray second = w.take();
    QVERIFY(second.contains("xmlns:tt=\"http://www.onvif.org/ver10/schema\""));
}

// --------------------------------------------------------------- Envelope

void TstSoap::envelope12()
{
    const SoapRequest req = soap::parseEnvelope(QByteArray(kRequest12));
    QVERIFY2(req.parseError.isEmpty(), qPrintable(req.parseError));
    QVERIFY(req.isValid());
    QCOMPARE(req.soapVersion, 12);
    QCOMPARE(req.action, QStringLiteral("http://www.onvif.org/ver10/media/wsdl/GetStreamUri"));
    QCOMPARE(req.to, QStringLiteral("http://192.168.1.10:2020/onvif/service"));
    QCOMPARE(req.messageId, QStringLiteral("urn:uuid:1f0e3dad-1111-4222-8333-000000000001"));
    QCOMPARE(req.replyTo, QStringLiteral("http://www.w3.org/2005/08/addressing/anonymous"));
    QCOMPARE(req.bodyNamespace, QLatin1String(ns::Media));
    QCOMPARE(req.bodyName, QStringLiteral("GetStreamUri"));
    QCOMPARE(req.body.childText(QStringLiteral("ProfileToken")), QStringLiteral("Profile_1"));
    QCOMPARE(req.raw, QByteArray(kRequest12));

    // UsernameToken 原始字段照抄出来，不做校验
    QVERIFY(req.hasSecurity);
    QCOMPARE(req.username, QStringLiteral("admin"));
    QVERIFY(req.passwordIsDigest);
    QCOMPARE(req.password, QStringLiteral("tuOSpGlFlIXsozq4HFNeeGeFLEI="));
    QCOMPARE(req.nonceBase64, QStringLiteral("LKqI6G/AikKCQrN0zqZFlg=="));
    QCOMPARE(req.created, QStringLiteral("2010-09-16T07:50:45Z"));
}

void TstSoap::envelope11()
{
    const SoapRequest req = soap::parseEnvelope(QByteArray(kRequest11));
    QVERIFY2(req.parseError.isEmpty(), qPrintable(req.parseError));
    QCOMPARE(req.soapVersion, 11);
    QCOMPARE(req.bodyNamespace, QLatin1String(ns::Device));
    QCOMPARE(req.bodyName, QStringLiteral("GetDeviceInformation"));
    QVERIFY(req.action.isEmpty());
    QVERIFY(req.hasSecurity);
    QCOMPARE(req.username, QStringLiteral("operator"));
    // Type 缺省即 PasswordText
    QVERIFY(!req.passwordIsDigest);
    QCOMPARE(req.password, QStringLiteral("plain-secret"));
}

void TstSoap::envelopeVersionFromContentType()
{
    QCOMPARE(soap::soapVersionFromContentType("application/soap+xml; charset=utf-8"), 12);
    QCOMPARE(soap::soapVersionFromContentType("Application/SOAP+XML"), 12);
    QCOMPARE(soap::soapVersionFromContentType("text/xml; charset=utf-8"), 11);
    QCOMPARE(soap::soapVersionFromContentType("application/octet-stream"), 12);
    QCOMPARE(QByteArray(soap::contentTypeForVersion(11)), QByteArray("text/xml; charset=utf-8"));
    QCOMPARE(QByteArray(soap::contentTypeForVersion(12)),
             QByteArray("application/soap+xml; charset=utf-8"));

    // Content-Type 只是提示：报文是 1.1 信封就按 1.1 走
    QCOMPARE(soap::parseEnvelope(QByteArray(kRequest11)).soapVersion, 11);
}

void TstSoap::envelopeErrors()
{
    QVERIFY(!soap::parseEnvelope(QByteArray("<a><b></a>")).parseError.isEmpty());
    QVERIFY(!soap::parseEnvelope(QByteArray("<Body/>")).parseError.isEmpty());

    const QByteArray unknownNs = "<e:Envelope xmlns:e=\"urn:nope\"><e:Body/></e:Envelope>";
    QVERIFY(!soap::parseEnvelope(unknownNs).parseError.isEmpty());

    const QByteArray emptyBody =
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Body/></s:Envelope>";
    const SoapRequest req = soap::parseEnvelope(emptyBody);
    QVERIFY(!req.parseError.isEmpty());
    QVERIFY(!req.isValid());

    const QByteArray noBody =
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Header/></s:Envelope>";
    QVERIFY(!soap::parseEnvelope(noBody).parseError.isEmpty());
}

// ------------------------------------------------------------ WS-Security

void TstSoap::passwordDigestVectors_data()
{
    QTest::addColumn<QString>("nonceB64");
    QTest::addColumn<QString>("created");
    QTest::addColumn<QString>("password");
    QTest::addColumn<QString>("digest");

    // ONVIF 规范里的 UsernameToken 示例，向量取自规范原文。
    QTest::newRow("onvif-spec") << "LKqI6G/AikKCQrN0zqZFlg==" << "2010-09-16T07:50:45Z"
                               << "userpassword" << "tuOSpGlFlIXsozq4HFNeeGeFLEI=";
    // 自造向量，用 openssl 独立算过一遍。
    QTest::newRow("ascii-nonce") << "MTIzNDU2Nzg5MDEyMzQ1Ng==" << "2024-05-17T09:15:23Z"
                                << "P@ssw0rd" << "CNIlz9VLgINQAcZ4V8lUezv6Dbg=";
    // 退化情形：nonce 与 created 都为空，等价于 SHA1(password)。
    QTest::newRow("empty-nonce") << QString() << QString() << "admin"
                                << "0DPiKuNIrrVmD8IUCuw1hQxNqZc=";
}

void TstSoap::passwordDigestVectors()
{
    QFETCH(QString, nonceB64);
    QFETCH(QString, created);
    QFETCH(QString, password);
    QFETCH(QString, digest);

    // nonce 参与摘要的是 Base64 解码后的原始字节
    const QByteArray nonce = QByteArray::fromBase64(nonceB64.toLatin1());
    QCOMPARE(WsSecurity::computePasswordDigest(nonce, created, password), digest);
    // 拿 Base64 串本身去算是最常见的实现错误，必须算不出同一个值
    if (!nonceB64.isEmpty()) {
        QVERIFY(WsSecurity::computePasswordDigest(nonceB64.toLatin1(), created, password)
                != digest);
    }
}

void TstSoap::verifyDigestAndText()
{
    const CameraModel model = makeModel();
    const Quirks quirks;
    const QString created = QStringLiteral("2024-05-17T09:15:23Z");
    const QDateTime now = utc(created);
    WsSecurity sec;

    const WsSecurityResult digest =
        sec.verify(makeToken(QStringLiteral("admin"), QStringLiteral("admin-pw"), true,
                             QStringLiteral("MTIzNDU2Nzg5MDEyMzQ1Ng=="), created),
                   model, quirks, now);
    QVERIFY2(digest.authenticated, qPrintable(digest.failureReason));
    QVERIFY(!digest.anonymous);
    QCOMPARE(digest.username, QStringLiteral("admin"));
    QVERIFY(digest.level == UserLevel::Administrator);

    const WsSecurityResult text =
        sec.verify(makeToken(QStringLiteral("guest"), QStringLiteral("guest-pw"), false,
                             QString(), created),
                   model, quirks, now);
    QVERIFY2(text.authenticated, qPrintable(text.failureReason));
    QVERIFY(text.level == UserLevel::User);

    // 没带 Security 头 = 匿名，不是失败
    SoapRequest bare;
    const WsSecurityResult anon = sec.verify(bare, model, quirks, now);
    QVERIFY(!anon.authenticated);
    QVERIFY(anon.anonymous);
    QVERIFY(anon.level == UserLevel::Anonymous);
}

void TstSoap::verifyRejects()
{
    const CameraModel model = makeModel();
    const Quirks quirks;
    const QString created = QStringLiteral("2024-05-17T09:15:23Z");
    const QDateTime now = utc(created);
    const QString nonce = QStringLiteral("MTIzNDU2Nzg5MDEyMzQ1Ng==");
    WsSecurity sec;

    const WsSecurityResult badDigest =
        sec.verify(makeToken(QStringLiteral("admin"), QStringLiteral("wrong"), true, nonce, created),
                   model, quirks, now);
    QVERIFY(!badDigest.authenticated);
    QVERIFY(!badDigest.anonymous);
    QVERIFY(!badDigest.failureReason.isEmpty());

    const WsSecurityResult badText =
        sec.verify(makeToken(QStringLiteral("guest"), QStringLiteral("wrong"), false, QString(),
                             created),
                   model, quirks, now);
    QVERIFY(!badText.authenticated);

    const WsSecurityResult unknownUser =
        sec.verify(makeToken(QStringLiteral("nobody"), QStringLiteral("x"), true, nonce, created),
                   model, quirks, now);
    QVERIFY(!unknownUser.authenticated);

    SoapRequest malformed =
        makeToken(QStringLiteral("admin"), QStringLiteral("admin-pw"), true, nonce, created);
    malformed.created = QStringLiteral("yesterday");
    QVERIFY(!sec.verify(malformed, model, quirks, now).authenticated);
}

void TstSoap::verifyTimeWindow()
{
    const CameraModel model = makeModel();
    const QString created = QStringLiteral("2024-05-17T09:15:23Z");
    const QString nonce = QStringLiteral("MTIzNDU2Nzg5MDEyMzQ1Ng==");
    const SoapRequest req =
        makeToken(QStringLiteral("admin"), QStringLiteral("admin-pw"), true, nonce, created);
    WsSecurity sec;

    // 默认宽松窗口 ±300s：设备钟偏 60 秒照样过
    const Quirks relaxed;
    QVERIFY(sec.verify(req, model, relaxed, utc(created).addSecs(60)).authenticated);
    QVERIFY(sec.verify(req, model, relaxed, utc(created).addSecs(-60)).authenticated);
    QVERIFY(!sec.verify(req, model, relaxed, utc(created).addSecs(3600)).authenticated);

    // A8：窗口收紧到 5 秒，客户端又不做时钟补偿 → 全线失败
    Quirks tight;
    tight.setEnabled(QuirkId::AuthTightTimeWindow, true);
    tight.setParam(QuirkId::AuthTightTimeWindow, QStringLiteral("seconds"), 5);
    QVERIFY(sec.verify(req, model, tight, utc(created).addSecs(3)).authenticated);
    QVERIFY(!sec.verify(req, model, tight, utc(created).addSecs(60)).authenticated);
    QVERIFY(!sec.verify(req, model, tight, utc(created).addSecs(-60)).authenticated);

    // 不带时区的 Created 按 UTC 认
    SoapRequest noZone =
        makeToken(QStringLiteral("admin"), QStringLiteral("admin-pw"), true, nonce,
                  QStringLiteral("2024-05-17T09:15:23"));
    QVERIFY(sec.verify(noZone, model, relaxed, utc(created)).authenticated);
}

void TstSoap::verifyPasswordTextOnlyQuirk()
{
    const CameraModel model = makeModel();
    const QString created = QStringLiteral("2024-05-17T09:15:23Z");
    const QDateTime now = utc(created);
    Quirks quirks;
    quirks.setEnabled(QuirkId::AuthPasswordTextOnly, true);
    WsSecurity sec;

    const SoapRequest digestReq =
        makeToken(QStringLiteral("admin"), QStringLiteral("admin-pw"), true,
                  QStringLiteral("MTIzNDU2Nzg5MDEyMzQ1Ng=="), created);
    QVERIFY(!sec.verify(digestReq, model, quirks, now).authenticated);

    // 同一台相机上 PasswordText 仍然放行
    const SoapRequest textReq =
        makeToken(QStringLiteral("admin"), QStringLiteral("admin-pw"), false, QString(), created);
    QVERIFY(sec.verify(textReq, model, quirks, now).authenticated);
}

void TstSoap::verifyNonceStrictOnce()
{
    const CameraModel model = makeModel();
    const QString created = QStringLiteral("2024-05-17T09:15:23Z");
    const QDateTime now = utc(created);
    Quirks quirks;
    quirks.setEnabled(QuirkId::AuthNonceStrictOnce, true);
    WsSecurity sec;

    const SoapRequest req =
        makeToken(QStringLiteral("admin"), QStringLiteral("admin-pw"), true,
                  QStringLiteral("MTIzNDU2Nzg5MDEyMzQ1Ng=="), created);
    QVERIFY(sec.verify(req, model, quirks, now).authenticated);
    QCOMPARE(sec.nonceCacheSize(), 1);
    const WsSecurityResult replay = sec.verify(req, model, quirks, now);
    QVERIFY(!replay.authenticated);
    QVERIFY(!replay.failureReason.isEmpty());

    // 换 nonce 就好
    const SoapRequest fresh =
        makeToken(QStringLiteral("admin"), QStringLiteral("admin-pw"), true,
                  QStringLiteral("Nzg5MDEyMzQ1NjEyMzQ1Ng=="), created);
    QVERIFY(sec.verify(fresh, model, quirks, now).authenticated);
    QCOMPARE(sec.nonceCacheSize(), 2);

    sec.clearNonceCache();
    QCOMPARE(sec.nonceCacheSize(), 0);
    QVERIFY(sec.verify(req, model, quirks, now).authenticated);

    // quirk 没开时不记 nonce，重放也放行
    WsSecurity loose;
    const Quirks off;
    QVERIFY(loose.verify(req, model, off, now).authenticated);
    QVERIFY(loose.verify(req, model, off, now).authenticated);
    QCOMPARE(loose.nonceCacheSize(), 0);
}

void TstSoap::levelGating()
{
    QVERIFY(WsSecurity::levelSatisfies(UserLevel::Administrator, AuthLevel::Administrator));
    QVERIFY(WsSecurity::levelSatisfies(UserLevel::Administrator, AuthLevel::Operator));
    QVERIFY(WsSecurity::levelSatisfies(UserLevel::Operator, AuthLevel::User));
    QVERIFY(WsSecurity::levelSatisfies(UserLevel::User, AuthLevel::User));
    // PreAuth 允许匿名
    QVERIFY(WsSecurity::levelSatisfies(UserLevel::Anonymous, AuthLevel::PreAuth));
    QVERIFY(WsSecurity::levelSatisfies(UserLevel::Administrator, AuthLevel::PreAuth));

    QVERIFY(!WsSecurity::levelSatisfies(UserLevel::Operator, AuthLevel::Administrator));
    QVERIFY(!WsSecurity::levelSatisfies(UserLevel::User, AuthLevel::Operator));
    QVERIFY(!WsSecurity::levelSatisfies(UserLevel::Anonymous, AuthLevel::User));
}

// ------------------------------------------------------------------ Fault

void TstSoap::faultWording_data()
{
    QTest::addColumn<QString>("wording");
    QTest::addColumn<bool>("recognizable");

    QTest::newRow("NotAuthorized") << "NotAuthorized" << true;
    QTest::newRow("SenderNotAuthorized") << "SenderNotAuthorized" << true;
    QTest::newRow("FailedAuthentication") << "FailedAuthentication" << true;
    QTest::newRow("Unknown") << "Unknown" << false;
}

void TstSoap::faultWording()
{
    QFETCH(QString, wording);
    QFETCH(bool, recognizable);

    Quirks quirks;
    quirks.setEnabled(QuirkId::AuthFaultWording, true);
    quirks.setParam(QuirkId::AuthFaultWording, QStringLiteral("value"), wording);

    const SoapFault fault = soap::authFault(quirks);
    QVERIFY(!fault.reason.isEmpty());
    QVERIFY(fault.senderFault);

    for (const int version : { 11, 12 }) {
        const QByteArray body = soap::makeFault(version, fault);
        QString error;
        xml::parse(body, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        // 四档措辞的差别就在客户端认不认得出这是认证错误
        QCOMPARE(containsAuthKeyword(body), recognizable);
    }
}

void TstSoap::faultDefaultWording()
{
    // quirk 没开 = 规范做法
    const Quirks quirks;
    const SoapFault fault = soap::authFault(quirks);
    QCOMPARE(fault.subcode, QLatin1String(ter::NotAuthorized));
    QVERIFY(containsAuthKeyword(soap::makeFault(12, fault)));
}

void TstSoap::faultHttpStatus()
{
    const SoapFault fault = soap::authFault(Quirks());

    const Quirks none;
    QCOMPARE(soap::faultHttpStatus(none, fault), 500);

    Quirks q200;
    q200.setEnabled(QuirkId::FaultHttpStatus, true);
    q200.setParam(QuirkId::FaultHttpStatus, QStringLiteral("value"), QStringLiteral("200"));
    QCOMPARE(soap::faultHttpStatus(q200, fault), 200);

    Quirks q400;
    q400.setEnabled(QuirkId::FaultHttpStatus, true);
    q400.setParam(QuirkId::FaultHttpStatus, QStringLiteral("value"), QStringLiteral("400"));
    QCOMPARE(soap::faultHttpStatus(q400, fault), 400);

    // handler 显式指定的状态码优先于 quirk
    SoapFault explicitStatus = fault;
    explicitStatus.httpStatus = 503;
    QCOMPARE(soap::faultHttpStatus(q200, explicitStatus), 503);
}

void TstSoap::faultStructure12()
{
    SoapFault fault;
    fault.subcode = QString::fromLatin1(ter::InvalidArgVal);
    fault.reason = QStringLiteral("Invalid Argument Value");
    fault.detail = QStringLiteral("ProfileToken");

    const QByteArray body = soap::makeFault(12, fault);
    QString error;
    const XmlNode root = xml::parse(body, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(root.ns, QLatin1String(ns::Soap12));
    QCOMPARE(root.path(QStringLiteral("Body/Fault/Code/Value"))->text, QStringLiteral("s:Sender"));
    QCOMPARE(root.path(QStringLiteral("Body/Fault/Code/Subcode/Value"))->text,
             QStringLiteral("ter:InvalidArgVal"));
    QCOMPARE(root.path(QStringLiteral("Body/Fault/Reason/Text"))->text, fault.reason);
    QCOMPARE(root.path(QStringLiteral("Body/Fault/Detail/Text"))->text, fault.detail);
    QVERIFY(body.contains("xmlns:ter=\"http://www.onvif.org/ver10/error\""));

    fault.senderFault = false;
    QCOMPARE(xml::parse(soap::makeFault(12, fault)).path(QStringLiteral("Body/Fault/Code/Value"))
                 ->text,
             QStringLiteral("s:Receiver"));

    // wsse 子码要连着 wsse 前缀一起声明，否则响应自己就不合法
    Quirks quirks;
    quirks.setEnabled(QuirkId::AuthFaultWording, true);
    quirks.setParam(QuirkId::AuthFaultWording, QStringLiteral("value"),
                    QStringLiteral("FailedAuthentication"));
    const QByteArray wsseFault = soap::makeFault(12, soap::authFault(quirks));
    QVERIFY(wsseFault.contains("xmlns:wsse="));
    QCOMPARE(xml::parse(wsseFault).path(QStringLiteral("Body/Fault/Code/Subcode/Value"))->text,
             QStringLiteral("wsse:FailedAuthentication"));
}

void TstSoap::faultStructure11()
{
    SoapFault fault;
    fault.subcode = QString::fromLatin1(ter::NotAuthorized);
    fault.reason = QStringLiteral("Not Authorized");
    fault.detail = QStringLiteral("digest mismatch");

    const QByteArray body = soap::makeFault(11, fault);
    QString error;
    const XmlNode root = xml::parse(body, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(root.ns, QLatin1String(ns::Soap11));
    // 1.1 的 Fault 子元素无命名空间，也没有 Code/Subcode 这一层
    QCOMPARE(root.path(QStringLiteral("Body/Fault/faultcode"))->text,
             QStringLiteral("ter:NotAuthorized"));
    QCOMPARE(root.path(QStringLiteral("Body/Fault/faultstring"))->text, fault.reason);
    QCOMPARE(root.path(QStringLiteral("Body/Fault/detail"))->text, fault.detail);
    QVERIFY(!root.path(QStringLiteral("Body/Fault/Code")));
    QVERIFY(root.path(QStringLiteral("Body/Fault/faultcode"))->ns.isEmpty());
}

void TstSoap::faultHelpers()
{
    const SoapFault notSupported = soap::notSupported(QStringLiteral("GetServices"));
    QCOMPARE(notSupported.subcode, QLatin1String(ter::ActionNotSupported));
    QCOMPARE(notSupported.detail, QStringLiteral("GetServices"));

    QCOMPARE(soap::invalidArg(QStringLiteral("Speed")).subcode, QLatin1String(ter::InvalidArgVal));
    QCOMPARE(soap::noProfile(QStringLiteral("Profile_9")).subcode, QLatin1String(ter::NoProfile));
    QCOMPARE(soap::noProfile(QStringLiteral("Profile_9")).detail, QStringLiteral("Profile_9"));

    // 默认子码是 ActionNotSupported，httpStatus 0 表示「按 quirk 决定」
    const SoapFault fresh;
    QCOMPARE(fresh.subcode, QLatin1String(ter::ActionNotSupported));
    QCOMPARE(fresh.httpStatus, 0);
    QVERIFY(fresh.senderFault);
}

// ------------------------------------------------------------- Namespaces

void TstSoap::serviceShortNames()
{
    QCOMPARE(QByteArray(ns::serviceShortName(ns::Device)), QByteArray("device"));
    QCOMPARE(QByteArray(ns::serviceShortName(ns::Media)), QByteArray("media"));
    QCOMPARE(QByteArray(ns::serviceShortName(ns::Media2)), QByteArray("media2"));
    QCOMPARE(QByteArray(ns::serviceShortName(ns::Ptz)), QByteArray("ptz"));
    QCOMPARE(QByteArray(ns::serviceShortName(ns::Imaging)), QByteArray("imaging"));
    QCOMPARE(QByteArray(ns::serviceShortName(ns::Events)), QByteArray("events"));
    QCOMPARE(QByteArray(ns::serviceShortName(ns::Analytics)), QByteArray("analytics"));
    QCOMPARE(QByteArray(ns::serviceShortName(ns::DeviceIo)), QByteArray("deviceio"));
    QCOMPARE(QByteArray(ns::serviceShortName("urn:unknown")), QByteArray(""));
    QCOMPARE(QByteArray(ns::serviceShortName(nullptr)), QByteArray(""));
}

QTEST_GUILESS_MAIN(TstSoap)

#include "tst_soap.moc"
