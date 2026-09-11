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

        # Qt 的 QSKIP / QVERIFY2 这类宏声明成了可变参数，而正常用法只传一个消息。
        # C++17 下「可变参数一个都不传」是 GNU 扩展（C++20 才标准化），
        # 于是 clang 16+ 在 -Wpedantic 下会对每一处 QSKIP 报一条警告 ——
        # 报的是 Qt 头文件的写法，不是调用方的错，开了 -Werror 就直接编不过。
        # macOS 上的 Apple clang / conda clang 都会触发，GCC 不认这个选项名。
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE
                -Wno-variadic-macro-arguments-omitted)
        endif()

        if(ENABLE_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
