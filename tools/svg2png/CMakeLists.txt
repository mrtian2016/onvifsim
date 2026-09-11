# 一次性的小工具：把 assets/logo/*.svg 渲染成位图，供 tools/make-icons.sh 生成
# 各平台图标。故意不挂进主构建 —— 它只在改动标志时手工跑一次。
#
#   cmake -S tools/svg2png -B build/svg2png -DCMAKE_PREFIX_PATH="$CONDA_PREFIX"
#   cmake --build build/svg2png
#
# 用 Qt 自己的 QSvgRenderer 渲染，而不是 rsvg / Inkscape：这样看到的就是程序
# 运行时看到的。SVG 里一旦用了 Qt 不支持的特性（CSS、filter、mask），这里就会
# 直接露馅，而不是等打包完才发现图标是空的。
cmake_minimum_required(VERSION 3.21)
project(svg2png LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Gui Svg)
add_executable(svg2png main.cpp)
target_link_libraries(svg2png PRIVATE Qt6::Gui Qt6::Svg)
