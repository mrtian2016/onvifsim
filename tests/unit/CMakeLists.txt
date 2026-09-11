# 每个模块一个测试二进制：tests/unit/tst_<module>.cpp
#
# onvifsim-core 是 PUBLIC 链接 Qt6::Gui 的（快照要用 QImage/QPainter），
# 于是 QTEST_MAIN 会展开成 QGuiApplication，在没有显示器的 CI / 容器里直接 abort。
# 统一给测试设 offscreen 平台：既让 QTEST_MAIN 能跑，也让快照测试拿得到字体，
# 比逐个文件改成 QTEST_GUILESS_MAIN 更省事，也不牺牲对渲染路径的覆盖。
function(onvifsim_add_test name)
    qt_add_executable(${name} ${name}.cpp ${ARGN})
    target_link_libraries(${name} PRIVATE onvifsim-core Qt6::Test)
    onvifsim_set_warnings(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

onvifsim_add_test(tst_soap)

onvifsim_add_test(tst_media)

onvifsim_add_test(tst_ptz)

onvifsim_add_test(tst_events)

onvifsim_add_test(tst_net)

onvifsim_add_test(tst_discovery)

onvifsim_add_test(tst_device_service)

onvifsim_add_test(tst_media_service)

onvifsim_add_test(tst_ptz_service)

onvifsim_add_test(tst_events_service)

onvifsim_add_test(tst_rtsp)

onvifsim_add_test(tst_vendor)

onvifsim_add_test(tst_model)

# 翻译覆盖：要读构建出来的 .qm（由 src/gui 的 onvifsim-translations 产出），
# 还要扫 src/gui 的源码，所以 BUILD_GUI=OFF 时整条都不成立 —— 少了这个条件，
# 无界面构建会在 add_dependencies 上直接报「目标不存在」。
if(BUILD_GUI AND Qt6LinguistTools_FOUND)
    onvifsim_add_test(tst_i18n)
    add_dependencies(tst_i18n onvifsim-translations)
    # guiNeverShowsRawCoreStrings 要扫 src/gui 的源码，把路径编进去。
    target_compile_definitions(tst_i18n
        PRIVATE "ONVIFSIM_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
endif()

# 界面层的测试要额外链 onvifsim-gui-lib（和 Qt6::Widgets，由它 PUBLIC 传递过来）。
# 只有开了 BUILD_GUI 才有这个目标。
if(BUILD_GUI)
    onvifsim_add_test(tst_gui_icons)
    target_link_libraries(tst_gui_icons PRIVATE onvifsim-gui-lib)
endif()
