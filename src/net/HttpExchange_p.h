#pragma once

// HttpExchange 的内部状态。只给 net/ 里的实现文件用（HttpTypes.cpp 与 HttpServer.cpp
// 都要碰它：前者实现 respond()，后者的 HttpConnection 负责装填与在断连时置位），
// 所以不能藏在某一个 .cpp 里。公开头 HttpTypes.h 只留一个 Private 前向声明。

#include "net/HttpTypes.h"

#include <functional>

namespace onvifsim {

struct HttpExchange::Private
{
    HttpRequest request;
    bool responded = false;
    bool streaming = false;
    bool connected = true;
    // 全都由 HttpConnection 装填；连接销毁或客户端断开时一并清空，
    // 这样 vendor 桩长期攥着的 HttpExchangePtr 晚一步调用也只是空转，
    // 不会写到已经没了的 socket 上。
    std::function<void(const HttpResponse &)> deliver;
    std::function<void(const HttpResponse &)> beginStream;
    std::function<bool(const QByteArray &)> writeChunk;
    std::function<void()> endStream;

    void detach()
    {
        deliver = nullptr;
        beginStream = nullptr;
        writeChunk = nullptr;
        endStream = nullptr;
    }
};

} // namespace onvifsim
