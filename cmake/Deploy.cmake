# Qt 部署工具的封装：windeployqt / macdeployqt / linuxdeploy。
#
# 两种用法：
#
# 1. 当模块用（在 CMakeLists.txt 里）：
#      list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
#      include(Deploy)
#      onvifsim_deploy_qt(onvifsim)          # 加一个 onvifsim-deploy 目标
#
# 2. 当脚本用（构建完之后跑，不需要改 CMakeLists.txt）：
#      cmake -DONVIFSIM_DEPLOY_BINARY=build/release/bin/onvifsim \
#            -DONVIFSIM_DEPLOY_DIR=dist/onvifsim \
#            -P cmake/Deploy.cmake
#
# packaging/ 下三个平台的脚本走的都是第 2 种，所以它们不依赖构建树的配置。

# ---------------------------------------------------------------------------
# 找部署工具。三个平台的工具都在 Qt 的 bin 目录里（linuxdeploy 除外，它是外部工具）。
# ---------------------------------------------------------------------------
# GET_RUNTIME_DEPENDENCIES 在新策略下会先归一化路径再匹配排除规则。
# 不显式设的话 CMake 4.x 会为每个依赖刷一条 author warning，把真正的错误淹掉。
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

function(onvifsim_find_deploy_tool out_var name)
    set(_hints)
    if(TARGET Qt6::qmake)
        get_target_property(_qmake Qt6::qmake IMPORTED_LOCATION)
        if(_qmake)
            get_filename_component(_qt_bin "${_qmake}" DIRECTORY)
            list(APPEND _hints "${_qt_bin}")
        endif()
    endif()
    if(DEFINED ENV{CONDA_PREFIX})
        list(APPEND _hints "$ENV{CONDA_PREFIX}/bin" "$ENV{CONDA_PREFIX}/Library/bin")
    endif()
    if(DEFINED ENV{QTDIR})
        list(APPEND _hints "$ENV{QTDIR}/bin")
    endif()
    find_program(${out_var} NAMES ${name} ${ARGN} HINTS ${_hints})
    set(${out_var} "${${out_var}}" PARENT_SCOPE)
endfunction()

# Qt 的插件目录在哪。先问 qtpaths，问不到就按部署工具的位置往上猜几个常见布局
# （conda-forge 是 <prefix>/lib/qt6/plugins，官方安装器是 <prefix>/plugins）。
function(onvifsim_qt_plugin_root out_var deploy_tool)
    set(_root "")
    onvifsim_find_deploy_tool(ONVIFSIM_QTPATHS qtpaths6 qtpaths)
    if(ONVIFSIM_QTPATHS)
        execute_process(COMMAND "${ONVIFSIM_QTPATHS}" --query QT_INSTALL_PLUGINS
                        OUTPUT_VARIABLE _root
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET)
    endif()
    if(NOT _root OR NOT EXISTS "${_root}")
        get_filename_component(_qt_bin "${deploy_tool}" DIRECTORY)
        get_filename_component(_qt_prefix "${_qt_bin}" DIRECTORY)
        set(_root "")
        foreach(_candidate "${_qt_prefix}/lib/qt6/plugins"
                           "${_qt_prefix}/plugins"
                           "${_qt_prefix}/share/qt6/plugins")
            if(EXISTS "${_candidate}")
                set(_root "${_candidate}")
                break()
            endif()
        endforeach()
    endif()
    set(${out_var} "${_root}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# Windows：windeployqt 把 Qt 的 DLL、平台插件、样式插件拷到可执行文件旁边。
# 结果是一个解压即跑的便携目录。
# ---------------------------------------------------------------------------
function(onvifsim_deploy_windows binary destination)
    onvifsim_find_deploy_tool(ONVIFSIM_WINDEPLOYQT windeployqt6 windeployqt)
    if(NOT ONVIFSIM_WINDEPLOYQT)
        message(FATAL_ERROR "找不到 windeployqt（在 Qt 的 bin 目录里）")
    endif()
    message(STATUS "windeployqt: ${ONVIFSIM_WINDEPLOYQT}")

    # windeployqt 靠 qtpaths 问 Qt 的安装布局，默认找的是不带版本后缀的
    # qtpaths.exe。conda-forge 的 Qt6 只提供 qtpaths6.exe，不显式指出来
    # 就会报 "Unable to query qtpaths"。官方安装器两个名字都有，所以只在
    # 找得到 qtpaths6 时才传这个参数。
    onvifsim_find_deploy_tool(ONVIFSIM_QTPATHS qtpaths6 qtpaths)
    set(_qtpaths_arg "")
    if(ONVIFSIM_QTPATHS)
        message(STATUS "qtpaths: ${ONVIFSIM_QTPATHS}")
        set(_qtpaths_arg --qtpaths "${ONVIFSIM_QTPATHS}")
    endif()

    execute_process(
        COMMAND "${ONVIFSIM_WINDEPLOYQT}"
                ${_qtpaths_arg}
                --dir "${destination}"
                --release
                --no-translations
                --no-system-d3d-compiler
                --no-opengl-sw
                --no-compiler-runtime
                "${binary}"
        RESULT_VARIABLE _result)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "windeployqt 失败：${_result}")
    endif()

    onvifsim_copy_windows_platform_plugins("${destination}")
    onvifsim_prune_unused_image_plugins("${destination}")
    onvifsim_copy_windows_runtime_deps("${binary}" "${destination}")
