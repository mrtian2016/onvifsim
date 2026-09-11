target_sources(onvifsim-core PRIVATE
    CameraModel.cpp
    LogBus.cpp
    Persona.cpp
    Quirks.cpp
    # 生成物：让 lupdate 扫得到 quirk 表里的字串，见 tools/make-i18n.py
    QuirkStrings.cpp
    Scenario.cpp
    Simulator.cpp
    VirtualCamera.cpp
    scenarios.qrc
)
