#pragma once

// SOAP 分发。按 Body 首元素的 namespace + localName 找服务和操作。
//
// 关键点：Media(ver10) 与 Media2(ver20) 的命名空间都含 "/media/"，
// 必须按完整命名空间串精确匹配，不能按子串（quirk A6 就是拿这个做文章）。
//
// 完整链路：鉴权 → 权限门控 → 找操作 → 调 handler → 包信封 / 包 Fault →
// 应用响应侧 quirk（延迟、慢发、畸形）→ 回包 → 发日志。

#include "net/HttpTypes.h"
#include "services/ServiceBase.h"

#include <QtCore/QtGlobal>
#include <QtCore/QList>
#include <QtCore/QString>

namespace onvifsim {

class Quirks;
class VirtualCamera;

namespace soap {

// 把响应侧的 quirk 一次性贴到 HttpResponse 上（延迟、慢发、超大、畸形、PTZ 抖动）。
// Dispatcher 每次回包前都会调它；订阅端点那种自己解信封、不走 Dispatcher 的路径
// 也要调一次，否则那条链路上所有传输层 quirk 都会失效。
// bodyNamespace 用来做按服务生效的判断（C10 只对 PTZ 抖动）。
void applyResponseQuirks(const Quirks &quirks, HttpResponse &response,
                         const HttpRequest &request, const QString &bodyNamespace = QString());

} // namespace soap

class SoapDispatcher
{
public:
    explicit SoapDispatcher(VirtualCamera *camera);
    ~SoapDispatcher();

    // 服务所有权归 dispatcher。
    void addService(SoapService *service);
    // 卸掉当前挂的全部服务（连同所有权一起销毁）。相机的能力开关在运行时被改了
    // 之后，服务集合要跟着重建 —— 否则「关掉 PTZ」这个动作只改了模型，
    // PtzService 还挂在那儿照常接受指令。
    void clearServices();
    SoapService *serviceByNamespace(const QString &ns) const;
    SoapService *serviceByName(const QString &name) const;
    QList<SoapService *> services() const;

    // 处理一次 SOAP 请求。已经拿到 HttpExchange，回包由本函数负责
    // （除非 handler 调了 takeOverResponse）。
    void handle(const HttpRequest &request, const HttpExchangePtr &exchange);

    // 供 Device 服务的 GetServices / GetCapabilities 用：
    // 各服务对外宣称的 XAddr，顺序受 quirk A6 影响。
    QList<SoapService *> servicesForAdvertisement() const;

private:
    // 裸 Private* + 析构里 delete d，默认拷贝构造会让两个对象指向同一块内存，
    // 第二次析构就是 double free —— 而编译器一个字都不会说。项目里其余 PIMPL
    // 类都继承自 QObject（天生禁拷贝），只有这几个是裸的。
    Q_DISABLE_COPY(SoapDispatcher)

    struct Private;
    Private *d;
};

} // namespace onvifsim