endfunction()

# 无界面模式跑的是 QGuiApplication + offscreen 平台插件（快照要 QPainter 画字，
# QCoreApplication 下 QFontDatabase 直接 abort）。但 windeployqt 只拷它认为
# 图形程序需要的 qwindows.dll —— 少了 offscreen，命令行版一启动就是
# "Could not find the Qt platform plugin offscreen"，而且这个错只在
# 真正跑 --headless 时才暴露，--version 那条路径不碰它。
function(onvifsim_copy_windows_platform_plugins destination)
    onvifsim_qt_plugin_root(_plugin_root "${ONVIFSIM_WINDEPLOYQT}")

    # qminimal 一并带上：某些受限环境（无窗口站的服务账号）连 offscreen 都起不来时还能兜底。
    foreach(_plugin qoffscreen qminimal)
        set(_src "${_plugin_root}/platforms/${_plugin}.dll")
        if(EXISTS "${_src}" AND NOT EXISTS "${destination}/platforms/${_plugin}.dll")
            file(COPY "${_src}" DESTINATION "${destination}/platforms")
            message(STATUS "  补平台插件: ${_plugin}.dll")
        endif()
    endforeach()
endfunction()

# windeployqt 把 imageformats 下的插件一股脑全拷（tiff / webp / gif / icns / tga …），
# 每个还各自拖一串第三方 DLL 进来。本项目只写 JPEG（快照）、只读内嵌的 PNG 图标，
# 其余格式一个都用不上。趁依赖分析之前删掉，那些 DLL 也就不会被拷进来。
function(onvifsim_prune_unused_image_plugins destination)
    set(_keep qjpeg qico qsvg)
    file(GLOB _plugins "${destination}/imageformats/*.dll")
    foreach(_plugin IN LISTS _plugins)
        get_filename_component(_name "${_plugin}" NAME_WE)
        list(FIND _keep "${_name}" _idx)
        if(_idx EQUAL -1)
            file(REMOVE "${_plugin}")
            message(STATUS "  剔除图片插件: ${_name}.dll")
        endif()
    endforeach()
endfunction()

