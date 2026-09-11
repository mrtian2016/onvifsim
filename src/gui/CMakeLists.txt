add_library(onvifsim-gui-lib STATIC
    GuiEntry.cpp
    GuiUtil.cpp
    I18n.cpp
    MainWindow.cpp
    CameraListWidget.cpp
    LogPanel.cpp
    NetworkDialog.cpp
    widgets/PtzPadWidget.cpp
    widgets/LevelMeterWidget.cpp
    tabs/CameraTab.cpp
    tabs/OverviewTab.cpp
    tabs/IdentityTab.cpp
    tabs/StreamsTab.cpp
    tabs/PtzTab.cpp
    tabs/EventsTab.cpp
    tabs/TalkbackTab.cpp
    tabs/QuirksTab.cpp
    tabs/ConnectionsTab.cpp
    # 程序图标走 AUTORCC（顶层 CMakeLists 已经打开）。静态库里的资源不会自动
    # 初始化，GuiUtil.cpp 里有一次显式的 Q_INIT_RESOURCE(icons)。
    icons.qrc
)
target_link_libraries(onvifsim-gui-lib PUBLIC onvifsim-core Qt6::Widgets)
onvifsim_set_warnings(onvifsim-gui-lib)

# 界面字串的源语言是中文，英文由这两份翻译提供。lrelease 只把 .ts 编成 .qm，
# 不会回写 .ts —— 更新 .ts 是开发者手工跑 lupdate 的事，不放进构建里，
# 免得构建一次就把翻译文件改脏。加了新的 tr() 之后这样刷新：
#
#   lupdate -I src src/gui -locations none -no-obsolete \
#           -ts assets/i18n/onvifsim_zh_CN.ts assets/i18n/onvifsim_en.ts
#
# `-I src` 不能省：少了它 lupdate 解析不到 gui/*.h，context 会退化成不带命名空间的
# "MainWindow"，而运行期 tr() 用的是 "onvifsim::gui::MainWindow" —— 对不上，
# 译文会静默地一句都不生效。
set(ONVIFSIM_TS_FILES
    "${CMAKE_SOURCE_DIR}/assets/i18n/onvifsim_zh_CN.ts"
    "${CMAKE_SOURCE_DIR}/assets/i18n/onvifsim_en.ts")

if(Qt6LinguistTools_FOUND)
    set(ONVIFSIM_I18N_OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/i18n")
    set_source_files_properties(${ONVIFSIM_TS_FILES}
        PROPERTIES OUTPUT_LOCATION "${ONVIFSIM_I18N_OUTPUT}")
    qt6_add_translation(ONVIFSIM_QM_FILES ${ONVIFSIM_TS_FILES})
    add_custom_target(onvifsim-translations ALL DEPENDS ${ONVIFSIM_QM_FILES})
    add_dependencies(onvifsim-gui-lib onvifsim-translations)
    # 开发期直接跑构建目录里的可执行文件时，从这里找 .qm。
    target_compile_definitions(onvifsim-gui-lib
        PRIVATE "ONVIFSIM_I18N_BUILD_DIR=\"${ONVIFSIM_I18N_OUTPUT}\"")
    install(FILES ${ONVIFSIM_QM_FILES} DESTINATION share/onvifsim/i18n)
else()
    message(STATUS "  未找到 Qt LinguistTools，界面只有中文（源语言）")
endif()
