target_sources(onvifsim-core PRIVATE
    MediaTypes.cpp
    H264Source.cpp
    AudioSource.cpp
    Snapshot.cpp
    # 样片走 AUTORCC（顶层 CMakeLists 已经打开）。静态库里资源不会自动初始化，
    # H264Source.cpp 里有一次显式的 Q_INIT_RESOURCE(assets)。
    assets.qrc
)