# conda-forge 的 Qt6 链接了一批**非 Qt** 的第三方 DLL（zlib / pcre2-16 / zstd /
# double-conversion / brotli / freetype / harfbuzz / libpng …），而 windeployqt
# 只认 Qt 自己的那些依赖，这些一个都不会拷。结果就是包在没装 conda 的机器上
# 一启动就 0xC0000135（找不到 DLL）—— 而且报错里不会说是哪个。
#
# 用 CMake 内置的 GET_RUNTIME_DEPENDENCIES 递归解析导入表，把能在 Qt 的 bin
# 目录里找到的全补进来。系统 DLL（System32 / api-ms-win-*）排除掉。
function(onvifsim_copy_windows_runtime_deps binary destination)
    set(_dirs)
    if(DEFINED ENV{CONDA_PREFIX})
        list(APPEND _dirs "$ENV{CONDA_PREFIX}/Library/bin" "$ENV{CONDA_PREFIX}/bin")
    endif()
    if(DEFINED ENV{QTDIR})
        list(APPEND _dirs "$ENV{QTDIR}/bin")
    endif()
    get_filename_component(_qt_bin "${ONVIFSIM_WINDEPLOYQT}" DIRECTORY)
    list(APPEND _dirs "${_qt_bin}")

    # MSVC 运行时：VS 的 redist 目录。系统里装没装 VC++ Redistributable 不好说，
    # 打进包里最省事，用户解压即跑。
    if(DEFINED ENV{VCToolsRedistDir})
        file(GLOB _crt_dirs "$ENV{VCToolsRedistDir}/x64/Microsoft.VC*.CRT")
        list(APPEND _dirs ${_crt_dirs})
    endif()

    # 已经拷进去的 Qt DLL 自己也有依赖，要一起当解析起点。
    file(GLOB_RECURSE _staged "${destination}/*.dll")

    # Qt 的 DLL 在 stage 与 conda 的 bin 里各有一份，同名会被判成「冲突」而直接报错。
    # 拿 CONFLICTING_DEPENDENCIES_PREFIX 收下这些冲突：它们全都是 windeployqt
    # 已经拷进 stage 的那批，跳过就好，我们只关心它还没管的第三方 DLL。
    file(GET_RUNTIME_DEPENDENCIES
        EXECUTABLES "${binary}"
        LIBRARIES ${_staged}
        RESOLVED_DEPENDENCIES_VAR _resolved
        UNRESOLVED_DEPENDENCIES_VAR _unresolved
        CONFLICTING_DEPENDENCIES_PREFIX _conflict
        DIRECTORIES ${_dirs}
        PRE_EXCLUDE_REGEXES "api-ms-win-.*" "ext-ms-.*"
        POST_EXCLUDE_REGEXES ".*[Ss]ystem32.*" ".*[Ss]ys[Ww][Oo][Ww]64.*")

    # 冲突项里如果有 stage 中还没有的，仍然要补一份（取第一个候选）。
    foreach(_name IN LISTS _conflict_FILENAMES)
        if(NOT EXISTS "${destination}/${_name}")
            list(GET _conflict_${_name} 0 _pick)
            file(COPY "${_pick}" DESTINATION "${destination}")
            message(STATUS "  补依赖(冲突取首个): ${_name}")
        endif()
    endforeach()

    set(_added 0)
    foreach(_dep IN LISTS _resolved)
        get_filename_component(_name "${_dep}" NAME)
        if(NOT EXISTS "${destination}/${_name}")
            file(COPY "${_dep}" DESTINATION "${destination}")
            message(STATUS "  补依赖: ${_name}")
            math(EXPR _added "${_added} + 1")
        endif()
    endforeach()
    message(STATUS "第三方依赖补了 ${_added} 个")

    if(_unresolved)
        # 解析不到的基本都是系统 DLL（KERNEL32 之类），列出来供排查。
        list(REMOVE_DUPLICATES _unresolved)
        message(STATUS "未解析（通常是系统 DLL，可忽略）：${_unresolved}")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# macOS：macdeployqt 只收它认为图形程序用得上的 cocoa 平台插件。可 `--headless`
# 会把 QT_QPA_PLATFORM 设成 offscreen（快照要 QGuiApplication + QPainter 画字），
# 少了它，.app 里的无界面模式一启动就是
#   qt.qpa.plugin: Could not find the Qt platform plugin "offscreen"
# 然后直接 abort。Windows 那边早就显式补了 qoffscreen.dll、Linux 靠
# EXTRA_PLATFORM_PLUGINS 补，**只有 macOS 这条一直漏着** —— v0.1.0 的 dmg
# 就是这么发出去的。
# ---------------------------------------------------------------------------
function(onvifsim_copy_macos_platform_plugins bundle)
    onvifsim_qt_plugin_root(_plugin_root "${ONVIFSIM_MACDEPLOYQT}")
    if(NOT _plugin_root)
        message(WARNING "问不到 Qt 插件目录，跳过补 offscreen 插件；.app 的 --headless 会起不来")
        return()
    endif()

    # qminimal 一并带上：某些受限环境里连 offscreen 都起不来时还能兜底。
    foreach(_plugin libqoffscreen libqminimal)
        set(_src "${_plugin_root}/platforms/${_plugin}.dylib")
        if(EXISTS "${_src}")
            file(COPY "${_src}" DESTINATION "${bundle}/Contents/PlugIns/platforms")
            message(STATUS "  补平台插件: ${_plugin}.dylib")
        elseif(_plugin STREQUAL "libqoffscreen")
            message(FATAL_ERROR
                "Qt 插件目录里没有 ${_src} —— 没有它 .app 的 --headless 起不来。")
        endif()
    endforeach()
