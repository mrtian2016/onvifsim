#pragma once

// 各厂商私有 API 桩的工厂。每个厂商一个 .cpp，互不干扰；
// VendorApiStub::create() 按 Persona 选一个。

namespace onvifsim {

class VendorApiStub;
class VirtualCamera;

namespace vendorapi {

VendorApiStub *createIsapi(VirtualCamera *camera);        // 海康 ISAPI
VendorApiStub *createDahuaCgi(VirtualCamera *camera);     // 大华 CGI
VendorApiStub *createReolinkJson(VirtualCamera *camera);  // Reolink JSON-RPC
VendorApiStub *createVigi(VirtualCamera *camera);         // TP-Link VIGI，20443 自签 HTTPS
VendorApiStub *createTplinkDs(VirtualCamera *camera);     // TP-Link TL-IPC /stok=<t>/ds

} // namespace vendorapi
} // namespace onvifsim
