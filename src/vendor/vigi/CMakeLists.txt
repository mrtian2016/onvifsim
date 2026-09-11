target_sources(onvifsim-core PRIVATE
    VigiStub.cpp
    # 自签证书与私钥走 AUTORCC。静态库里资源不会自动初始化，
    # VigiStub.cpp 里有一次显式的 Q_INIT_RESOURCE(vigitls)。
    vigitls.qrc
)