endfunction()

# bundle 里的二进制只允许依赖三类路径：@rpath / @executable_path / @loader_path
# 开头的（bundle 内部）、/usr/lib 和 /System（系统自带）。剩下的一律是构建机上的
# 绝对路径 —— 本机跑没事，别人下下来就是「打不开」，而且报错里不会说是哪个库。
function(onvifsim_check_macos_bundle_paths bundle)
    find_program(ONVIFSIM_OTOOL otool)
    if(NOT ONVIFSIM_OTOOL)
        message(WARNING "找不到 otool，跳过 bundle 路径体检")
        return()
    endif()

    file(GLOB_RECURSE _binaries "${bundle}/Contents/PlugIns/*.dylib")
    get_filename_component(_bundle_name "${bundle}" NAME_WE)
    list(APPEND _binaries "${bundle}/Contents/MacOS/${_bundle_name}")

    set(_bad "")
    foreach(_bin IN LISTS _binaries)
        execute_process(COMMAND "${ONVIFSIM_OTOOL}" -L "${_bin}"
                        OUTPUT_VARIABLE _out OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        string(REPLACE "\n" ";" _lines "${_out}")
        foreach(_line IN LISTS _lines)
            # otool 的依赖行以 tab 开头；顶格的是标题行。**fat 二进制每个架构一行
            # 标题**（`<路径> (architecture arm64):`），所以不能只跳过第一行 ——
            # Qt 官方的 macOS 插件全是 x86_64 + arm64 的通用二进制，只跳一行的话
            # 第二个架构的标题会被当成依赖，整份报表全是误报。踩过。
            if(NOT _line MATCHES "^[ \t]")
                continue()
            endif()
            string(STRIP "${_line}" _line)
            string(REGEX REPLACE " \\(compatibility.*" "" _dep "${_line}")
            if(_dep STREQUAL "")
                continue()
            endif()
            if(_dep MATCHES "^@" OR _dep MATCHES "^/usr/lib/" OR _dep MATCHES "^/System/")
                continue()
            endif()
            list(APPEND _bad "${_bin}: ${_dep}")
        endforeach()
    endforeach()

    if(_bad)
        string(REPLACE ";" "\n  " _bad_text "${_bad}")
        message(FATAL_ERROR
            "bundle 里有指向构建机的绝对路径，换台机器就加载不了：\n  ${_bad_text}")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# macOS：macdeployqt 打 .app，然后 **ad-hoc 签名**。
# ad-hoc（codesign -s -）不需要开发者账号，签完本机双击就能开，
# 只是别的机器上首次打开仍要右键「打开」。没有签名的话 arm64 上直接起不来。
# ---------------------------------------------------------------------------
function(onvifsim_deploy_macos bundle)
    cmake_parse_arguments(ARG "NO_DMG" "DMG_OUTPUT" "" ${ARGN})
    onvifsim_find_deploy_tool(ONVIFSIM_MACDEPLOYQT macdeployqt6 macdeployqt)
    if(NOT ONVIFSIM_MACDEPLOYQT)
        message(FATAL_ERROR "找不到 macdeployqt（在 Qt 的 bin 目录里）")
    endif()
    message(STATUS "macdeployqt: ${ONVIFSIM_MACDEPLOYQT}")

    # **必须在 macdeployqt 之前**把 offscreen 插件放进去：macdeployqt 会把它在
    # bundle 里找到的每个二进制的依赖路径改写成 @executable_path/../Frameworks，
    # 事后再拷进去的那份不会被改写，仍然指着构建机上 conda 的绝对路径 ——
    # 在本机测一切正常，换台机器就加载不了。
    onvifsim_copy_macos_platform_plugins("${bundle}")

    set(_args "${bundle}" -always-overwrite)
    execute_process(COMMAND "${ONVIFSIM_MACDEPLOYQT}" ${_args} RESULT_VARIABLE _result)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "macdeployqt 失败：${_result}")
    endif()

    # 体检：bundle 里不该再有指向构建机的绝对路径。上面那条「先拷再 deploy」的
    # 顺序要求是靠这道检查兜底的 —— 顺序写反了本机照样跑得通，只有别人的机器会坏。
    onvifsim_check_macos_bundle_paths("${bundle}")

    # ad-hoc 签名。--deep 是刻意的：macdeployqt 塞进来的框架也要一起签，
    # 否则 Gatekeeper 会因为「签名不完整」拒绝加载。
    find_program(ONVIFSIM_CODESIGN codesign)
    if(ONVIFSIM_CODESIGN)
        execute_process(
            COMMAND "${ONVIFSIM_CODESIGN}" --force --deep --sign - "${bundle}"
            RESULT_VARIABLE _sign_result)
        if(NOT _sign_result EQUAL 0)
            message(WARNING "ad-hoc 签名失败（${_sign_result}），产物在别的机器上可能打不开")
        endif()
    else()
        message(WARNING "找不到 codesign，跳过 ad-hoc 签名")
    endif()

    if(NOT ARG_NO_DMG)
        if(NOT ARG_DMG_OUTPUT)
            get_filename_component(_dir "${bundle}" DIRECTORY)
            set(ARG_DMG_OUTPUT "${_dir}/onvifsim.dmg")
        endif()
        find_program(ONVIFSIM_HDIUTIL hdiutil)
        if(NOT ONVIFSIM_HDIUTIL)
            message(FATAL_ERROR "找不到 hdiutil")
        endif()

        # dmg 里要放一个指向 /Applications 的替身，用户拖过去就算装好了。
        # 直接 -srcfolder <bundle> 打出来的盘里只有一个 .app，用户得自己开
        # 一个 Finder 窗口找「应用程序」——每个 macOS 用户都会觉得别扭。
        get_filename_component(_dmg_dir "${ARG_DMG_OUTPUT}" DIRECTORY)
        set(_stage "${_dmg_dir}/.dmg-stage")
        file(REMOVE_RECURSE "${_stage}")
        file(MAKE_DIRECTORY "${_stage}")

        # 必须用 cp -R 而不是 file(COPY)：.app 里 Qt 框架的 Versions/Current
        # 之类全是符号链接，CMake 会把它们展开成真文件，体积翻倍不说，
        # 之前打好的签名也会当场失效。
        get_filename_component(_bundle_name "${bundle}" NAME)
        execute_process(COMMAND cp -R "${bundle}" "${_stage}/${_bundle_name}"
                        RESULT_VARIABLE _copy_result)
        if(NOT _copy_result EQUAL 0)
            message(FATAL_ERROR "复制 .app 到暂存目录失败：${_copy_result}")
        endif()
        file(CREATE_LINK "/Applications" "${_stage}/Applications" SYMBOLIC)

        file(REMOVE "${ARG_DMG_OUTPUT}")
        execute_process(
            COMMAND "${ONVIFSIM_HDIUTIL}" create -volname onvifsim
                    -srcfolder "${_stage}" -ov -format UDZO "${ARG_DMG_OUTPUT}"
            RESULT_VARIABLE _dmg_result)
        file(REMOVE_RECURSE "${_stage}")
        if(NOT _dmg_result EQUAL 0)
            message(FATAL_ERROR "hdiutil 打 dmg 失败：${_dmg_result}")
        endif()
        message(STATUS "dmg: ${ARG_DMG_OUTPUT}")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Linux：linuxdeploy + qt 插件打 AppImage。
