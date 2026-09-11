<?xml version='1.0' encoding='utf-8'?>
<!DOCTYPE TS>
<TS version="2.1" language="en">
    <context>
        <name>onvifsim::core</name>
        <message>
            <source>端口 %1 超出 1~65535：%2</source>
            <translation>Port %1 out of range 1-65535: %2</translation>
        </message>
        <message>
            <source>未知的品牌预设：%1</source>
            <translation>Unknown vendor persona: %1</translation>
        </message>
        <message>
            <source>非法的绑定地址：%1</source>
            <translation>Invalid bind address: %1</translation>
        </message>
        <message>
            <source>未知的权限级别：%1</source>
            <translation>Unknown user level: %1</translation>
        </message>
        <message>
            <source>用户名为空的用户项已忽略</source>
            <translation>Ignored a user entry with an empty username</translation>
        </message>
        <message>
            <source>预设目录不存在：%1</source>
            <translation>Preset directory does not exist: %1</translation>
        </message>
        <message>
            <source>无法读取 %1：%2</source>
            <translation>Cannot read %1: %2</translation>
        </message>
        <message>
            <source>%1 不是合法 JSON：%2</source>
            <translation>%1 is not valid JSON: %2</translation>
        </message>
        <message>
            <source>未知的 quirk：%1</source>
            <translation>Unknown quirk: %1</translation>
        </message>
        <message>
            <source>quirk %1 的值必须是布尔或对象</source>
            <translation>The value of quirk %1 must be a boolean or an object</translation>
        </message>
        <message>
            <source>quirk %1 没有参数 %2</source>
            <translation>Quirk %1 has no parameter %2</translation>
        </message>
        <message>
            <source>quirk %1 的参数 %2 取值非法：%3（可选 %4）</source>
            <translation>Invalid value for parameter %2 of quirk %1: %3 (allowed: %4)</translation>
        </message>
        <message>
            <source>quirk %1 的参数 %2 小于下限 %3</source>
            <translation>Parameter %2 of quirk %1 is below the minimum %3</translation>
        </message>
        <message>
            <source>quirk %1 的参数 %2 大于上限 %3</source>
            <translation>Parameter %2 of quirk %1 is above the maximum %3</translation>
        </message>
        <message>
            <source>未知的网络模式：%1</source>
            <translation>Unknown network mode: %1</translation>
        </message>
        <message>
            <source>非法的控制面地址：%1</source>
            <translation>Invalid control API address: %1</translation>
        </message>
        <message>
            <source>场景文件格式版本 %1 比本程序新，可能有字段被忽略</source>
            <translation>Scenario format version %1 is newer than this build; some fields may be ignored</translation>
        </message>
        <message>
            <source>无法打开场景文件 %1：%2</source>
            <translation>Cannot open scenario file %1: %2</translation>
        </message>
        <message>
            <source>场景文件不是合法 JSON（偏移 %1）：%2</source>
            <translation>Scenario file is not valid JSON (offset %1): %2</translation>
        </message>
        <message>
            <source>场景文件的顶层必须是一个 JSON 对象</source>
            <translation>The top level of a scenario file must be a JSON object</translation>
        </message>
        <message>
            <source>无法创建目录：%1</source>
            <translation>Cannot create directory: %1</translation>
        </message>
        <message>
            <source>无法写入 %1：%2</source>
            <translation>Cannot write %1: %2</translation>
        </message>
        <message>
            <source>写入 %1 失败：%2</source>
            <translation>Failed to write %1: %2</translation>
        </message>
        <message>
            <source>HTTP 监听失败（%1:%2）：%3</source>
            <translation>Cannot listen on HTTP %1:%2: %3</translation>
        </message>
        <message>
            <source>RTSP 监听失败（%1:%2）：%3</source>
            <translation>Cannot listen on RTSP %1:%2: %3</translation>
        </message>
        <message>
            <source>当前平台不支持自动加 IP 别名</source>
            <translation>Adding IP aliases automatically is not supported on this platform</translation>
        </message>
        <message>
            <source>找不到 %1 命令</source>
            <translation>Cannot find the %1 command</translation>
        </message>
        <message>
            <source>%1 执行超时</source>
            <translation>%1 timed out</translation>
        </message>
        <message>
            <source>%1 返回 %2</source>
            <translation>%1 returned %2</translation>
        </message>
        <message>
            <source>没有 pkexec，无法自动提权</source>
            <translation>pkexec is not installed, cannot elevate automatically</translation>
        </message>
        <message>
            <source>请以管理员身份重新运行，或手动执行下面的命令</source>
            <translation>Re-run as administrator, or execute the commands below by hand</translation>
        </message>
        <message>
            <source>提权执行超时（密码框没被处理？）</source>
            <translation>Elevated command timed out (was the password prompt left open?)</translation>
        </message>
        <message>
            <source>提权执行失败（用户取消？）</source>
            <translation>Elevated command failed (cancelled?)</translation>
        </message>
        <message>
            <source>网卡名或地址为空</source>
            <translation>Interface name or address is empty</translation>
        </message>
        <message>
            <source>网卡名不合法：%1</source>
            <translation>Invalid interface name: %1</translation>
        </message>
        <message>
            <source>起始地址必须是 IPv4，台数必须为正</source>
            <translation>The start address must be IPv4 and the count must be positive</translation>
        </message>
        <message>
            <source>一次最多分配 254 个地址（要了 %1 个）</source>
            <translation>At most 254 addresses at a time (asked for %1)</translation>
        </message>
        <message>
            <source>控制面监听 %1:%2 失败：%3</source>
            <translation>Cannot listen for the control API on %1:%2: %3</translation>
        </message>
    </context>
    <context>
        <name>onvifsim::events</name>
        <message>
            <source>移动侦测</source>
            <translation>Motion detection</translation>
        </message>
        <message>
            <source>移动报警</source>
            <translation>Motion alarm</translation>
        </message>
        <message>
            <source>越界侦测</source>
            <translation>Line crossing</translation>
        </message>
        <message>
            <source>区域入侵</source>
            <translation>Field intrusion</translation>
        </message>
        <message>
            <source>遮挡篡改</source>
            <translation>Tamper</translation>
        </message>
        <message>
            <source>场景变化</source>
            <translation>Scene change</translation>
        </message>
        <message>
            <source>声音侦测</source>
            <translation>Audio detection</translation>
        </message>
        <message>
            <source>画面过暗</source>
            <translation>Image too dark</translation>
        </message>
        <message>
            <source>数字输入</source>
            <translation>Digital input</translation>
        </message>
        <message>
            <source>继电器输出</source>
            <translation>Relay output</translation>
        </message>
        <message>
            <source>CPU 占用</source>
            <translation>CPU usage</translation>
        </message>
        <message>
            <source>人形侦测</source>
            <translation>People detection</translation>
        </message>
        <message>
            <source>车辆侦测</source>
            <translation>Vehicle detection</translation>
        </message>
        <message>
            <source>动物侦测</source>
            <translation>Animal detection</translation>
        </message>
        <message>
            <source>人脸侦测</source>
            <translation>Face detection</translation>
        </message>
    </context>
    <context>
        <name>onvifsim::gui</name>
        <message>
            <source>启动失败</source>
            <translation>Start failed</translation>
        </message>
        <message>
            <source>自动加载上次场景失败：%1</source>
            <translation>Auto-loading the last scenario failed: %1</translation>
        </message>
    </context>
    <context>
        <name>onvifsim::gui::CameraListWidget</name>
        <message>
            <source>离线</source>
            <translation>offline</translation>
        </message>
        <message>
            <source>在线</source>
            <translation>online</translation>
        </message>
        <message>
            <source>已停止</source>
            <translation>stopped</translation>
        </message>
        <message>
            <source>无事件</source>
            <translation>no events</translation>
        </message>
        <message>
            <source>%1  %2
%3 · 流 %4 · 订阅 %5 · %6</source>
            <translation>%1  %2
%3 · streams %4 · subs %5 · %6</translation>
        </message>
        <message>
            <source>%1
%2:%3
预设 %4</source>
            <translation>%1
