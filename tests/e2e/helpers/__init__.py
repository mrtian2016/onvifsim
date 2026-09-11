"""e2e 测试用的辅助模块。

这些都不是测试，pytest 不会收集它们。分工：

- ``ports``      找空闲端口，让多次运行互不打架
- ``sim``        起 / 停 headless 的 onvifsim 进程
- ``control``    REST 控制面客户端
- ``soap``       裸 SOAP 客户端（带 WS-Security UsernameToken）
- ``pullpoint``  PullPoint 订阅全流程，走裸 SOAP 以便控制订阅地址
- ``rtsp``       手写的最小 RTSP / RTP 客户端（不引第三方流媒体库）
- ``wsd``        裸 WS-Discovery Probe，用来看未经库过滤的原始应答
- ``onvifclient`` onvif-zeep 的薄封装
- ``media``      ffprobe 探流
"""