# 工具不在系统里就现下（CI 上就是这么干的），下载地址可以用
# ONVIFSIM_LINUXDEPLOY / ONVIFSIM_LINUXDEPLOY_QT 覆盖成本地路径。
# ---------------------------------------------------------------------------
function(onvifsim_deploy_linux binary)
    cmake_parse_arguments(ARG "" "APPDIR;DESKTOP;ICON;OUTPUT" "" ${ARGN})
    if(NOT ARG_APPDIR)
        message(FATAL_ERROR "onvifsim_deploy_linux 需要 APPDIR")
    endif()

    find_program(ONVIFSIM_LINUXDEPLOY NAMES linuxdeploy linuxdeploy-x86_64.AppImage)
    find_program(ONVIFSIM_LINUXDEPLOY_QT
                 NAMES linuxdeploy-plugin-qt linuxdeploy-plugin-qt-x86_64.AppImage)
    if(NOT ONVIFSIM_LINUXDEPLOY)
        message(FATAL_ERROR
            "找不到 linuxdeploy。从 https://github.com/linuxdeploy/linuxdeploy/releases "
            "下 linuxdeploy-x86_64.AppImage，chmod +x 后放进 PATH。")
    endif()

    set(_args --appdir "${ARG_APPDIR}" --executable "${binary}")
    if(ARG_DESKTOP)
        list(APPEND _args --desktop-file "${ARG_DESKTOP}")
    endif()
    if(ARG_ICON)
        list(APPEND _args --icon-file "${ARG_ICON}")
    endif()
    if(ONVIFSIM_LINUXDEPLOY_QT)
        list(APPEND _args --plugin qt)
    else()
        message(WARNING "没有 linuxdeploy-plugin-qt，AppImage 里不会带 Qt 插件")
    endif()
    list(APPEND _args --output appimage)

    execute_process(COMMAND "${ONVIFSIM_LINUXDEPLOY}" ${_args}
                    RESULT_VARIABLE _result)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "linuxdeploy 失败：${_result}")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# 按平台分派。当模块用时给 target 挂一个 <target>-deploy 目标。