%2:%3
persona %4</translation>
        </message>
        <message>
            <source>添加相机…</source>
            <translation>Add camera…</translation>
        </message>
        <message>
            <source>停止</source>
            <translation>Stop</translation>
        </message>
        <message>
            <source>启动</source>
            <translation>Start</translation>
        </message>
        <message>
            <source>模拟离线 30 秒</source>
            <translation>Go offline for 30 s</translation>
        </message>
        <message>
            <source>复制 XAddr</source>
            <translation>Copy XAddr</translation>
        </message>
        <message>
            <source>重命名…</source>
            <translation>Rename…</translation>
        </message>
        <message>
            <source>删除相机</source>
            <translation>Remove camera</translation>
        </message>
        <message>
            <source>启动失败</source>
            <translation>Start failed</translation>
        </message>
        <message>
            <source>重命名相机</source>
            <translation>Rename camera</translation>
        </message>
        <message>
            <source>显示名</source>
            <translation>Display name</translation>
        </message>
        <message>
            <source>确定删除 %1 吗？</source>
            <translation>Really remove %1?</translation>
        </message>
    </context>
    <context>
        <name>onvifsim::gui::ConnectionsTab</name>
        <message>
            <source>视频 %1 · 音频 %2 · 对讲 %3</source>
            <translation>video %1 · audio %2 · talkback %3</translation>
        </message>
        <message>
            <source>RTSP 会话</source>
            <translation>RTSP sessions</translation>
        </message>
        <message>
            <source>会话 id</source>
            <translation>Session id</translation>
        </message>
        <message>
            <source>客户端</source>
            <translation>Client</translation>
        </message>
        <message>
            <source>Profile</source>
            <translation>Profile</translation>
        </message>
        <message>
            <source>轨道</source>
            <translation>Tracks</translation>
        </message>
        <message>
            <source>状态</source>
            <translation>State</translation>
        </message>
        <message>
            <source>开始</source>
            <translation>Start</translation>
        </message>
        <message>
            <source>最近活动</source>
            <translation>Last activity</translation>
        </message>
        <message>
            <source>断开选中</source>
            <translation>Tear down selected</translation>
        </message>
        <message>
            <source>全部断开</source>
            <translation>Tear down all</translation>
        </message>
        <message>
            <source>模拟相机侧主动 TEARDOWN，测客户端会不会自动重连。</source>
            <translation>Simulate a camera-initiated TEARDOWN and see whether the client reconnects.</translation>
        </message>
        <message>
            <source>HTTP 客户端</source>
            <translation>HTTP clients</translation>
        </message>
        <message>
            <source>地址</source>
            <translation>Address</translation>
        </message>
        <message>
            <source>User-Agent</source>
            <translation>User-Agent</translation>
        </message>
        <message>
            <source>请求数</source>
            <translation>Requests</translation>
        </message>
        <message>
            <source>最近路径</source>
            <translation>Last path</translation>
        </message>
        <message>
            <source>最近时间</source>
            <translation>Last seen</translation>
        </message>
        <message>
            <source>清空记录</source>
            <translation>Clear records</translation>
        </message>
        <message>
            <source>播放中</source>
            <translation>playing</translation>
        </message>
        <message>
            <source>已建立</source>
            <translation>set up</translation>
        </message>
        <message>
            <source>（未带 User-Agent）</source>
            <translation>(no User-Agent)</translation>
        </message>
        <message>
            <source>当前 HTTP 连接 %1 · 累计客户端 %2</source>
            <translation>HTTP connections %1 · clients seen %2</translation>
        </message>
    </context>
    <context>
        <name>onvifsim::gui::EventsTab</name>
        <message>
            <source>Topic</source>
            <translation>Topic</translation>
        </message>
        <message>
            <source>名称</source>
            <translation>Name</translation>
        </message>
        <message>
            <source>类型</source>
            <translation>Kind</translation>
        </message>
        <message>
            <source>当前状态</source>
            <translation>State</translation>
        </message>
        <message>
            <source>触发选中的事件</source>
            <translation>Trigger the selected event</translation>
        </message>
        <message>
            <source>持续</source>
            <translation>Duration</translation>
        </message>
        <message>
            <source> 毫秒</source>
            <translation> ms</translation>
        </message>
        <message>
            <source>属性型事件会先发 true，过了这个时长再发配对的 false；瞬时事件忽略这个值。</source>
            <translation>Property events send true first and the paired false after this delay; one-shot events ignore it.</translation>
        </message>
        <message>
            <source>自动触发</source>
            <translation>Scheduled triggers</translation>
        </message>
        <message>
            <source>事件</source>
            <translation>Events</translation>
        </message>
        <message>
            <source>间隔</source>
            <translation>Interval</translation>
        </message>
        <message>
            <source>抖动</source>
            <translation>Jitter</translation>
        </message>
        <message>
            <source>启用</source>
            <translation>Enabled</translation>
        </message>
        <message>
            <source>添加</source>
            <translation>Add</translation>
        </message>
        <message>
            <source>删除</source>
            <translation>Remove</translation>
        </message>
        <message>
            <source>应用</source>
            <translation>Apply</translation>
        </message>
        <message>
            <source>事件风暴</source>
            <translation>Event storm</translation>
        </message>
        <message>
            <source> 条/秒</source>
            <translation> msg/s</translation>
        </message>
        <message>
            <source>开始</source>
            <translation>Start</translation>
        </message>
        <message>
            <source>压测客户端的事件处理与队列，看它会不会被淹。</source>
            <translation>Stress the client's event handling and queues to see whether it drowns.</translation>
        </message>
        <message>
            <source>当前订阅</source>
            <translation>Subscriptions</translation>
        </message>
        <message>
            <source>id</source>
            <translation>id</translation>
        </message>
        <message>
            <source>地址</source>
            <translation>Address</translation>
        </message>
        <message>
            <source>过滤器</source>
            <translation>Filter</translation>
        </message>
        <message>
            <source>创建</source>
            <translation>Created</translation>
        </message>
        <message>
            <source>到期</source>
            <translation>Expires</translation>
        </message>
        <message>
            <source>队列</source>
            <translation>Queue</translation>
        </message>
        <message>
            <source>已拉取</source>
            <translation>Pulled</translation>
        </message>
        <message>
            <source>来源</source>
            <translation>Origin</translation>
        </message>
        <message>
            <source>属性型</source>
            <translation>property</translation>
        </message>
        <message>
            <source>瞬时</source>
            <translation>one-shot</translation>
        </message>
        <message>
            <source> 秒</source>
            <translation> s</translation>
        </message>
        <message>
            <source>true</source>
            <translation>true</translation>
        </message>
        <message>
            <source>false</source>
            <translation>false</translation>
        </message>
        <message>
            <source>（全收）</source>
            <translation>(everything)</translation>
        </message>
        <message>
            <source>订阅 %1 · 挂起的 PullMessages %2 · 累计事件 %3 · 最近 %4 %5</source>
            <translation>Subscriptions %1 · pending PullMessages %2 · events sent %3 · last %4 %5</translation>
        </message>
        <message>
            <source>无</source>
            <translation>none</translation>
        </message>
        <message>
            <source>停止</source>
            <translation>Stop</translation>
        </message>
    </context>
    <context>
        <name>onvifsim::gui::IdentityTab</name>
        <message>
            <source>品牌预设</source>
            <translation>Brand persona</translation>
        </message>
        <message>
            <source>预设决定设备信息三件套、RTSP 路径风格、topic 命名、私有 API 桩，以及默认打开哪些故障注入。</source>
            <translation>The persona decides the device information triplet, the RTSP path style, topic naming, the private API stub, and which quirks are on by default.</translation>
        </message>
        <message>
            <source>套用预设</source>
            <translation>Apply persona</translation>
        </message>
        <message>
            <source>用预设的默认值覆盖下面的设备信息与码流路径。</source>
            <translation>Overwrite the device information and stream paths below with the persona defaults.</translation>
        </message>
        <message>
            <source>设备信息</source>
            <translation>Device information</translation>
        </message>
        <message>
            <source>显示名</source>
            <translation>Display name</translation>
        </message>
        <message>
            <source>客户端按这个字串挑厂商适配器，必须逐字可改。</source>
            <translation>Clients pick their vendor adapter from this string, so it has to be editable verbatim.</translation>
        </message>
        <message>
            <source>厂商 Manufacturer</source>
            <translation>Manufacturer</translation>
        </message>
        <message>
            <source>型号 Model</source>
            <translation>Model</translation>
        </message>
        <message>
            <source>固件 FirmwareVersion</source>
            <translation>FirmwareVersion</translation>
        </message>
        <message>
            <source>硬件 id HardwareId</source>
            <translation>HardwareId</translation>
        </message>
        <message>
            <source>序列号 SerialNumber</source>
            <translation>SerialNumber</translation>
        </message>
        <message>
            <source>主机名</source>
            <translation>Host name</translation>
        </message>
        <message>
            <source>位置</source>
            <translation>Location</translation>
        </message>
        <message>
            <source>WS-Discovery 的去重键，单次运行内固定。</source>
            <translation>The WS-Discovery dedup key, fixed for the lifetime of the process.</translation>
        </message>
        <message>
            <source>EndpointReference</source>
            <translation>EndpointReference</translation>
        </message>
        <message>
            <source>Scopes</source>
            <translation>Scopes</translation>
        </message>
        <message>
            <source>完整的 scope URI 列表，ProbeMatch 与 GetScopes 都吐这些。</source>
            <translation>The full list of scope URIs. Both ProbeMatch and GetScopes report these.</translation>
        </message>
        <message>
            <source>添加</source>
            <translation>Add</translation>
        </message>
        <message>
            <source>编辑</source>
            <translation>Edit</translation>
        </message>
        <message>
            <source>删除</source>
            <translation>Remove</translation>
        </message>
        <message>
            <source>添加 scope</source>
            <translation>Add scope</translation>
        </message>
        <message>
            <source>scope URI</source>
            <translation>scope URI</translation>
        </message>
        <message>
            <source>编辑 scope</source>
            <translation>Edit scope</translation>
        </message>
        <message>
            <source>用户</source>
            <translation>Users</translation>
        </message>
        <message>
            <source>用户名</source>
            <translation>User name</translation>
        </message>
        <message>
            <source>密码</source>
            <translation>Password</translation>
        </message>
        <message>
            <source>级别</source>
            <translation>Level</translation>
        </message>
    </context>
    <context>
        <name>onvifsim::gui::LogPanel</name>
        <message>
            <source>相机</source>
            <translation>Camera</translation>
        </message>
        <message>
            <source>级别</source>
            <translation>Level</translation>
        </message>
        <message>
            <source>分类</source>
            <translation>Category</translation>
        </message>
        <message>
            <source>全部</source>
            <translation>All</translation>
        </message>
        <message>
            <source>搜索摘要 / 原文</source>
            <translation>Search summary / payload</translation>
        </message>
        <message>
            <source>暂停</source>
            <translation>Pause</translation>
        </message>
        <message>
            <source>暂停期间新记录仍在收集，恢复后一起显示。</source>
            <translation>New records keep piling up while paused and appear together when you resume.</translation>
        </message>
        <message>
            <source>自动滚动</source>
            <translation>Follow</translation>
        </message>
        <message>
            <source>上限</source>
            <translation>Limit</translation>
        </message>
        <message>
            <source>界面里最多保留多少条。超出的从最老的开始丢，完整日志请用「导出」或命令行的 --log-file。</source>
            <translation>How many records the panel keeps. The oldest are dropped first; for the complete log use Export or --log-file.</translation>
        </message>
        <message>
            <source>清空</source>
            <translation>Clear</translation>
        </message>
        <message>
            <source>导出…</source>
            <translation>Export…</translation>
        </message>
        <message>
            <source>时间</source>
            <translation>Time</translation>
        </message>
        <message>
            <source>客户端</source>
            <translation>Client</translation>
        </message>
        <message>
            <source>耗时</source>
            <translation>Took</translation>
        </message>
        <message>
            <source>摘要</source>
            <translation>Summary</translation>
        </message>
        <message>
            <source>显示 %1 / 保留 %2 条</source>
            <translation>showing %1 / keeping %2</translation>
        </message>
        <message>
            <source> · 待刷新 %1</source>
            <translation> · %1 pending</translation>
        </message>
        <message>
            <source> · 因刷屏丢弃 %1</source>
            <translation> · %1 dropped (flooding)</translation>
        </message>
        <message>
            <source>导出日志</source>
            <translation>Export log</translation>
        </message>
        <message>
            <source>日志文件 (*.log *.txt)</source>
            <translation>Log files (*.log *.txt)</translation>
        </message>
        <message>
            <source>导出失败</source>
            <translation>Export failed</translation>
        </message>
        <message>
            <source>写入 %1 失败：%2</source>
            <translation>Failed to write %1: %2</translation>
        </message>
        <message>
            <source>导出完成</source>
            <translation>Export finished</translation>
        </message>
        <message>
            <source>已导出 %1 条记录到 %2</source>
            <translation>Exported %1 records to %2</translation>
        </message>
    </context>
    <context>
        <name>onvifsim::gui::MainWindow</name>
        <message>
            <source>onvifsim %1 —— ONVIF 摄像头模拟器</source>
            <translation>onvifsim %1 — ONVIF camera simulator</translation>
        </message>
        <message>
            <source>主工具栏</source>
            <translation>Main toolbar</translation>
        </message>
        <message>
            <source>添加相机</source>
            <translation>Add camera</translation>
        </message>
        <message>
            <source>按品牌预设新建一台相机</source>
            <translation>Add a camera from a brand persona</translation>
        </message>
        <message>
            <source>删除相机</source>
            <translation>Remove camera</translation>
        </message>
        <message>
            <source>确定删除 %1 吗？</source>
            <translation>Really remove %1?</translation>
        </message>
        <message>
            <source>全部启动</source>
            <translation>Start all</translation>
        </message>
        <message>
            <source>加载场景</source>
            <translation>Load scenario</translation>
        </message>
        <message>
            <source>保存场景</source>
            <translation>Save scenario</translation>
        </message>
        <message>
            <source>网络</source>
            <translation>Network</translation>
        </message>
        <message>
            <source>网卡选择、网络模式、一键分配 IP 段</source>
            <translation>Interface choice, network mode, one-click IP range</translation>
        </message>
        <message>
            <source>设置</source>
            <translation>Settings</translation>
        </message>
        <message>
            <source>概览</source>
            <translation>Overview</translation>
        </message>
        <message>
            <source>身份</source>
            <translation>Identity</translation>
        </message>
        <message>
            <source>流</source>
            <translation>Streams</translation>
        </message>
        <message>
            <source>PTZ</source>
            <translation>PTZ</translation>
        </message>
        <message>
            <source>事件</source>
            <translation>Events</translation>
        </message>
        <message>
            <source>对讲</source>
            <translation>Talkback</translation>
        </message>
        <message>
            <source>故障注入</source>
            <translation>Quirks</translation>
        </message>
        <message>
            <source>连接</source>
            <translation>Connections</translation>
        </message>
        <message>
            <source>显示主窗口</source>
            <translation>Show main window</translation>
        </message>
        <message>
            <source>退出</source>
            <translation>Quit</translation>
        </message>
        <message>
            <source>onvifsim</source>
            <translation>onvifsim</translation>
        </message>
        <message>
            <source>添加相机失败</source>
            <translation>Adding the camera failed</translation>
        </message>
        <message>
            <source>无法新建相机，详见日志。</source>
            <translation>The camera could not be created, see the log.</translation>
        </message>
        <message>
            <source>启动时有失败</source>
            <translation>Some parts failed to start</translation>
        </message>
        <message>
            <source>%1

