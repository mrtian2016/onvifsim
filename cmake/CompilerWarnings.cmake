include(CheckCXXCompilerFlag)

# Qt 的 QSKIP / QVERIFY2 这类宏声明成了可变参数，而正常用法只传一个消息。
# C++17 下「可变参数一个都不传」是 GNU 扩展（C++20 才标准化），于是 clang 在
# -Wpedantic 下会对每一处 QSKIP 报一条 —— 报的是 Qt 头文件的写法，不是调用方的
# 错，开了 -Werror 就直接编不过。
#
# **同一条诊断在不同 clang 上叫不同名字**，而且互相不认：
#   clang 16+        -Wvariadic-macro-arguments-omitted
#   clang 15 及更早   -Wgnu-zero-variadic-macro-arguments
# 给 clang 15 传前者，`-Wunknown-warning-option` 本身就在 -Wall 里，配 -Werror
# 直接满屏 `error: unknown warning option`；只传后者又管不住 clang 16+。
#
# 所以两个都探测，认哪个加哪个 —— **不要按编译器 ID 或版本号猜**。
# 这两个坑是连着踩的：先是按 ID 无条件加了新名字，Xcode 15.4 上整个 core 编不过；
# 改成探测之后新名字被正确跳过，结果 clang 15 又用老名字把同一条报了出来。
foreach(_flag -Wno-variadic-macro-arguments-omitted
              -Wno-gnu-zero-variadic-macro-arguments)
    string(MAKE_C_IDENTIFIER "ONVIFSIM_HAVE_${_flag}" _have_var)
    check_cxx_compiler_flag(${_flag} ${_have_var})
    if(${_have_var})
        list(APPEND ONVIFSIM_QUIET_VARIADIC_MACRO_FLAGS ${_flag})
    endif()
endforeach()

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

        # 探测之外还要判编译器：GCC 对不认识的 `-Wno-*` 是默默接受的（只有别处
        # 真报了诊断时才回头说一句「这个选项没认出来」），探测在它上面会假阳性。
        # 而这条诊断本来就只有 clang 会发。
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE
                ${ONVIFSIM_QUIET_VARIADIC_MACRO_FLAGS})
        endif()

        if(ENABLE_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
