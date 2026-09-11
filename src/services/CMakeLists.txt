target_sources(onvifsim-core PRIVATE
    ServiceBase.cpp
    ProfileQuirks.cpp
)

add_subdirectory(device)
add_subdirectory(media)
add_subdirectory(media2)
add_subdirectory(ptz)
add_subdirectory(imaging)
add_subdirectory(events)
add_subdirectory(analytics)
add_subdirectory(deviceio)
