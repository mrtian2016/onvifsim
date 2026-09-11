#pragma once

// GUI 入口。GUI 只是 Simulator 的观察者，不碰任何协议细节。

#include "cli/CliOptions.h"

class QApplication;

namespace onvifsim {
namespace gui {

int run(const CliOptions &options, QApplication &app);

} // namespace gui
} // namespace onvifsim
