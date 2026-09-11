include(CheckCXXCompilerFlag)

# Qt 的 QSKIP / QVERIFY2 这类宏声明成了可变参数，而正常用法只传一个消息。
# C++17 下「可变参数一个都不传」是 GNU 扩展（C++20 才标准化），于是 clang 16+
# 在 -Wpedantic 下会对每一处 QSKIP 报一条警告 —— 报的是 Qt 头文件的写法，
# 不是调用方的错，开了 -Werror 就直接编不过。
#
# **必须探测，不能按编译器 ID 判断**：这个选项名只有 clang 16+ 认得。
# Xcode 15.4（macos-14 runner 的默认 Xcode）里的 Apple clang 不认，而
# `-Wunknown-warning-option` 本身就在 -Wall 里，配上 -Werror 就是满屏
#   error: unknown warning option '-Wno-variadic-macro-arguments-omitted'
# 踩过一次：原来这里写的是 `if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")`，
# 在 Xcode 16 上好好的，一换到 Xcode 15.4 整个 core 库编不过。
check_cxx_compiler_flag(-Wno-variadic-macro-arguments-omitted
                        ONVIFSIM_HAVE_WNO_VARIADIC_MACRO_ARGUMENTS_OMITTED)

function(onvifsim_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(ENABLE_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wcast-qual -Wno-unused-parameter)

        # 探测 **加上** 编译器判断：GCC 对不认识的 `-Wno-*` 是默默接受的
        # （只有在别处真报了诊断时才回头说一句「这个选项没认出来」），
        # 所以探测在 GCC 上会假阳性。这个警告本来也只有 clang 16+ 才发。
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang"
           AND ONVIFSIM_HAVE_WNO_VARIADIC_MACRO_ARGUMENTS_OMITTED)
            target_compile_options(${target} PRIVATE
                -Wno-variadic-macro-arguments-omitted)
        endif()

        if(ENABLE_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
