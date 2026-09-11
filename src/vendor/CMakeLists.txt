target_sources(onvifsim-core PRIVATE
    VendorApiStub.cpp
)

add_subdirectory(hikvision)
add_subdirectory(dahua)
add_subdirectory(reolink)
add_subdirectory(vigi)
add_subdirectory(tplink)