# ---------------------------------------------------------------------------
function(onvifsim_deploy_qt target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "没有这个目标：${target}")
    endif()
    if(WIN32)
        add_custom_target(${target}-deploy
            COMMAND "${CMAKE_COMMAND}"
                    -DONVIFSIM_DEPLOY_BINARY=$<TARGET_FILE:${target}>
                    -DONVIFSIM_DEPLOY_DIR=$<TARGET_FILE_DIR:${target}>
                    -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
            DEPENDS ${target}
            COMMENT "windeployqt ${target}")
    elseif(APPLE)
        add_custom_target(${target}-deploy
            COMMAND "${CMAKE_COMMAND}"
                    -DONVIFSIM_DEPLOY_BUNDLE=$<TARGET_BUNDLE_DIR:${target}>
                    -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
            DEPENDS ${target}
            COMMENT "macdeployqt + ad-hoc 签名 ${target}")
    else()
        add_custom_target(${target}-deploy
            COMMAND "${CMAKE_COMMAND}"
                    -DONVIFSIM_DEPLOY_BINARY=$<TARGET_FILE:${target}>
                    -DONVIFSIM_DEPLOY_APPDIR=${CMAKE_BINARY_DIR}/AppDir
                    -DONVIFSIM_DEPLOY_DESKTOP=${CMAKE_SOURCE_DIR}/packaging/linux/onvifsim.desktop
                    -DONVIFSIM_DEPLOY_ICON=${CMAKE_SOURCE_DIR}/packaging/linux/onvifsim.svg
                    -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
            DEPENDS ${target}
            COMMENT "linuxdeploy AppImage ${target}")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# 脚本模式入口（cmake -P cmake/Deploy.cmake）
# ---------------------------------------------------------------------------
if(CMAKE_SCRIPT_MODE_FILE)
    if(DEFINED ONVIFSIM_DEPLOY_BUNDLE)
        set(_macos_args)
        if(ONVIFSIM_DEPLOY_NO_DMG)
            list(APPEND _macos_args NO_DMG)
        endif()
        if(DEFINED ONVIFSIM_DEPLOY_DMG)
            list(APPEND _macos_args DMG_OUTPUT "${ONVIFSIM_DEPLOY_DMG}")
        endif()
        onvifsim_deploy_macos("${ONVIFSIM_DEPLOY_BUNDLE}" ${_macos_args})
    elseif(DEFINED ONVIFSIM_DEPLOY_APPDIR)
        onvifsim_deploy_linux("${ONVIFSIM_DEPLOY_BINARY}"
            APPDIR "${ONVIFSIM_DEPLOY_APPDIR}"
            DESKTOP "${ONVIFSIM_DEPLOY_DESKTOP}"
            ICON "${ONVIFSIM_DEPLOY_ICON}")
    elseif(DEFINED ONVIFSIM_DEPLOY_BINARY)
        if(NOT DEFINED ONVIFSIM_DEPLOY_DIR)
            get_filename_component(ONVIFSIM_DEPLOY_DIR "${ONVIFSIM_DEPLOY_BINARY}" DIRECTORY)
        endif()
        onvifsim_deploy_windows("${ONVIFSIM_DEPLOY_BINARY}" "${ONVIFSIM_DEPLOY_DIR}")
    else()
        message(FATAL_ERROR
            "脚本模式要给一个：ONVIFSIM_DEPLOY_BINARY / ONVIFSIM_DEPLOY_BUNDLE / "
            "ONVIFSIM_DEPLOY_APPDIR")
    endif()
endif()