常见原因：端口被占用，或者独立 IP 模式下别名还没建好。</source>
            <translation>%1

Usual causes: the port is already taken, or the IP alias is not up yet in dedicated-IP mode.</translation>
        </message>
        <message>
            <source>场景文件 (*.json)</source>
            <translation>Scenario files (*.json)</translation>
        </message>
        <message>
            <source>加载场景失败</source>
            <translation>Loading the scenario failed</translation>
        </message>
        <message>
            <source>场景已加载，但有告警</source>
            <translation>Scenario loaded, with warnings</translation>
        </message>
        <message>
            <source>保存场景失败</source>
            <translation>Saving the scenario failed</translation>
        </message>
        <message>
            <source>已保存到 %1</source>
            <translation>Saved to %1</translation>
        </message>
        <message>
            <source>最近使用</source>
            <translation>Recent</translation>
        </message>
        <message>
            <source>内置示例</source>
            <translation>Built-in samples</translation>
        </message>
        <message>
            <source>浏览…</source>
            <translation>Browse…</translation>
        </message>
        <message>
            <source>需要重启模拟器</source>
            <translation>The simulator has to restart</translation>
        </message>
        <message>
            <source>绑定地址 / 端口 / 发现设置改了，要现在停掉再启动吗？</source>
            <translation>The bind address / ports / discovery settings changed. Stop and start now?</translation>
        </message>
        <message>
            <source>跟随系统</source>
            <translation>Follow the system</translation>
        </message>
        <message>
            <source>界面语言</source>
            <translation>Interface language</translation>
        </message>
        <message>
            <source>日志显示上限</source>
            <translation>Log display limit</translation>
        </message>
        <message>
            <source>调试</source>
            <translation>Debug</translation>
        </message>
        <message>
            <source>信息</source>
            <translation>Info</translation>
        </message>
        <message>
            <source>警告</source>
            <translation>Warning</translation>
        </message>
        <message>
            <source>错误</source>
            <translation>Error</translation>
        </message>
        <message>
            <source>这是 LogBus 层的阈值，比它低的记录连产生都不会产生。</source>
            <translation>This is the LogBus threshold: records below it are never produced at all.</translation>
        </message>
        <message>
            <source>最低日志级别</source>
            <translation>Minimum log level</translation>
        </message>
        <message>
            <source>显示系统托盘图标（关窗口时收进托盘，不退出）</source>
            <translation>Show tray icon (closing the window hides it instead of quitting)</translation>
        </message>
        <message>
            <source>改语言需要重启程序，确定后会问你要不要现在重启。</source>
            <translation>Changing the language needs a restart; you'll be asked after confirming.</translation>
        </message>
        <message>
            <source>重启生效</source>
            <translation>Restart to apply</translation>
        </message>
        <message>
            <source>界面语言已改。翻译要重启程序才能装上。

现在重启吗？重启会让所有相机短暂离线（约一秒），已连接的客户端需要自己重连。</source>
            <translation>The interface language changed. Translations only load at startup.

Restart now? All cameras go offline for about a second and connected clients will have to reconnect.</translation>
        </message>
        <message>
            <source>重启失败</source>
            <translation>Restart failed</translation>
        </message>
        <message>
            <source>没能重新拉起进程，请手动重开。</source>
            <translation>Could not relaunch the process. Please start it again manually.</translation>
        </message>
        <message>
            <source>onvifsim 还在后台跑</source>
            <translation>onvifsim is still running</translation>
        </message>
        <message>
            <source>相机没有停。要真正退出，右键托盘图标选「退出」。</source>
            <translation>The cameras are still up. To quit for real, right-click the tray icon and choose Quit.</translation>
        </message>
        <message>
            <source>启动时自动加载上次的场景</source>
            <translation>Load the last scenario on startup</translation>
        </message>
        <message>
            <source>控制面 关</source>
            <translation>control API off</translation>
        </message>
        <message>
            <source>控制面 http://%1:%2</source>
            <translation>control API http://%1:%2</translation>
        </message>
        <message>
            <source>相机 %1 · 在线 %2 · RTSP 会话 %3 · 订阅 %4 · 发现 %5 · %6</source>
            <translation>Cameras %1 · online %2 · RTSP sessions %3 · subscriptions %4 · discovery %5 · %6</translation>
        </message>
        <message>
            <source>开</source>
            <translation>on</translation>
        </message>
        <message>
            <source>关</source>
            <translation>off</translation>
        </message>
        <message>
            <source>全部停止</source>
            <translation>Stop all</translation>
        </message>
    </context>
    <context>
        <name>onvifsim::gui::NetworkDialog</name>
        <message>
            <source>网络设置</source>
            <translation>Network settings</translation>
        </message>
        <message>
            <source>网卡</source>
            <translation>Interface</translation>
        </message>
        <message>
            <source>全部网卡</source>
            <translation>All interfaces</translation>
        </message>
        <message>
            <source>目标网卡</source>
            <translation>Target interface</translation>
        </message>
        <message>
            <source>网络模式</source>
            <translation>Network mode</translation>
        </message>
        <message>
            <source>端口模式：同一 IP、端口递增（默认，不需要权限）</source>
            <translation>Ports: one IP, incrementing ports (the default, needs no privileges)</translation>
        </message>
        <message>
            <source>独立 IP 模式：每台相机一个 IP 别名、绑标准端口 80 / 554</source>
            <translation>Dedicated IP: one alias per camera, bound to the standard ports 80 / 554</translation>
        </message>
        <message>
            <source>外部编排：地址由 Docker macvlan 等安排好，只负责绑定</source>
            <translation>External: addresses are arranged by Docker macvlan or the like, we only bind to them</translation>
        </message>
        <message>
            <source>0.0.0.0 表示所有网卡。同机测试时对外报的地址会自动挑 LAN IP，不会给出 127.0.0.1。</source>
            <translation>0.0.0.0 means every interface. When client and simulator share a host the advertised address picks a LAN IP, never 127.0.0.1.</translation>
        </message>
        <message>
            <source>绑定地址</source>
            <translation>Bind address</translation>
        </message>
        <message>
            <source>HTTP 起始端口</source>
            <translation>HTTP base port</translation>
        </message>
        <message>
            <source>RTSP 起始端口</source>
            <translation>RTSP base port</translation>
        </message>
        <message>
            <source>IP 别名（独立 IP 模式）</source>
            <translation>IP aliases (dedicated-IP mode)</translation>
        </message>
        <message>
            <source>起始地址</source>
            <translation>First address</translation>
        </message>
        <message>
            <source>数量</source>
            <translation>Count</translation>
        </message>
        <message>
            <source>子网掩码</source>
            <translation>Netmask</translation>
        </message>
        <message>
            <source>一键分配 IP 段</source>
            <translation>Assign IP range</translation>
        </message>
        <message>
            <source>给现有相机逐台分配地址，需要的话会提权添加网卡别名。</source>
            <translation>Hand one address to each existing camera, elevating to add interface aliases when needed.</translation>
        </message>
        <message>
            <source>回收本进程添加的别名</source>
            <translation>Release aliases added by this process</translation>
        </message>
        <message>
            <source>提权失败时，这里会列出可以手工执行的命令。</source>
            <translation>When elevation fails, the commands you can run by hand show up here.</translation>
        </message>
        <message>
            <source>发现与控制面</source>
            <translation>Discovery and control API</translation>
        </message>
        <message>
            <source>启用 WS-Discovery（UDP 3702）</source>
            <translation>Enable WS-Discovery (UDP 3702)</translation>
        </message>
        <message>
            <source>发现使用的网卡</source>
            <translation>Discovery interface</translation>
        </message>
        <message>
            <source>启用 REST 控制面</source>
            <translation>Enable the REST control API</translation>
        </message>
        <message>
            <source>控制面端口</source>
            <translation>Control API port</translation>
        </message>
        <message>
            <source>留空表示不校验</source>
            <translation>Leave empty to skip validation</translation>
        </message>
        <message>
            <source>控制面 token</source>
            <translation>Control API token</translation>
        </message>
        <message>
            <source>地址不合法</source>
            <translation>Invalid address</translation>
        </message>
        <message>
            <source>绑定地址「%1」不是合法的 IP，填 0.0.0.0 表示监听全部网卡。</source>
            <translation>“%1” is not a valid IP address. Use 0.0.0.0 to listen on every interface.</translation>
        </message>
        <message>
            <source>请填一个 IPv4 地址，例如 192.168.1.201</source>
            <translation>Enter an IPv4 address, for example 192.168.1.201</translation>
        </message>
        <message>
            <source>添加别名失败：%1

可以手工执行：
%2

%3</source>
            <translation>Adding the alias failed: %1

You can run this by hand:
%2

%3</translation>
        </message>
        <message>
            <source>已分配 %1 个地址：
%2</source>
            <translation>Assigned %1 addresses:
%2</translation>
        </message>
        <message>
            <source>已回收本进程添加的全部别名。</source>
            <translation>Every alias added by this process has been released.</translation>
        </message>
    </context>
    <context>
        <name>onvifsim::gui::OverviewTab</name>
        <message>
            <source>运行状态</source>
            <translation>Run state</translation>
        </message>
        <message>
            <source>启动</source>
            <translation>Start</translation>
        </message>
        <message>
            <source>模拟离线</source>
            <translation>Go offline</translation>
        </message>
        <message>
            <source> 秒</source>
            <translation> s</translation>
        </message>
        <message>
            <source>启动失败</source>
            <translation>Start failed</translation>
        </message>
        <message>
            <source>服务地址</source>
            <translation>Service addresses</translation>
        </message>
        <message>
            <source>设备服务 XAddr</source>
            <translation>Device service XAddr</translation>
        </message>
        <message>
            <source>客户端硬编码这条路径，任何故障注入都不会改它。</source>
            <translation>Clients hardcode this path, and no quirk ever changes it.</translation>
        </message>
        <message>
            <source>Media 服务</source>
            <translation>Media service</translation>
        </message>
        <message>
            <source>Events 服务</source>
            <translation>Events service</translation>
        </message>
        <message>
            <source>PTZ 服务</source>
            <translation>PTZ service</translation>
        </message>
        <message>
            <source>Imaging 服务</source>
            <translation>Imaging service</translation>
        </message>
        <message>
            <source>快照地址</source>
            <translation>Snapshot URI</translation>
        </message>
        <message>
            <source>实际监听</source>
            <translation>Listening on</translation>
        </message>
        <message>
            <source>码流地址</source>
            <translation>Stream URIs</translation>
        </message>
        <message>
            <source>凭据</source>
            <translation>Credentials</translation>
        </message>
        <message>
            <source>用户名</source>
            <translation>User name</translation>
        </message>
        <message>
            <source>密码</source>
            <translation>Password</translation>
        </message>
        <message>
            <source>复制</source>
            <translation>Copy</translation>
        </message>
        <message>
            <source>复制到剪贴板</source>
            <translation>Copy to clipboard</translation>
        </message>
        <message>
            <source>没有选中相机</source>
            <translation>No camera selected</translation>
        </message>
        <message>
            <source>离线中（还有 %1 秒恢复）</source>
            <translation>offline (back in %1 s)</translation>
        </message>
        <message>
            <source>在线</source>
            <translation>online</translation>
        </message>
        <message>
            <source>已停止</source>
            <translation>stopped</translation>
        </message>
        <message>
            <source>%1 · %2 · RTSP 会话 %3 · 订阅 %4 · HTTP 连接 %5 · 最近事件 %6</source>
            <translation>%1 · %2 · RTSP sessions %3 · subscriptions %4 · HTTP connections %5 · last event %6</translation>
        </message>
        <message>
            <source>无</source>
            <translation>none</translation>
        </message>
        <message>
            <source>停止</source>
            <translation>Stop</translation>
        </message>
        <message>
            <source>%1  HTTP %2  RTSP %3</source>
            <translation>%1  HTTP %2  RTSP %3</translation>
        </message>
        <message>
            <source>（无用户，等于不鉴权）</source>
            <translation>(no users, i.e. no authentication)</translation>
        </message>
    </context>
    <context>
        <name>onvifsim::gui::PtzPadWidget</name>
        <message>
            <source>按住拖动 = 连续移动（离中心越远越快），松开即停。</source>
            <translation>Press and drag to move continuously (farther from the centre is faster); release to stop.</translation>
        </message>
        <message>
            <source>P %1  T %2  Z %3</source>
            <translation>P %1  T %2  Z %3</translation>
        </message>
    </context>
    <context>
        <name>onvifsim::gui::PtzTab</name>
        <message>
            <source>空闲</source>
            <translation>idle</translation>
        </message>
        <message>
            <source>移动中</source>
            <translation>moving</translation>
        </message>
        <message>
            <source>未知</source>
            <translation>unknown</translation>
        </message>
        <message>
            <source>速度</source>
            <translation>Speed</translation>
        </message>
        <message>
            <source>方向</source>
            <translation>Direction</translation>
        </message>
        <message>
            <source>停</source>
            <translation>Stop</translation>
        </message>
        <message>
            <source>设为 Home</source>
            <translation>Set home</translation>
        </message>
        <message>
            <source>回 Home</source>
            <translation>Go home</translation>
        </message>
        <message>
            <source>回 Home 失败</source>
            <translation>Cannot go to Home</translation>
        </message>
        <message>
            <source>这台相机还没有设置过 Home 位置，先点「设为 Home」。</source>
            <translation>This camera has no Home position yet. Use “Set Home” first.</translation>
        </message>
        <message>
            <source>图像参数</source>
            <translation>Imaging</translation>
        </message>
        <message>
            <source>亮度</source>
            <translation>Brightness</translation>
        </message>
        <message>
            <source>对比度</source>
            <translation>Contrast</translation>
        </message>
        <message>
            <source>自动</source>
            <translation>Auto</translation>
        </message>
        <message>
            <source>开</source>
            <translation>on</translation>
        </message>
        <message>
            <source>关</source>
            <translation>off</translation>
        </message>
        <message>
            <source>红外滤片</source>
            <translation>IR cut filter</translation>
        </message>
        <message>
            <source>白光补光灯</source>
            <translation>White supplement light</translation>
        </message>
        <message>
            <source>ONVIF 没有标准字段，走厂商私有接口，但状态就是这一份。</source>
            <translation>ONVIF has no standard field for this, so vendors expose it privately, but the state lives right here.</translation>
        </message>
        <message>
            <source>补光亮度</source>
            <translation>Light brightness</translation>
        </message>
        <message>
            <source>预置位</source>
            <translation>Presets</translation>
        </message>
        <message>
            <source>Token</source>
            <translation>Token</translation>
        </message>
        <message>
            <source>名称</source>
            <translation>Name</translation>
        </message>
        <message>
            <source>预置位名称</source>
            <translation>Preset name</translation>
        </message>
        <message>
            <source>保存当前位置</source>
            <translation>Save current position</translation>
        </message>
        <message>
            <source>转到</source>
            <translation>Go to</translation>
        </message>
        <message>
            <source>删除</source>
            <translation>Remove</translation>
        </message>
        <message>
            <source>一键生成出厂槽位</source>
            <translation>Generate factory presets</translation>
        </message>
        <message>
            <source>对应故障注入 C6：真机出厂就把槽位填满，客户端把 GetPresets 结果原样铺到界面上会被撑爆。</source>
            <translation>Quirk C6: real firmware ships with every slot filled, which blows up clients that render GetPresets results as-is.</translation>
        </message>
        <message>
            <source>清空</source>
            <translation>Clear</translation>
        </message>
        <message>
            <source>预置位 %1</source>
            <translation>Preset %1</translation>
        </message>
        <message>
            <source>保存预置位失败</source>
            <translation>Saving the preset failed</translation>
        </message>
        <message>
            <source>转到预置位失败</source>
            <translation>Cannot go to preset</translation>
        </message>
        <message>
            <source>预置位 %1 不存在，列表可能已经过期。</source>
            <translation>Preset %1 does not exist; the list may be out of date.</translation>
        </message>
        <message>
            <source>删除预置位失败</source>
            <translation>Cannot delete preset</translation>
        </message>
        <message>
            <source>PanTilt %1 · Zoom %2 · 预置位 %3 / %4</source>
            <translation>PanTilt %1 · Zoom %2 · presets %3 / %4</translation>
        </message>
    </context>
    <context>
        <name>onvifsim::gui::QuirksTab</name>
        <message>
            <source>作用于</source>
            <translation>Apply to</translation>
        </message>
        <message>
            <source>当前相机</source>
            <translation>Current camera</translation>
        </message>
        <message>
            <source>全局默认（新建相机的初值）</source>
            <translation>Global defaults (initial values for new cameras)</translation>
        </message>
        <message>
            <source>搜索标题 / 说明 / 出处编号 / key</source>
            <translation>Search title / description / source id / key</translation>
        </message>
        <message>
            <source>只看已开启</source>
            <translation>Enabled only</translation>
        </message>
        <message>
            <source>全部关闭</source>
            <translation>Turn all off</translation>
        </message>
        <message>
            <source>把全局默认应用到全部相机</source>
            <translation>Apply the global defaults to every camera</translation>
        </message>
        <message>
            <source>共 %1 条故障注入，已开启 %2 条。悬停任意一项可以看到它的出处说明。</source>
            <translation>%1 quirks in total, %2 enabled. Hover any of them to see where it came from.</translation>
        </message>
    </context>
    <context>
        <name>onvifsim::gui::StreamsTab</name>
        <message>
            <source>Token</source>
            <translation>Token</translation>
        </message>
        <message>
            <source>名称</source>
            <translation>Name</translation>
        </message>
        <message>
            <source>分辨率</source>
            <translation>Resolution</translation>
        </message>
        <message>
            <source>编码</source>
            <translation>Codec</translation>
        </message>
        <message>
            <source>帧率</source>
            <translation>Frame rate</translation>
        </message>
        <message>
            <source>码率</source>
            <translation>Bitrate</translation>
        </message>
        <message>
            <source>音频</source>
            <translation>Audio</translation>
        </message>
        <message>
            <source>PTZConfiguration</source>
            <translation>PTZConfiguration</translation>
        </message>
        <message>
            <source>样片</source>
            <translation>Clip</translation>
        </message>
        <message>
            <source>添加 profile</source>
            <translation>Add profile</translation>
        </message>
        <message>
            <source>删除 profile</source>
            <translation>Remove profile</translation>
        </message>
        <message>
            <source>编辑选中的 profile</source>
            <translation>Edit the selected profile</translation>
        </message>
        <message>
            <source>参照客户端判主 / 子码流纯看这个名字，改成没有主子语义的串就能复现「认不出主码流」。</source>
            <translation>The reference client tells main from sub stream purely by this name; rename it to something without that hint to reproduce "main stream not recognised".</translation>
        </message>
        <message>
            <source>声明分辨率</source>
            <translation>Declared resolution</translation>
        </message>
        <message>
            <source>视频编码</source>
            <translation>Video codec</translation>
        </message>
        <message>
            <source> kbps</source>
            <translation> kbps</translation>
        </message>
        <message>
            <source>GOP 长度</source>
            <translation>GOP length</translation>
        </message>
        <message>
            <source>带音频</source>
            <translation>With audio</translation>
        </message>
        <message>
            <source>音频编码</source>
            <translation>Audio codec</translation>
        </message>
        <message>
            <source>音频码率</source>
            <translation>Audio bitrate</translation>
        </message>
        <message>
            <source>挂 PTZConfiguration</source>
            <translation>Attach PTZConfiguration</translation>
        </message>
        <message>
            <source>很多真机只在子码流上挂，主码流不挂 —— 见故障注入 C1 / C2。</source>
            <translation>Plenty of real cameras attach it to the sub stream only, never to the main one — see quirks C1 / C2.</translation>
        </message>
        <message>
            <source>挂 AudioOutput / AudioDecoder（对讲）</source>
            <translation>Attach AudioOutput / AudioDecoder (talkback)</translation>
        </message>
        <message>
            <source>挂 Metadata 配置</source>
            <translation>Attach the metadata configuration</translation>
        </message>
        <message>
            <source>留空表示按品牌预设的风格生成。</source>
            <translation>Leave empty to generate it in the persona's style.</translation>
        </message>
        <message>
            <source>RTSP 路径</source>
            <translation>RTSP path</translation>
        </message>
        <message>
            <source>内嵌样片档位，或外部 .h264 文件的绝对路径。这里发的才是真实码流，与上面的声明值可以故意不一致。</source>
            <translation>A built-in clip preset, or the absolute path of an external .h264 file. This is what actually goes on the wire, and it may deliberately disagree with the values declared above.</translation>
        </message>
        <message>
            <source>外部文件…</source>
            <translation>External file…</translation>
        </message>
        <message>
            <source>实际样片</source>
            <translation>Actual clip</translation>
        </message>
        <message>
            <source>应用</source>
            <translation>Apply</translation>
        </message>
        <message>
            <source>选择 H.264 裸流文件</source>
            <translation>Choose an H.264 elementary stream</translation>
        </message>
        <message>
            <source>H.264 裸流 (*.h264 *.264);;所有文件 (*)</source>
            <translation>H.264 elementary stream (*.h264 *.264);;All files (*)</translation>
        </message>
        <message>
            <source>%1 kbps</source>
            <translation>%1 kbps</translation>
        </message>
        <message>
            <source>无</source>
            <translation>none</translation>
        </message>
        <message>
            <source>有</source>
            <translation>yes</translation>
        </message>
    </context>
    <context>
        <name>onvifsim::gui::TalkbackTab</name>
        <message>
            <source>无</source>
            <translation>none</translation>
        </message>
        <message>
            <source>对讲会话</source>
            <translation>Talkback session</translation>
        </message>
        <message>
            <source>重置统计</source>
            <translation>Reset statistics</translation>
        </message>
        <message>
            <source>保存 wav…</source>
            <translation>Save WAV…</translation>
        </message>
        <message>
            <source>电平</source>
            <translation>Level</translation>
        </message>
        <message>
            <source>统计项</source>
            <translation>Metric</translation>
        </message>
        <message>
            <source>值</source>
            <translation>Value</translation>
        </message>
        <message>
            <source>编码</source>
            <translation>Codec</translation>
        </message>
        <message>
            <source>收到包数</source>
            <translation>Packets received</translation>
        </message>
        <message>
            <source>收到字节</source>
            <translation>Bytes received</translation>
        </message>
        <message>
            <source>推算丢包</source>
            <translation>Estimated loss</translation>
        </message>
        <message>
            <source>marker 计数</source>
            <translation>marker count</translation>
        </message>
        <message>
            <source>marker 缺失</source>
            <translation>markers missing</translation>
        </message>
        <message>
            <source>当前电平</source>
            <translation>Current level</translation>
        </message>
        <message>
            <source>峰值电平</source>
            <translation>Peak level</translation>
        </message>
        <message>
            <source>首包时间</source>
            <translation>First packet</translation>
        </message>
        <message>
            <source>末包时间</source>
            <translation>Last packet</translation>
        </message>
        <message>
            <source>保存对讲音频</source>
            <translation>Save talkback audio</translation>
        </message>
        <message>
            <source>WAV 音频 (*.wav)</source>
            <translation>WAV audio (*.wav)</translation>
        </message>
        <message>
            <source>保存失败</source>
            <translation>Save failed</translation>
        </message>
        <message>
            <source>%1 · %2</source>
            <translation>%1 · %2</translation>
        </message>
        <message>
            <source>没有选中相机。</source>
            <translation>No camera selected.</translation>
        </message>
        <message>
            <source>还没有带 backchannel 的 RTSP 会话。客户端要先 DESCRIBE 时带上 Require: www.onvif.org/ver20/backchannel，SETUP 那条 sendonly 轨，再 PLAY，才会有音频推进来。</source>
            <translation>No RTSP session with a backchannel yet. The client has to DESCRIBE with Require: www.onvif.org/ver20/backchannel, SETUP the sendonly track and then PLAY before any audio can arrive.</translation>
        </message>
        <message>
            <source>已收到 %1 个 RTP 包。</source>
            <translation>%1 RTP packets received.</translation>
        </message>
        <message>
            <source>会话已建立，但还没有 RTP 包进来。</source>
            <translation>The session is up, but no RTP packet has arrived yet.</translation>
        </message>
        <message>
            <source>停止录制</source>
            <translation>Stop recording</translation>
        </message>
    </context>
    <context>
        <name>onvifsim::gui::util</name>
        <message>
            <source>已复制</source>
            <translation>Copied</translation>
        </message>
        <message>
            <source>%1 秒前</source>
            <translation>%1 s ago</translation>
        </message>
        <message>
            <source>%1 分钟前</source>
            <translation>%1 min ago</translation>
        </message>
        <message>
            <source>%1 小时前</source>
            <translation>%1 h ago</translation>
        </message>
        <message>
            <source>%1 B</source>
            <translation>%1 B</translation>
        </message>
        <message>
            <source>%1 KB</source>
            <translation>%1 KB</translation>
        </message>
        <message>
            <source>%1 MB</source>
            <translation>%1 MB</translation>
        </message>
        <message>
            <source>%1 GB</source>
            <translation>%1 GB</translation>
        </message>
        <message>
            <source>%1 µs</source>
            <translation>%1 µs</translation>
        </message>
        <message>
            <source>%1 ms</source>
            <translation>%1 ms</translation>
        </message>
        <message>
            <source>调试</source>
            <translation>Debug</translation>
        </message>
        <message>
            <source>信息</source>
            <translation>Info</translation>
        </message>
        <message>
            <source>警告</source>
            <translation>Warning</translation>
        </message>
        <message>
            <source>错误</source>
            <translation>Error</translation>
        </message>
    </context>
    <context>
        <name>onvifsim::persona</name>
        <message>
            <source>标准 ONVIF Generic</source>
            <translation>Generic ONVIF</translation>
        </message>
        <message>
            <source>海康威视 Hikvision</source>
            <translation>Hikvision</translation>
        </message>
        <message>
            <source>大华 Dahua</source>
            <translation>Dahua</translation>
        </message>
        <message>
            <source>Reolink</source>
            <translation>Reolink</translation>
        </message>
        <message>
            <source>TP-Link VIGI</source>
            <translation>TP-Link VIGI</translation>
        </message>
        <message>
            <source>TP-Link TL-IPC</source>
            <translation>TP-Link TL-IPC</translation>
        </message>
        <message>
            <source>Axis</source>
            <translation>Axis</translation>
        </message>
        <message>
            <source>宇视 Uniview</source>
            <translation>Uniview</translation>
        </message>
    </context>
    <context>
        <name>onvifsim::quirks</name>
        <message>
            <source>发现</source>
            <translation>Discovery</translation>
        </message>
        <message>
            <source>WS-Discovery 命名空间方言</source>
            <translation>WS-Discovery namespace dialect</translation>
        </message>
        <message>
            <source>默认照抄 Probe 的命名空间回复。参照客户端用 1.0 方言（2005/04 discovery + 2004/08 addressing），ONVIF 另一常见方言是 2009/01；强制成客户端不认的那套即可复现「搜不到设备」。</source>
            <translation>By default the reply echoes the Probe's namespaces. The reference client speaks the 1.0 dialect (2005/04 discovery + 2004/08 addressing); the other common ONVIF dialect is 2009/01. Forcing the dialect the client does not accept reproduces “the device is never found”.</translation>
        </message>
        <message>
            <source>echo=照抄 Probe，2005=强制 1.0，2009=强制 OASIS</source>
            <translation>echo = mirror the Probe, 2005 = force 1.0, 2009 = force OASIS</translation>
        </message>
        <message>
            <source>ProbeMatch 不带 XAddrs</source>
            <translation>ProbeMatch without XAddrs</translation>
        </message>
        <message>
            <source>客户端收到空 XAddrs 会自动多播 Resolve 补问，用来验证 Resolve 分支。</source>
            <translation>A client that receives empty XAddrs multicasts a Resolve to ask again. Use this to exercise that branch.</translation>
        </message>
        <message>
            <source>ProbeMatch 缺 MetadataVersion</source>
            <translation>ProbeMatch missing MetadataVersion</translation>
        </message>
        <message>
            <source>wsdiscovery 2.1.2 解析 ProbeMatch 时缺这个字段会把整包丢掉，设备直接消失。</source>
            <translation>wsdiscovery 2.1.2 drops the whole packet when this field is absent, so the device simply vanishes.</translation>
        </message>
        <message>
            <source>XAddrs 放不可达地址</source>
            <translation>Unreachable address in XAddrs</translation>
        </message>
        <message>
            <source>客户端只取 getXAddrs()[0]，把坏地址放第一位就能测它的容错。</source>
            <translation>Clients often use getXAddrs()[0] only. Putting the bad address first tests how they cope.</translation>
        </message>
        <message>
            <source>要插入的 host，可填 0.0.0.0 或主机名</source>
            <translation>Host to insert; 0.0.0.0 or a hostname also works</translation>
        </message>
        <message>
            <source>放在 XAddrs 第一位</source>
            <translation>Put it first in XAddrs</translation>
        </message>
        <message>
            <source>不回 ProbeMatch</source>
            <translation>No ProbeMatch at all</translation>
        </message>
        <message>
            <source>设备在线但不应答发现，客户端只能手工填 IP。</source>
            <translation>The device is up but never answers discovery, so the address has to be typed in by hand.</translation>
        </message>
        <message>
            <source>延迟回复 ProbeMatch</source>
            <translation>Delayed ProbeMatch</translation>
        </message>
        <message>
            <source>wsdiscovery 的 searchServices 是「发 Probe → sleep timeout → 收结果」，延迟超过客户端窗口（默认 3s）就等于没回。</source>
            <translation>wsdiscovery's searchServices sends the Probe, sleeps for the timeout, then collects replies. A delay past that window (3s by default) is the same as no reply.</translation>
        </message>
        <message>
            <source>延迟毫秒数</source>
            <translation>Delay in milliseconds</translation>
        </message>
        <message>
            <source>同一 Probe 回两次</source>
            <translation>Answer the same Probe twice</translation>
        </message>
        <message>
            <source>客户端按 EPR 去重、同 EPR 后到覆盖先到；两份 XAddrs 不同时「最后到达那份的第一个 XAddr 胜出」。</source>
            <translation>Clients de-duplicate by EPR and the later reply wins. When the two replies carry different XAddrs, the first XAddr of the last one to arrive is what gets used.</translation>
        </message>
        <message>
            <source>第二份用不同的 XAddrs</source>
            <translation>Use different XAddrs in the second reply</translation>
        </message>
        <message>
            <source>Scopes 不含 name</source>
            <translation>Scopes without a name</translation>
        </message>
        <message>
            <source>上层只从 Scopes 读 onvif.org/name/ 与 /hardware/，缺 name 时相机显示为无名。</source>
            <translation>Clients read onvif.org/name/ and /hardware/ out of Scopes; without name the camera shows up unnamed.</translation>
        </message>
        <message>
            <source>建连与鉴权</source>
            <translation>Connect and authentication</translation>
        </message>
        <message>
            <source>XAddr 报非常规端口 / 路径</source>
            <translation>XAddr advertises an odd port or path</translation>
        </message>
        <message>
            <source>真机 TL-IPC652P-A4 报 :2020/onvif/service 而实际连的是 80。客户端只接管 host:port、保留 path，正好测这条逻辑。</source>
            <translation>A real TL-IPC652P-A4 advertises :2020/onvif/service while actually listening on 80. Clients replace host:port but keep the path, which is exactly what this exercises.</translation>
        </message>
        <message>
            <source>对外宣称的端口</source>
            <translation>Advertised port</translation>
        </message>
        <message>
            <source>对外宣称的服务路径</source>
            <translation>Advertised service path</translation>
        </message>
        <message>
            <source>GetServices 里 Media2 排在 Media 前</source>
            <translation>Media2 listed before Media in GetServices</translation>
        </message>
        <message>
            <source>Media(ver10) 与 Media2(ver20) 的命名空间都含 /media/；只按服务名匹配的客户端会把 media 解析到 Media2 端点，然后往那儿发 ver10 的 GetProfiles 吃 ActionNotSupported。海康 / Axis 双栈固件的真实布局。</source>
            <translation>Both Media (ver10) and Media2 (ver20) namespaces contain /media/. A client matching on the service name alone resolves media to the Media2 endpoint and then gets ActionNotSupported for a ver10 GetProfiles. This is the real layout of dual-stack Hikvision and Axis firmware.</translation>
        </message>
        <message>
            <source>不实现 GetServices</source>
            <translation>GetServices not implemented</translation>
        </message>
        <message>
            <source>老固件常见。客户端要能回落到 GetCapabilities 或约定路径。</source>
            <translation>Common on older firmware. Clients must fall back to GetCapabilities or the conventional paths.</translation>
        </message>
        <message>
            <source>UsernameToken 时间窗收紧</source>
            <translation>Tighter UsernameToken time window</translation>
        </message>
        <message>
            <source>参照客户端不做时钟补偿（adjust_time 从不传 True）。把 Created 允许偏差收紧再叠加时钟偏移，就能复现「全线 401」。</source>
            <translation>The reference client never compensates for clock skew (it never passes adjust_time=True). Tightening the allowed Created skew and adding a clock offset reproduces “everything returns 401”.</translation>
        </message>
        <message>
            <source>允许的时间偏差（秒）</source>
            <translation>Allowed skew (seconds)</translation>
        </message>
        <message>
            <source>设备时钟相对真实时间的偏移（秒）</source>
            <translation>Device clock offset from real time (seconds)</translation>
        </message>
        <message>
            <source>订阅只增不回收</source>
            <translation>Subscriptions are never reclaimed</translation>
        </message>
        <message>
            <source>onvif-zeep 每次构造 ONVIFCamera 都建一条 PullPoint 订阅且从不 Unsubscribe。关掉过期回收即可复现真实高发的槽位泄漏。</source>
            <translation>onvif-zeep creates a PullPoint subscription every time an ONVIFCamera is constructed and never unsubscribes. Turning off expiry reclamation reproduces the slot leak this causes in the field.</translation>
        </message>
        <message>
            <source>GetDeviceInformation 缺字段</source>
            <translation>GetDeviceInformation with missing fields</translation>
        </message>
        <message>
            <source>真机常缺 HardwareId 或 SerialNumber，按这些字串选厂商适配器的客户端会落空。</source>
            <translation>Real devices often omit HardwareId or SerialNumber, which breaks clients that pick a vendor adapter by matching those strings.</translation>
        </message>
        <message>
            <source>要省略的字段，逗号分隔：Manufacturer,Model,FirmwareVersion,SerialNumber,HardwareId</source>
            <translation>Fields to omit, comma separated: Manufacturer,Model,FirmwareVersion,SerialNumber,HardwareId</translation>
        </message>
        <message>
            <source>只接受 PasswordText</source>
            <translation>Accept PasswordText only</translation>
        </message>
        <message>
            <source>参照客户端一律发 PasswordDigest，拒绝 Digest 即全线鉴权失败。</source>
            <translation>The reference client always sends PasswordDigest, so rejecting Digest fails every request.</translation>
        </message>
        <message>
            <source>nonce 严格一次性</source>
            <translation>Strictly single-use nonce</translation>
        </message>
        <message>
            <source>缓存已用 nonce，重复即 401，用来测客户端是否每次重新生成。</source>
            <translation>Used nonces are remembered and a repeat gets 401, testing whether the client generates a fresh one each time.</translation>
        </message>
        <message>
            <source>nonce 缓存时长（秒）</source>
            <translation>How long a nonce is remembered (seconds)</translation>
        </message>
        <message>
            <source>PRE_AUTH 操作也要鉴权</source>
            <translation>Require auth even for PRE_AUTH operations</translation>
        </message>
        <message>
            <source>规范允许 GetSystemDateAndTime / GetCapabilities / GetServices / GetWsdlUrl 匿名调用；有些固件不允许。</source>
            <translation>The spec allows GetSystemDateAndTime, GetCapabilities, GetServices and GetWsdlUrl anonymously; some firmware does not.</translation>
        </message>
        <message>
            <source>鉴权失败回 HTTP 401</source>
            <translation>Return HTTP 401 instead of a Fault</translation>
        </message>
        <message>
            <source>规范做法是 SOAP Fault ter:NotAuthorized；有些固件直接 401 带 WWW-Authenticate: Digest。</source>
            <translation>The spec says SOAP Fault ter:NotAuthorized; some firmware replies 401 with WWW-Authenticate: Digest instead.</translation>
        </message>
        <message>
            <source>SOAP Fault 的 HTTP 状态码</source>
            <translation>HTTP status carrying a SOAP Fault</translation>
        </message>
        <message>
            <source>Fault 既可能走 500 也可能走 200。客户端对 500 要放行给 body 解析。</source>
            <translation>A Fault may arrive with 500 or with 200. Clients must still parse the body on a 500.</translation>
        </message>
        <message>
            <source>返回 Fault 时用的 HTTP 状态码</source>
            <translation>HTTP status used for Faults</translation>
        </message>
        <message>
            <source>鉴权 Fault 措辞变体</source>
            <translation>Auth Fault wording variants</translation>
        </message>
        <message>
            <source>客户端靠关键词匹配判定认证错误：notauthorized / not authorized / unauthorized / authentication / sender not authorized / failedauthentication。</source>
            <translation>Clients detect an auth failure by keyword: notauthorized / not authorized / unauthorized / authentication / sender not authorized / failedauthentication.</translation>
        </message>
        <message>
            <source>Fault 子码与措辞风格；Unknown=不含任何关键词</source>
            <translation>Fault subcode and wording; Unknown = contains no keyword at all</translation>
        </message>
        <message>
            <source>Media 与快照</source>
            <translation>Media and snapshots</translation>
        </message>
        <message>
            <source>profile 命名风格</source>
            <translation>Profile naming style</translation>
        </message>
        <message>
            <source>客户端判定主 / 子码流只看 profile Name 是否含 main|primary|high / sub|secondary|low；Profile_1 / Profile_2 这种没有主子语义的名字会逼它按顺序猜。</source>
            <translation>Clients decide main vs sub stream purely from whether the profile Name contains main|primary|high or sub|secondary|low. Names like Profile_1 / Profile_2 carry no such hint and force the client to guess by order.</translation>
        </message>
        <message>
            <source>命名风格</source>
            <translation>Naming style</translation>
        </message>
        <message>
            <source>StreamUri 自带 user:pass@</source>
            <translation>StreamUri carries user:pass@</translation>
        </message>
        <message>
            <source>客户端会丢弃相机返回的 userinfo 再注入自己的凭据，用来验证这段清洗逻辑。</source>
            <translation>Clients strip the userinfo the camera returned and inject their own credentials; this exercises that sanitising step.</translation>
        </message>
        <message>
            <source>嵌入 URI 的 user:pass</source>
            <translation>user:pass embedded in the URI</translation>
        </message>
        <message>
            <source>URI 套在 MediaUri/Uri 下</source>
            <translation>URI nested under MediaUri/Uri</translation>
        </message>
        <message>
            <source>响应结构多一层，只认顶层 Uri 的客户端会取空。</source>
            <translation>One extra level of nesting; a client that only reads the top-level Uri gets nothing.</translation>
        </message>
        <message>
            <source>StreamUri 用占位 IP</source>
            <translation>StreamUri with a placeholder IP</translation>
        </message>
        <message>
            <source>固件把出厂默认 IP 写死进 URI，客户端必须用 XAddr 的 host 覆盖。</source>
            <translation>The firmware bakes its factory-default IP into the URI, so the client must override the host with the one from XAddr.</translation>
        </message>
        <message>
            <source>占位 host，可填 0.0.0.0</source>
            <translation>Placeholder host; 0.0.0.0 also works</translation>
        </message>
        <message>
            <source>快照 200 + 空体</source>
            <translation>Snapshot: 200 with an empty body</translation>
        </message>
        <message>
            <source>真踩过「200 + image/jpeg + 空体」。客户端必须按体长与魔数判定而不是状态码。</source>
            <translation>Seen in the field: 200, image/jpeg, and nothing in the body. Clients must judge by length and magic bytes, not by the status code.</translation>
        </message>
        <message>
            <source>坏响应形态</source>
            <translation>Broken response shape</translation>
        </message>
        <message>
            <source>快照鉴权方式</source>
            <translation>Snapshot authentication scheme</translation>
        </message>
        <message>
            <source>客户端按 Digest → Basic → 无 顺序试，且只在 401 时才换下一种。</source>
            <translation>Clients try Digest, then Basic, then none, switching only on a 401.</translation>
        </message>
        <message>
            <source>鉴权方式；token=URI 里带一次性令牌</source>
            <translation>Scheme; token = a one-shot token carried in the URI</translation>
        </message>
        <message>
            <source>快照 URI 定期失效</source>
            <translation>Snapshot URI expires periodically</translation>
        </message>
        <message>
            <source>客户端连续 3 次取图失败才判 URI 失效并重探，中间会盲取。</source>
            <translation>Clients only re-probe after three consecutive failures, fetching blindly in between.</translation>
        </message>
        <message>
            <source>URI 有效期（秒）</source>
            <translation>URI lifetime (seconds)</translation>
        </message>
        <message>
            <source>不实现 GetSnapshotUri</source>
            <translation>GetSnapshotUri not implemented</translation>
        </message>
        <message>
            <source>回 ActionNotSupported，客户端应退化为从码流抽帧或干脆没有快照。</source>
            <translation>Returns ActionNotSupported; the client should fall back to grabbing a frame from the stream, or do without snapshots.</translation>
        </message>
        <message>
            <source>声明与实发编码不一致</source>
            <translation>Declared codec differs from the one sent</translation>
        </message>
        <message>
            <source>很多家用机 capability 列了 G.711 / G.722 / AAC，backchannel SDP 却只有 PCMU。运行时必须以 SDP 为准。</source>
            <translation>Many consumer devices advertise G.711 / G.722 / AAC in their capabilities while the backchannel SDP offers only PCMU. At runtime the SDP wins.</translation>
        </message>
        <message>
            <source>ONVIF 里声明的音频编码</source>
            <translation>Audio codec declared in ONVIF</translation>
        </message>
        <message>
            <source>SDP 与 RTP 里实际用的编码</source>
            <translation>Codec actually used in SDP and RTP</translation>
        </message>
        <message>
            <source>声明分辨率与码流不符</source>
            <translation>Declared resolution differs from the stream</translation>
        </message>
        <message>
            <source>客户端不读 VideoEncoderConfiguration、全靠 ffprobe 探流，这条用来测两边不一致时它信谁。</source>
            <translation>Clients that ignore VideoEncoderConfiguration and probe the stream with ffprobe instead; this shows which side they trust when the two disagree.</translation>
        </message>
        <message>
            <source>PTZ</source>
            <translation>PTZ</translation>
        </message>
        <message>
            <source>PTZConfiguration 只挂子码流</source>
            <translation>PTZConfiguration on the sub stream only</translation>
        </message>
        <message>
            <source>只看主码流 profile 的客户端会判定「没有 PTZ」。</source>
            <translation>A client that only inspects the main-stream profile concludes there is no PTZ.</translation>
        </message>
        <message>
            <source>不挂 PTZConfiguration 但 PTZ 可用</source>
            <translation>No PTZConfiguration, yet PTZ works</translation>
        </message>
        <message>
            <source>所有 profile 都不带 PTZConfiguration，但 GetNodes 非空且能真转。客户端应回落到 GetNodes 判定。</source>
            <translation>No profile carries a PTZConfiguration, but GetNodes is non-empty and the camera really moves. Clients should fall back to GetNodes.</translation>
        </message>
        <message>
            <source>SupportedPTZSpaces 为空</source>
            <translation>Empty SupportedPTZSpaces</translation>
        </message>
        <message>
            <source>能力声明四档之一：完整 / 为空 / 只有 Default*Space / 只有 pan。</source>
            <translation>One of four capability shapes seen in the field: complete, empty, only Default*Space, or pan only.</translation>
        </message>
        <message>
            <source>声明档位</source>
            <translation>Declaration shape</translation>
        </message>
        <message>
            <source>PTZ Range 的 Min == Max</source>
            <translation>PTZ Range with Min == Max</translation>
        </message>
        <message>
            <source>除零或退化区间会让客户端的归一化计算炸掉。</source>
            <translation>A zero-width range makes the client's normalisation blow up.</translation>
        </message>
        <message>
            <source>非零 Zoom 触发畸形响应</source>
            <translation>Non-zero Zoom triggers a malformed reply</translation>
        </message>
        <message>
            <source>真机在 ContinuousMove 带非零 Zoom 时回显请求字节 + 500。</source>
            <translation>A real device echoes the request bytes back with a 500 when ContinuousMove carries a non-zero Zoom.</translation>
        </message>
        <message>
            <source>出厂预填 300 个预置位</source>
            <translation>300 factory preset slots</translation>
        </message>
        <message>
            <source>「预置点 1..300」加巡航扫描 / 远程重启等功能槽，共享同一个假 PTZPosition。用来压客户端的预置位列表 UI。</source>
            <translation>“Preset 1..300” plus function slots such as tour scan and remote reboot, all sharing one fake PTZPosition. Good for stress-testing the client's preset list UI.</translation>
        </message>
        <message>
            <source>预置位数量</source>
            <translation>Number of presets</translation>
        </message>
        <message>
            <source>预置位名百分号编码</source>
            <translation>Percent-encoded preset names</translation>
        </message>
        <message>
            <source>相机把 %E9%A2%84 这类编码原样吐回，客户端要 unquote 才显示得对。</source>
            <translation>The camera returns percent-encoded names such as %E9%A2%84 verbatim; the client has to unquote them to display them correctly.</translation>
        </message>
        <message>
            <source>不实现 GetPresets</source>
            <translation>GetPresets not implemented</translation>
        </message>
        <message>
            <source>回 ActionNotSupported，或回一段 not implemented 文本。</source>
            <translation>Returns ActionNotSupported, or a plain “not implemented” text.</translation>
        </message>
        <message>
            <source>不支持的表现形式</source>
            <translation>How the refusal is expressed</translation>
        </message>
        <message>
            <source>SetPreset 返回形态</source>
            <translation>SetPreset response shape</translation>
        </message>
        <message>
            <source>有的固件返回裸字符串 token，有的返回带 PresetToken 的对象。</source>
            <translation>Some firmware returns a bare token string, some an object carrying PresetToken.</translation>
        </message>
        <message>
            <source>返回形态</source>
            <translation>Response shape</translation>
        </message>
        <message>
            <source>PTZ 响应延迟抖动</source>
            <translation>Jittery PTZ response latency</translation>
        </message>
        <message>
            <source>100~800ms 随机延迟，用来验证客户端的命令乱序防护。</source>
            <translation>A random 100-800 ms delay, to check the client's protection against out-of-order commands.</translation>
        </message>
        <message>
            <source>最小延迟</source>
            <translation>Minimum delay</translation>
        </message>
        <message>
            <source>最大延迟</source>
            <translation>Maximum delay</translation>
        </message>
        <message>
            <source>GotoPreset 特别慢</source>
            <translation>GotoPreset is very slow</translation>
        </message>
        <message>
            <source>响应正常但机械动作要好几秒，GetStatus 期间一直 MOVING。</source>
            <translation>The response is immediate but the mechanics take seconds; GetStatus reports MOVING throughout.</translation>
        </message>
        <message>
            <source>到位耗时</source>
            <translation>Time to reach the preset</translation>
        </message>
        <message>
            <source>移动不反映到 GetStatus</source>
            <translation>Movement never shows up in GetStatus</translation>
        </message>
        <message>
            <source>命令返回成功但 GetStatus 的 Position 永远不动，客户端无法闭环。</source>
            <translation>Commands succeed but the Position in GetStatus never changes, so the client cannot close the loop.</translation>
        </message>
        <message>
            <source>事件</source>
            <translation>Events</translation>
        </message>
        <message>
            <source>订阅管理器换独立端口且递增</source>
            <translation>Subscription manager on its own, incrementing port</translation>
        </message>
        <message>
            <source>真机形如 :1024/event-1024_1024，下一条订阅换 :1025。客户端必须用返回的订阅地址而不是主服务地址去 Pull。</source>
            <translation>Real devices answer with something like :1024/event-1024_1024 and move to :1025 for the next subscription. Clients must pull from the returned address, not from the main service address.</translation>
        </message>
        <message>
            <source>起始端口</source>
            <translation>First port</translation>
        </message>
        <message>
            <source>订阅地址 host 不可达</source>
            <translation>Unreachable host in the subscription address</translation>
        </message>
        <message>
            <source>固件把内网地址写进订阅 URL，客户端要么接管 host 要么彻底卡住。</source>
            <translation>The firmware writes its own internal address into the subscription URL; clients either override the host or hang completely.</translation>
        </message>
        <message>
            <source>写进订阅地址的 host</source>
            <translation>Host written into the subscription address</translation>
        </message>
        <message>
            <source>订阅槽位上限</source>
            <translation>Subscription slot limit</translation>
        </message>
        <message>
            <source>配合 A9（只增不回收）就是真实的槽位耗尽故障。</source>
            <translation>Together with A9 (subscriptions never reclaimed) this is the real-world slot exhaustion failure.</translation>
        </message>
        <message>
            <source>最大并发订阅数</source>
            <translation>Maximum concurrent subscriptions</translation>
        </message>
        <message>
            <source>超限行为</source>
            <translation>Behaviour past the limit</translation>
        </message>
        <message>
            <source>不实现 GetEventProperties</source>
            <translation>GetEventProperties not implemented</translation>
        </message>
        <message>
            <source>客户端拿不到 TopicSet，只能盲订阅全部。</source>
            <translation>Without a TopicSet the client can only subscribe to everything blindly.</translation>
        </message>
        <message>
            <source>GetEventProperties 返回非法 XML</source>
            <translation>GetEventProperties returns invalid XML</translation>
        </message>
        <message>
            <source>TP-Link TL-IPC 真机返回属性值不加引号的 XML（wstop:topic=true）。客户端必须先补引号再解析。</source>
            <translation>A real TP-Link TL-IPC returns XML with unquoted attribute values (wstop:topic=true). Clients have to add the quotes before parsing.</translation>
        </message>
        <message>
            <source>订阅 Address 不套 Reference</source>
            <translation>Subscription Address without the Reference wrapper</translation>
        </message>
        <message>
            <source>Address 直接放响应下，不包 SubscriptionReference 一层。</source>
            <translation>Address sits directly under the response instead of inside SubscriptionReference.</translation>
        </message>
        <message>
            <source>Topic 命名风格</source>
            <translation>Topic naming style</translation>
        </message>
        <message>
            <source>标准 ONVIF / TP-Link（LineCrossDetector）/ Reolink（MyRuleDetector）/ Axis（tnsaxis: 前缀）四套。</source>
            <translation>Four schemes: standard ONVIF, TP-Link (LineCrossDetector), Reolink (MyRuleDetector) and Axis (tnsaxis: prefix).</translation>
        </message>
        <message>
            <source>PullMessages 永远返回空</source>
            <translation>PullMessages always returns empty</translation>
        </message>
        <message>
            <source>订阅建得起来、Pull 也不报错，就是永远没有事件。最难查的一类故障。</source>
            <translation>The subscription is created, Pull never errors, and no event ever arrives. One of the hardest failures to diagnose.</translation>
        </message>
        <message>
            <source>属性型事件不发配对的 false</source>
            <translation>Property events never send the matching false</translation>
        </message>
        <message>
            <source>正常应成对发 IsMotion=true / false；只发 true 会让客户端的移动侦测永远不复位。</source>
            <translation>IsMotion should arrive as true then false; sending only true leaves the client's motion state stuck on.</translation>
        </message>
        <message>
            <source>Renew 总是失败</source>
            <translation>Renew always fails</translation>
        </message>
        <message>
            <source>客户端必须能在 Renew 失败后重建订阅而不是放弃。</source>
            <translation>Clients must recreate the subscription after a failed Renew rather than give up.</translation>
        </message>
        <message>
            <source>订阅建完立刻过期</source>
            <translation>Subscription expires immediately</translation>
        </message>
        <message>
            <source>CreatePullPointSubscription 成功但 TerminationTime 已是过去时间。</source>
            <translation>CreatePullPointSubscription succeeds but the TerminationTime is already in the past.</translation>
        </message>
        <message>
            <source>事件风暴</source>
            <translation>Event storm</translation>
        </message>
        <message>
            <source>每秒推 N 条事件，压客户端的队列与 UI。</source>
            <translation>Pushes N events per second to stress the client's queue and UI.</translation>
        </message>
        <message>
            <source>每秒事件数</source>
            <translation>Events per second</translation>
        </message>
        <message>
            <source>不支持 SetSynchronizationPoint</source>
            <translation>SetSynchronizationPoint not supported</translation>
        </message>
        <message>
            <source>客户端拿不到属性型 topic 的当前状态，只能等下一次变化。</source>
            <translation>The client cannot read the current state of property topics and must wait for the next change.</translation>
        </message>
        <message>
            <source>RTSP 与对讲</source>
            <translation>RTSP and talkback</translation>
        </message>
        <message>
            <source>音频能力谎标</source>
            <translation>Audio capability lies</translation>
        </message>
        <message>
            <source>ONVIF 声明的解码能力与 backchannel SDP 实际开放的 codec 不一致。客户端必须以 SDP 为准。</source>
            <translation>The decoding capability declared over ONVIF does not match the codec the backchannel SDP actually offers. The SDP is what counts.</translation>
        </message>
        <message>
            <source>G.722 采样率错写成 8000</source>
            <translation>G.722 sample rate mis-stated as 8000</translation>
        </message>
        <message>
            <source>G.722 的 RTP 时钟率按规范写 8000 但实际是 16000，很多固件在 ONVIF 能力里也错写成 8000。客户端应强制按 16000 处理。</source>
            <translation>G.722's RTP clock rate is 8000 by specification while the real sample rate is 16000, and much firmware repeats the 8000 in its ONVIF capabilities. Clients should force 16000.</translation>
        </message>
        <message>
            <source>码率单位变体</source>
            <translation>Bitrate unit variants</translation>
        </message>
        <message>
            <source>有的固件按 kbps 报，有的按 bps 报，差 1000 倍。</source>
            <translation>Some firmware reports kbps, some bps — a factor of 1000 apart.</translation>
        </message>
        <message>
            <source>码率字段单位</source>
            <translation>Unit of the bitrate field</translation>
        </message>
        <message>
            <source>GetAudioDecoderConfigurationOptions 响应形态</source>
            <translation>GetAudioDecoderConfigurationOptions response shape</translation>
        </message>
        <message>
            <source>形态 A：G711/G722/G726/AAC 子元素直挂 opts；形态 B：AudioDecoderOptions 列表，每项带 Encoding。两种都得支持。</source>
            <translation>Shape A hangs G711/G722/G726/AAC elements directly off the options; shape B returns an AudioDecoderOptions list where each entry carries an Encoding. Both occur in the field.</translation>
        </message>
        <message>
            <source>a=子元素直挂，b=Options 列表</source>
            <translation>a = direct child elements, b = Options list</translation>
        </message>
        <message>
            <source>采样率字段形态</source>
            <translation>Sample-rate field shape</translation>
        </message>
        <message>
            <source>SampleRateRange / SampleRateList / 单值三种形态都在真机上出现过。</source>
            <translation>SampleRateRange, SampleRateList and a single value have all been seen on real devices.</translation>
        </message>
        <message>
            <source>字段形态</source>
            <translation>Field shape</translation>
        </message>
        <message>
            <source>对讲双轨布局</source>
            <translation>Two-track talkback layout</translation>
        </message>
        <message>
            <source>海康是单轨（只有 sendonly 对讲轨）；大华是双轨（麦克风 recvonly 在前 + 对讲 sendonly 在后），客户端要挑对轨才推得进去。</source>
            <translation>Hikvision offers a single sendonly talkback track; Dahua offers two (a recvonly microphone first, then the sendonly talkback). The client has to pick the right one to push audio at all.</translation>
        </message>
        <message>
            <source>对讲忙槽位</source>
            <translation>Talkback slot stays busy</translation>
        </message>
        <message>
            <source>TEARDOWN 后 N 秒内对新的 backchannel DESCRIBE 回 401。海康球机实测 3~4 秒。客户端必须重试而不是判定凭据错误。</source>
            <translation>For N seconds after a TEARDOWN a new backchannel DESCRIBE gets 401. Measured at 3-4 s on Hikvision PTZ domes. Clients must retry instead of concluding the credentials are wrong.</translation>
        </message>
        <message>
            <source>忙槽位持续秒数</source>
            <translation>How long the slot stays busy (seconds)</translation>
        </message>
        <message>
            <source>同时发 Basic + Digest 挑战</source>
            <translation>Offer Basic and Digest together</translation>
        </message>
        <message>
            <source>两条 WWW-Authenticate，顺序可配。有的客户端只看第一条。</source>
            <translation>Two WWW-Authenticate headers in a configurable order. Some clients only look at the first.</translation>
        </message>
        <message>
            <source>两条挑战的顺序</source>
            <translation>Order of the two challenges</translation>
        </message>
        <message>
            <source>严格 Digest 参数校验</source>
            <translation>Strict Digest parameter checking</translation>
        </message>
        <message>
            <source>客户端 Authorization 里出现挑战没给过的参数（如 algorithm=MD5）就 401。真机上很多客户端因此连不上。</source>
            <translation>Any parameter in the client's Authorization that the challenge did not offer (algorithm=MD5, say) gets a 401. This keeps a lot of clients out of real devices.</translation>
        </message>
        <message>
            <source>SDP 声明不规范 codec</source>
            <translation>Non-standard codec name in SDP</translation>
        </message>
        <message>
            <source>G7221 / G726-32 这类写法，客户端的 codec 表匹配不上要能优雅降级。</source>
            <translation>Spellings such as G7221 or G726-32 that miss the client's codec table; it should degrade gracefully.</translation>
        </message>
        <message>
            <source>写进 a=rtpmap 的编码名</source>
            <translation>Codec name written into a=rtpmap</translation>
        </message>
        <message>
            <source>SDP 不写 rtpmap</source>
            <translation>SDP without rtpmap</translation>
        </message>
        <message>
            <source>只给静态 payload type（0=PCMU / 8=PCMA），客户端要按 RFC 3551 静态表推断。</source>
            <translation>Only a static payload type is given (0 = PCMU, 8 = PCMA); clients must infer the codec from the RFC 3551 table.</translation>
        </message>
        <message>
            <source>只有 session 级 a=control</source>
            <translation>Session-level a=control only</translation>
        </message>
        <message>
            <source>媒体级没有 control 属性，客户端拼 SETUP URL 时只能用 session 级的。</source>
            <translation>No media-level control attribute, so the client has to build the SETUP URL from the session-level one.</translation>
        </message>
        <message>
            <source>校验 talkspurt 首包 marker</source>
            <translation>Require a marker on the first talkspurt packet</translation>
        </message>
        <message>
            <source>相机要求每段话首包置 RTP marker 位，否则整段静音丢弃。</source>
            <translation>The camera expects the RTP marker bit on the first packet of each talkspurt and silently drops the rest otherwise.</translation>
        </message>
        <message>
            <source>SETUP 响应不带 Session 头</source>
            <translation>SETUP response without a Session header</translation>
        </message>
        <message>
            <source>违反 RFC 2326，客户端后续 PLAY 无 Session 可用。</source>
            <translation>Violates RFC 2326 and leaves the client with no Session for the following PLAY.</translation>
        </message>
        <message>
            <source>忽略 backchannel Require 头</source>
            <translation>Ignore the backchannel Require header</translation>
        </message>
        <message>
            <source>不带 Require 也返回含 sendonly 的 SDP，或严格按 RFC 2326 对不支持的 Require 回 551。</source>
            <translation>Either return SDP with a sendonly track even without Require, or follow RFC 2326 strictly and answer 551 for an unsupported Require.</translation>
        </message>
        <message>
            <source>ignore=不看 Require 照给，strict_551=不支持就 551</source>
            <translation>ignore = hand it over regardless, strict_551 = answer 551 when unsupported</translation>
        </message>
        <message>
            <source>不实现 AudioOutput/Decoder 配置</source>
            <translation>AudioOutput/Decoder configuration not implemented</translation>
        </message>
        <message>
            <source>AddAudioOutputConfiguration / AddAudioDecoderConfiguration 整体回 ActionNotSupported，客户端只能直接开 backchannel 试。</source>
            <translation>AddAudioOutputConfiguration and AddAudioDecoderConfiguration both return ActionNotSupported, leaving the client to open the backchannel and hope.</translation>
        </message>
        <message>
            <source>对讲推流期停止排空</source>
            <translation>Stop draining during talkback</translation>
        </message>
        <message>
            <source>相机不再读 TCP 接收缓冲，客户端的 sendall 卡死在内核缓冲区满。</source>
            <translation>The camera stops reading its TCP receive buffer, so the client's sendall blocks once the kernel buffer fills.</translation>
        </message>
        <message>
            <source>推流开始后多久停止排空</source>
            <translation>How long after PLAY draining stops</translation>
        </message>
        <message>
            <source>RTSP 并发会话上限</source>
            <translation>RTSP concurrent session limit</translation>
        </message>
        <message>
            <source>廉价相机常态：超过上限回 453 Not Enough Bandwidth。</source>
            <translation>Routine on cheap cameras: past the limit they answer 453 Not Enough Bandwidth.</translation>
        </message>
        <message>
            <source>最大并发会话数</source>
            <translation>Maximum concurrent sessions</translation>
        </message>
        <message>
            <source>周期性主动断流</source>
            <translation>Periodically tear the stream down</translation>
        </message>
        <message>
            <source>每 N 秒相机自己 TEARDOWN，客户端必须能自动重连。</source>
            <translation>The camera issues its own TEARDOWN every N seconds; the client has to reconnect on its own.</translation>
        </message>
        <message>
            <source>断流间隔</source>
            <translation>Interval between teardowns</translation>
        </message>
        <message>
            <source>RTP 丢包</source>
            <translation>RTP packet loss</translation>
        </message>
        <message>
            <source>按比例随机丢弃 RTP 包，测客户端的花屏恢复。</source>
            <translation>Drops RTP packets at random to test how the client recovers from corruption.</translation>
        </message>
        <message>
            <source>丢包率（%）</source>
            <translation>Loss rate (%)</translation>
        </message>
        <message>
            <source>RTP 时间戳跳变</source>
            <translation>RTP timestamp jumps</translation>
        </message>
        <message>
            <source>周期性让时间戳大幅跳跃，测客户端的抖动缓冲。</source>
            <translation>Makes the timestamp jump periodically to test the client's jitter buffer.</translation>
        </message>
        <message>
            <source>跳变间隔</source>
            <translation>Interval between jumps</translation>
        </message>
        <message>
            <source>跳变量（毫秒，可负）</source>
            <translation>Jump size (milliseconds, may be negative)</translation>
        </message>
        <message>
            <source>SPS/PPS 位置</source>
            <translation>Where SPS/PPS live</translation>
        </message>
        <message>
            <source>只在 SDP 的 sprop-parameter-sets / 只在带内 / 两处都有。只认一处的解码器会黑屏。</source>
            <translation>Only in the SDP's sprop-parameter-sets, only in band, or both. A decoder that looks in one place only shows a black picture.</translation>
        </message>
        <message>
            <source>放置策略</source>
            <translation>Placement</translation>
        </message>
        <message>
            <source>画面冻结</source>
            <translation>Frozen picture</translation>
        </message>
        <message>
            <source>RTP 继续发但画面内容不变，测客户端的冻结检测。</source>
            <translation>RTP keeps flowing but the picture never changes, testing freeze detection.</translation>
        </message>
        <message>
            <source>黑屏</source>
            <translation>Black picture</translation>
        </message>
        <message>
            <source>切换到全黑样片，测客户端的黑屏检测。</source>
            <translation>Switches to an all-black clip to test black-frame detection.</translation>
        </message>
        <message>
            <source>帧率突变</source>
            <translation>Frame rate changes abruptly</translation>
        </message>
        <message>
            <source>实际帧率与声明不符且中途变化。</source>
            <translation>The real frame rate does not match the declared one and changes part way through.</translation>
        </message>
        <message>
            <source>实际帧率</source>
            <translation>Actual frame rate</translation>
        </message>
        <message>
            <source>不发 RTCP SR</source>
            <translation>No RTCP SR</translation>
        </message>
        <message>
            <source>客户端拿不到 NTP 映射，音视频同步只能靠 RTP 时间戳硬估。</source>
            <translation>Without the NTP mapping the client can only guess at A/V sync from RTP timestamps.</translation>
        </message>
        <message>
            <source>传输与设备</source>
            <translation>Transport and device</translation>
        </message>
        <message>
            <source>畸形 HTTP 响应</source>
            <translation>Malformed HTTP response</translation>
        </message>
        <message>
            <source>回显请求原始字节 + 500，或缺 Content-Length、状态行残缺。</source>
            <translation>Echo the raw request bytes with a 500, omit Content-Length, or truncate the status line.</translation>
        </message>
        <message>
            <source>畸形形式</source>
            <translation>Kind of malformation</translation>
        </message>
        <message>
            <source>触发概率（%）</source>
            <translation>Trigger probability (%)</translation>
        </message>
        <message>
            <source>自签证书 HTTPS</source>
            <translation>Self-signed HTTPS</translation>
        </message>
        <message>
            <source>TP-Link VIGI 的私有 API 走 20443 自签 HTTPS，客户端必须跳过证书校验。</source>
            <translation>TP-Link VIGI's private API listens on 20443 with a self-signed certificate, so clients have to skip verification.</translation>
        </message>
        <message>
            <source>HTTPS 端口</source>
            <translation>HTTPS port</translation>
        </message>
        <message>
            <source>并发过载时模拟重启</source>
            <translation>Pretend to reboot when overloaded</translation>
        </message>
        <message>
            <source>一秒内的 SOAP 请求数超过阈值就整机假死再回来，复现廉价固件被打挂。按速率而不是瞬时并发判定：单事件循环下请求本来就是串行处理的。</source>
            <translation>Once the SOAP request rate passes the threshold the device plays dead and comes back, reproducing cheap firmware buckling under load. Judged by rate rather than instantaneous concurrency, because a single event loop handles requests serially anyway.</translation>
        </message>
        <message>
            <source>每秒请求数阈值</source>
            <translation>Requests-per-second threshold</translation>
        </message>
        <message>
            <source>假死秒数</source>
            <translation>Seconds spent playing dead</translation>
        </message>
        <message>
            <source>SystemReboot 真的离线</source>
            <translation>SystemReboot really goes offline</translation>
        </message>
        <message>
            <source>发 Bye → 关掉全部端口 N 秒 → 发 Hello 回来，而不是只回个 OK。</source>
            <translation>Sends Bye, closes every port for N seconds, then comes back with Hello, instead of merely answering OK.</translation>
        </message>
        <message>
            <source>离线秒数</source>
            <translation>Seconds offline</translation>
        </message>
        <message>
            <source>随机掉线</source>
            <translation>Random dropouts</translation>
        </message>
        <message>
            <source>按概率整机离线一小段时间，模拟不稳定的 PoE / WiFi。</source>
            <translation>Takes the whole device offline briefly at random, imitating flaky PoE or Wi-Fi.</translation>
        </message>
        <message>
            <source>每次请求触发掉线的概率（%）</source>
            <translation>Probability per request (%)</translation>
        </message>
        <message>
            <source>每次掉线秒数</source>
            <translation>Seconds per dropout</translation>
        </message>
        <message>
            <source>超大响应体</source>
            <translation>Oversized response body</translation>
        </message>
        <message>
            <source>塞进大量填充，测客户端的解析上限与内存。</source>
            <translation>Pads the body heavily to test the client's parse limits and memory.</translation>
        </message>
        <message>
            <source>响应体大小（KB）</source>
            <translation>Body size (KB)</translation>
        </message>
        <message>
            <source>慢发送</source>
            <translation>Slow send</translation>
        </message>
        <message>
            <source>响应分成小块、每块之间等一会儿，测客户端的读超时。</source>
            <translation>Splits the response into small chunks with a pause between them, testing read timeouts.</translation>
        </message>
        <message>
            <source>每块字节数</source>
            <translation>Bytes per chunk</translation>
        </message>
        <message>
            <source>块间隔</source>
            <translation>Interval between chunks</translation>
        </message>
        <message>
            <source>整机响应延迟</source>
            <translation>Device-wide response delay</translation>
        </message>
        <message>
            <source>所有 HTTP 响应统一延迟，模拟远端 / 弱网相机。</source>
            <translation>Delays every HTTP response, imitating a distant camera on a poor link.</translation>
        </message>
        <message>
            <source>RTP 限速</source>
            <translation>RTP rate limit</translation>
        </message>
        <message>
            <source>按给定带宽发 RTP，低于码流需求就会持续积压。</source>
            <translation>Sends RTP at the given bandwidth; below what the stream needs the backlog grows without bound.</translation>
        </message>
        <message>
            <source>上限带宽（kbps）</source>
            <translation>Bandwidth cap (kbps)</translation>
        </message>
    </context>
</TS>