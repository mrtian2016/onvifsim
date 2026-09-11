#include "vendor/VendorApiStub.h"

#include "vendor/VendorFactories.h"

namespace onvifsim {

VendorApiStub::VendorApiStub(VirtualCamera *camera, QObject *parent)
    : QObject(parent), m_camera(camera)
{
}

VendorApiStub::~VendorApiStub() = default;

VirtualCamera *VendorApiStub::camera() const
{
    return m_camera;
}

VendorApiStub *VendorApiStub::create(VendorApi kind, VirtualCamera *camera)
{
    switch (kind) {
    case VendorApi::Isapi:       return vendorapi::createIsapi(camera);
    case VendorApi::DahuaCgi:    return vendorapi::createDahuaCgi(camera);
    case VendorApi::ReolinkJson: return vendorapi::createReolinkJson(camera);
    case VendorApi::VigiJsonRpc: return vendorapi::createVigi(camera);
    case VendorApi::TplinkDs:    return vendorapi::createTplinkDs(camera);
    case VendorApi::None:        break;
    }
    return nullptr;
}

} // namespace onvifsim
