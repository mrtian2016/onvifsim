#pragma once

// 每个 ONVIF 服务继承 SoapService，在构造函数里用 op() 把操作注册进来。
// 一个操作一个 handler，handler 只负责往 ctx.out 写响应元素，
// 信封、Fault 包装、鉴权、日志、故障注入都由 Dispatcher 统一处理。
//
//   DeviceService::DeviceService() {
//       op("GetDeviceInformation", AuthLevel::PreAuth, [](SoapContext &ctx) { ... });
//   }

#include "core/Quirks.h"
#include "net/HttpTypes.h"
#include "soap/Envelope.h"
#include "soap/Fault.h"
#include "soap/XmlWriter.h"

#include <QtCore/QHash>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <functional>

namespace onvifsim {

class VirtualCamera;

// ONVIF 的四级权限。PreAuth 允许匿名（可被 quirk AuthPreAuthRequired 收紧）。
enum class AuthLevel { PreAuth, User, Operator, Administrator };

struct SoapContext {
    VirtualCamera *camera = nullptr;
    const SoapRequest *soap = nullptr;
    const XmlNode *body = nullptr;      // Body 首元素（= 操作元素），等价于 soap->body
    const HttpRequest *http = nullptr;
    HttpExchangePtr exchange;
    XmlWriter *out = nullptr;           // 已进入 Body，直接写操作响应元素即可

    // ---- 便捷访问 ----
    const Quirks &quirks() const;
    QString cameraId() const;

    // 回填 URI 用的对外地址。优先用请求的 Host 头，其次相机的绑定地址，
    // 再叠加 XAddrOddPort / StreamUriPlaceholderIp 等 quirk。
    QString advertisedHost() const;
    quint16 advertisedHttpPort() const;
    QString serviceUri(const QString &path) const;

    // ---- 取参数 ----
    QString arg(const QString &name, const QString &fallback = QString()) const;
    int argInt(const QString &name, int fallback = 0) const;
    bool argBool(const QString &name, bool fallback = false) const;
    bool hasArg(const QString &name) const;
    const XmlNode *argNode(const QString &name) const;

    // ---- 结果 ----
    // 产生 Fault 并终止本次操作。handler 调用后应立即 return。
    void fault(const SoapFault &fault);
    void fault(const QString &subcode, const QString &reason);
    void faultNotSupported();
    bool hasFault() const;
    const SoapFault &pendingFault() const;

    // 声明「我会自己 respond」（PullMessages 的长轮询、C5 的畸形响应）。
    // Dispatcher 见到这个标记就不再包信封、不再回包。
    void takeOverResponse();
    bool responseTakenOver() const;

private:
    friend class SoapDispatcher;
    bool m_hasFault = false;
    bool m_takenOver = false;
    SoapFault m_fault;
};

using SoapHandler = std::function<void(SoapContext &)>;

struct SoapOperation {
    QString name;
    AuthLevel auth = AuthLevel::User;
    SoapHandler handler;
};

class SoapService
{
public:
    virtual ~SoapService();

    virtual const char *serviceNamespace() const = 0;
    virtual const char *serviceName() const = 0;   // "device" / "media" / "media2" / ...
    virtual QString defaultPath() const = 0;       // "/onvif/device_service"

    // 出现在 GetServices / GetCapabilities 里的版本号。
    virtual int versionMajor() const { return 2; }
    virtual int versionMinor() const { return 6; }

    // 该服务在 GetServices 里的 Capabilities 片段；默认不写。
    virtual void writeServiceCapabilities(SoapContext &ctx) const;

    const SoapOperation *findOperation(const QString &name) const;

protected:
    void op(const char *name, AuthLevel auth, SoapHandler handler);

    // 和 op() 一样注册一个操作，但先挡掉「没有相机」的情况。
    //
    // 生产路径上 Dispatcher 一定带着相机，这条是为了别让手工构造的 context
    // 把服务打崩。Device / PTZ / Analytics / DeviceIO 四个服务原本各写了一份
    // 逐字相同的包装。
    void cameraOp(const char *name, AuthLevel auth, SoapHandler handler);

private:
    QHash<QString, SoapOperation> m_operations;
};

// 所有服务的工厂。VirtualCamera 按能力开关挑选要装哪些。
//
// **所有权交给调用方**：返回的裸指针必须交给 SoapDispatcher::addService()，
// 由 dispatcher 在析构（或 clearServices()）时 delete。自己 new 出来又不挂上去
// 就是内存泄漏。
namespace services {
SoapService *createDevice();
SoapService *createMedia();
SoapService *createMedia2();
SoapService *createPtz();
SoapService *createImaging();
SoapService *createEvents();
SoapService *createAnalytics();
SoapService *createDeviceIo();
} // namespace services

} // namespace onvifsim
