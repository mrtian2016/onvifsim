# onvifsim-core: all protocol logic. Depends only on Qt Core/Network/Gui.
# The GUI links this but must never reach into protocol details itself.
add_library(onvifsim-core STATIC)

target_link_libraries(onvifsim-core PUBLIC Qt6::Core Qt6::Network Qt6::Gui)
target_include_directories(onvifsim-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
onvifsim_generate_version_header(onvifsim-core)
onvifsim_set_warnings(onvifsim-core)

if(ENABLE_AUDIO_PLAYBACK)
    target_link_libraries(onvifsim-core PUBLIC Qt6::Multimedia)
    target_compile_definitions(onvifsim-core PUBLIC ONVIFSIM_AUDIO_PLAYBACK)
endif()

# Each module appends to onvifsim-core via target_sources in its own file,
# so parallel work on different modules never touches the same CMakeLists.
add_subdirectory(core)
add_subdirectory(net)
add_subdirectory(soap)
add_subdirectory(services)
add_subdirectory(events)
add_subdirectory(ptz)
add_subdirectory(imaging)
add_subdirectory(rtsp)
add_subdirectory(media)
add_subdirectory(discovery)
add_subdirectory(vendor)
add_subdirectory(control)

add_subdirectory(cli)
if(BUILD_GUI)
    add_subdirectory(gui)
endif()

# 平台图标。母版是 assets/logo/onvifsim.svg，各平台的位图由 tools/make-icons.sh
# 预先生成并入库 —— 构建期不做矢量渲染，CI 上就不用装 rsvg/Inkscape。
if(WIN32)
    # 资源管理器和任务栏读的是 exe 内嵌的图标，不是 Qt 的 setWindowIcon()：
    # 少了这个 .rc，程序跑起来标题栏有图标、文件本身却还是一张白纸。
    set(ONVIFSIM_ICON_PATH "onvifsim.ico")
    string(REPLACE "." "," ONVIFSIM_VERSION_RC "${ONVIFSIM_VERSION_SHORT}")
    string(APPEND ONVIFSIM_VERSION_RC ",0")   # VERSIONINFO 要四段
    # rc.exe 解析相对路径是相对 .rc 文件所在目录，所以把图标复制到一起，
    # 免得路径里的反斜杠 / 中文用户名之类再出幺蛾子。
    file(COPY "${CMAKE_SOURCE_DIR}/packaging/windows/onvifsim.ico"
         DESTINATION "${CMAKE_BINARY_DIR}/generated")
    configure_file("${CMAKE_SOURCE_DIR}/packaging/windows/onvifsim.rc.in"
                   "${CMAKE_BINARY_DIR}/generated/onvifsim.rc" @ONLY)
    set(ONVIFSIM_WIN_RESOURCE "${CMAKE_BINARY_DIR}/generated/onvifsim.rc")
endif()

if(APPLE)
    # Info.plist 里写了 CFBundleIconFile=onvifsim.icns，文件得真的躺在
    # Contents/Resources 里，Dock 和 Finder 才认。
    set(ONVIFSIM_MACOS_ICON "${CMAKE_SOURCE_DIR}/packaging/macos/onvifsim.icns")
    set_source_files_properties("${ONVIFSIM_MACOS_ICON}"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
endif()

# Single entry point: compiles into the GUI when built with it, else headless.
if(BUILD_GUI)
    qt_add_executable(onvifsim main.cpp ${ONVIFSIM_WIN_RESOURCE} ${ONVIFSIM_MACOS_ICON})
    target_link_libraries(onvifsim PRIVATE onvifsim-core onvifsim-cli-lib onvifsim-gui-lib)
    target_compile_definitions(onvifsim PRIVATE ONVIFSIM_HAVE_GUI)
    set_target_properties(onvifsim PROPERTIES WIN32_EXECUTABLE ON MACOSX_BUNDLE ON)
    if(APPLE)
        set_target_properties(onvifsim PROPERTIES MACOSX_BUNDLE_ICON_FILE "onvifsim.icns")
    endif()

    if(WIN32)
        # Windows 上 GUI 子系统的程序没有控制台，命令行里跑 --headless / --list-quirks
        # 看不到任何输出。额外出一个控制台子系统的 onvifsim-cli.exe：
        # 双击 onvifsim.exe 走界面、命令行里用 onvifsim-cli.exe 走无界面，各得其所。
        # 两者共用同一个 main.cpp，只是没有 ONVIFSIM_HAVE_GUI 这个宏。
        qt_add_executable(onvifsim-cli main.cpp ${ONVIFSIM_WIN_RESOURCE})
        target_link_libraries(onvifsim-cli PRIVATE onvifsim-core onvifsim-cli-lib)
        set_target_properties(onvifsim-cli PROPERTIES WIN32_EXECUTABLE OFF)
        onvifsim_set_warnings(onvifsim-cli)
    endif()
else()
    qt_add_executable(onvifsim main.cpp ${ONVIFSIM_WIN_RESOURCE})
    target_link_libraries(onvifsim PRIVATE onvifsim-core onvifsim-cli-lib)
endif()
onvifsim_set_warnings(onvifsim)
