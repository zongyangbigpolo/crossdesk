# CrossDesk Flutter UI 重构设计文档

> 目标：用 Flutter 重写 CrossDesk 桌面端的"外壳 UI"（主窗口、设置、文件传输、托盘等），同时把"远程会话窗口（Session Window）"剥离为一个独立的原生进程，由 Flutter 主程序按需拉起，从而既获得 Flutter 现代化界面，又保留远程画面渲染在高刷新率/低延迟下的性能。

---

## 1. 现状分析

### 1.1 当前架构（C++ / SDL3 + Dear ImGui 单进程）

主要源码位于 [src/](../src/)，关键模块：

| 模块 | 路径 | 职责 |
|---|---|---|
| 入口 | [app/main.cpp](../src/app/main.cpp), [app/daemon.cpp](../src/app/daemon.cpp) | 进程入口、服务安装/卸载、Daemon 初始化 |
| 渲染 / UI 总控 | [gui/render.cpp](../src/gui/render.cpp) (~2955 行), [gui/render.h](../src/gui/render.h) (~749 行) | 唯一的 `Render` 类，混合了窗口管理、视频纹理上传、输入分发、状态机 |
| 主界面窗口 | [gui/windows/main_window.cpp](../src/gui/windows/main_window.cpp), [gui/windows/main_settings_window.cpp](../src/gui/windows/main_settings_window.cpp), [gui/windows/server_window.cpp](../src/gui/windows/server_window.cpp), [gui/windows/about_window.cpp](../src/gui/windows/about_window.cpp), [gui/windows/update_notification_window.cpp](../src/gui/windows/update_notification_window.cpp) | 主控制台、设置、服务端窗口、关于、升级提示 |
| 会话窗口 | [gui/windows/stream_window.cpp](../src/gui/windows/stream_window.cpp), [gui/windows/control_window.cpp](../src/gui/windows/control_window.cpp), [gui/windows/connection_status_window.cpp](../src/gui/windows/connection_status_window.cpp), [gui/windows/file_transfer_window.cpp](../src/gui/windows/file_transfer_window.cpp) | 远程画面、悬浮控制条、连接状态、文件传输 |
| 面板/工具栏 | [gui/panels/](../src/gui/panels/), [gui/toolbars/](../src/gui/toolbars/) | 本地/远程对端面板、最近连接、标题栏、控制条 |
| 托盘 | [gui/tray/win_tray.cpp](../src/gui/tray/win_tray.cpp) | Windows 任务栏托盘 |
| 渲染回调 | [gui/render_callback.cpp](../src/gui/render_callback.cpp) (~1613 行) | minirtc 视频/音频/数据回调、输入事件 → RemoteAction 转换 |
| RTC 内核 | [submodules/minirtc/](../submodules/minirtc/) | P2P 传输、H264/AV1 编解码、SRTP、Opus、信令 |
| 屏幕采集 | [screen_capturer/](../src/screen_capturer/) | Windows / macOS / Linux 屏幕采集 |
| 设备控制 | [device_controller/](../src/device_controller/) | 键鼠注入、键码转换 |
| 配置 | [config_center/](../src/config_center/) | INI 配置中心 |
| 系统服务 | [service/](../src/service/) | Windows 服务（SAS、UAC 桌面切换） |

### 1.2 关键的性能/耦合事实

- 视频帧路径：minirtc 解码线程 → `Render::OnReceiveVideoBufferCb` ([render_callback.cpp:727](../src/gui/render_callback.cpp#L727)) → NV12 双缓冲 → 主线程 `SDL_Texture* stream_texture_` 上传 → ImGui `ImageButton` 显示。
- 输入路径：SDL3 事件 → ImGui 处理 → `Render` 取热区光标坐标 → `RemoteAction` JSON → minirtc DataChannel。
- 每个远程会话的状态封装在 `SubStreamWindowProperties` ([render.h:88+](../src/gui/render.h#L88))，包含 `PeerPtr*`、纹理、缓冲、控制条状态、文件传输状态等，由 `client_properties_` map 管理（**支持多路并发会话**）。
- `Render` 类同时持有：主面板状态、设置窗口状态、最近连接、托盘、自动更新、所有会话——典型的"上帝类"。

### 1.3 痛点

1. ImGui 即时模式风格使界面定制（动效、阴影、主题、响应式布局）非常难。
2. 主界面与会话画面共用一个 SDL3 窗口循环 + 一个渲染线程，刷新率耦合。
3. 跨平台一致性差（托盘是 Windows-only；macOS 部分用了 .mm）。
4. 国际化、设计稿迭代成本高（自研 `localization_data.h`）。

---

## 2. 重构目标

### 2.1 必须达到

1. **主界面/设置/对端管理/文件传输/托盘** 全部由 **Flutter Desktop** 实现，跨 Windows / macOS / Linux 一致。
2. **远程会话窗口（Session Window）保留原生实现**（SDL3 + 现有视频管线），作为**独立可执行文件** `crossdesk_session`，由 Flutter 主程序按需 spawn。
3. 既有 RTC 能力（minirtc、屏幕采集、设备控制、文件传输、Windows 服务）**不重写**，以稳定 C ABI 共享库形式被 Flutter 与 Session Window 复用。
4. 不破坏现有 Web 客户端、服务端二进制的协议兼容性。

### 2.2 性能预算

- 主界面冷启动 ≤ 1.5s，常驻内存 ≤ 200MB（不含会话）。
- 单路会话端到端延迟、CPU/GPU 占用不劣于当前 ImGui 版本（≤ 5% 浮动）。
- 60fps 视频流下，Session Window 主线程不允许 > 8ms 卡顿。

### 2.3 非目标

- 不重写 minirtc、不动 [submodules/minirtc/](../submodules/minirtc/)。
- 不改 Web Client。
- 不改信令协议、不改远程控制协议（`RemoteAction` JSON）。

---

## 3. 总体架构

### 3.1 进程拓扑

```
┌───────────────────────────────────────────────────────────────┐
│                  crossdesk_shell  (Flutter Desktop)            │
│   - 主窗口 / 设置 / 最近连接 / 文件传输面板 / 托盘 / 升级提示  │
│   - 在线状态、信令登录、配置持久化                              │
│                                                                 │
│   Dart ──FFI──> libcrossdesk_core.{dll|dylib|so}               │
│                                 │                               │
│                                 │ (信令、presence、配置、文件)   │
│                                 ▼                               │
│                          minirtc + 业务库                       │
└───────────────┬───────────────────────────────────────────────┘
                │ spawn + IPC (UDS / Named Pipe, JSON-RPC)
                ▼
┌───────────────────────────────────────────────────────────────┐
│           crossdesk_session  (一连接一进程, 原生 C++)           │
│   - SDL3 窗口、视频纹理、控制条 (ImGui 极简)、输入捕获          │
│   - 文件接收落盘、剪贴板桥接                                    │
│                                                                 │
│   静态/动态链接 libcrossdesk_core, 直接持有 PeerPtr             │
└───────────────────────────────────────────────────────────────┘
                ▲
                │ 系统服务 IPC (Windows Service Pipe, 已存在)
                ▼
       crossdesk_service.exe (Windows only, 已存在, 不动)
```

要点：
- **每个远程连接 = 一个 `crossdesk_session` 子进程**。Flutter 仅负责"发起连接"和"管控生命周期"；建立 PeerConnection、解码、渲染、输入注入全部由子进程闭环完成。
- **不在进程之间传视频帧**——这是规避 Flutter 渲染瓶颈的核心：解码后的 NV12/RGB 纹理永远不离开 Session Window 进程，避免 IPC 拷贝/同步开销。
- Flutter 与 Session Window 之间仅传 **控制消息**（连接成功、码率、丢包、断开、用户在控制条点了什么按钮等）。

### 3.2 模块切分

| 新模块 | 形态 | 复用现有代码 |
|---|---|---|
| `libcrossdesk_core` | C ABI 共享库 | minirtc 封装、`device_presence`、`config_center`、`screen_capturer`、`device_controller`、`thumbnail`、`version_checker`、`path_manager`、`speaker_capturer`、`service_host` |
| `crossdesk_shell` | Flutter Desktop 应用 | 全新 |
| `crossdesk_session` | 原生 C++ exe | 抽取 [stream_window.cpp](../src/gui/windows/stream_window.cpp) + [control_window.cpp](../src/gui/windows/control_window.cpp) + [render_callback.cpp](../src/gui/render_callback.cpp) 中视频/输入相关逻辑，去掉主界面相关代码 |
| `crossdesk_service` | Windows 服务 exe | [service/](../src/service/) 直接复用 |
| `crossdesk_ipc` | header-only C++ + Dart | 新增，统一定义 IPC schema |

### 3.3 选型说明

| 项 | 选择 | 理由 |
|---|---|---|
| Flutter Desktop 版本 | 3.22+ stable | Windows/macOS/Linux 已 stable，支持 Impeller (macOS/iOS)，CMake 集成成熟 |
| Dart ↔ C++ | `dart:ffi` + `package:ffigen` | 零依赖、性能好；避免引入 Platform Channel 的线程跳转成本 |
| 状态管理 | `riverpod` (推荐) | 编译期安全、易测；备选 `bloc` |
| IPC 序列化 | JSON (复用 nlohmann) + 长度前缀帧 | 与 `RemoteAction` 一致，调试方便；高频路径（如 mouse move）可后续切 FlatBuffers |
| IPC 传输 | Unix Domain Socket / Windows Named Pipe | 简单、跨平台、Flutter 侧通过 FFI 调底层 |
| 托盘 | `tray_manager` Flutter 包；Windows 仍可在 shell 进程 host 现有 [win_tray.cpp](../src/gui/tray/win_tray.cpp) | 跨平台 |
| Session Window 内部 UI | 继续 SDL3 + 现有 ImGui 控制条 | 控制条不是性能瓶颈，重写没收益；仅删主面板部分 |

---

## 4. `libcrossdesk_core` C ABI 设计

> 把现在散布在 `Render` 类里的"非渲染逻辑"沉降到一个共享库，Flutter 和 Session Window 都基于它工作。

### 4.1 命名空间划分

```c
// crossdesk_core.h  (节选)

typedef struct cd_core cd_core_t;
typedef struct cd_peer cd_peer_t;
typedef uint64_t cd_token_t;

// ---- lifecycle ----
cd_core_t* cd_core_create(const char* config_dir);
void       cd_core_destroy(cd_core_t*);

// ---- signaling / presence ----
int  cd_core_login(cd_core_t*, const char* server, const char* device_id);
void cd_core_set_presence_cb(cd_core_t*, void(*cb)(const char* id, int online, void* user), void* user);

// ---- config (复用 config_center) ----
int  cd_core_get_str(cd_core_t*, const char* key, char* out, size_t out_len);
int  cd_core_set_str(cd_core_t*, const char* key, const char* val);
// ... 数值/bool 同理

// ---- thumbnail / recent connections ----
int  cd_core_recent_list(cd_core_t*, char* json_out, size_t out_len);
int  cd_core_thumbnail_get(cd_core_t*, const char* peer_id, uint8_t* png, size_t* len);

// ---- file transfer orchestration ----
cd_token_t cd_core_file_send(cd_core_t*, const char* peer_id, const char* path);
void       cd_core_file_cancel(cd_core_t*, cd_token_t);
void       cd_core_set_file_progress_cb(cd_core_t*, void(*cb)(cd_token_t, uint64_t sent, uint64_t total, int status, void*), void*);

// ---- session lifecycle (由 Shell 调用，但 Peer 由 Session Window 持有) ----
// 返回一段"会话 bootstrap blob"，Shell 通过 IPC 传给 spawn 出来的 session
int  cd_core_session_prepare(cd_core_t*, const char* peer_id, const char* password,
                             uint8_t* bootstrap_out, size_t* len);
// Session Window 内部用 bootstrap 构造 peer
cd_peer_t* cd_peer_from_bootstrap(const uint8_t* blob, size_t len);
void       cd_peer_destroy(cd_peer_t*);

// ---- Session Window 专用：注册视频/音频/数据回调 ----
void cd_peer_set_video_cb (cd_peer_t*, void(*cb)(const cd_video_frame*, void*), void*);
void cd_peer_set_audio_cb (cd_peer_t*, void(*cb)(const cd_audio_frame*, void*), void*);
void cd_peer_send_action  (cd_peer_t*, const char* json);  // 复用 RemoteAction JSON
```

### 4.2 线程模型

- 所有回调在 core 内部 worker 线程触发；FFI 侧需用 `dart:isolate` 的 `NativeCallable` 或 `SendPort` 跨回主 isolate。
- 高频回调（视频帧）**不暴露给 Flutter**，仅 Session Window 进程注册。

### 4.3 兼容性策略

- 共享库 SONAME `libcrossdesk_core.1.so`；C ABI 一旦发布禁止 break；新增能力走新函数。
- 内部 C++ 实现可以自由演进。

---

## 5. `crossdesk_shell` (Flutter) 设计

### 5.1 工程结构

```
flutter_shell/
├── lib/
│   ├── main.dart
│   ├── ffi/                 # ffigen 生成 + 手写包装
│   │   └── core_bindings.dart
│   ├── core/                # 业务层 (PeerService, ConfigService, SessionManager...)
│   ├── state/               # riverpod providers
│   ├── ui/
│   │   ├── pages/
│   │   │   ├── home_page.dart          # 等价旧 MainWindow
│   │   │   ├── settings_page.dart      # 等价 MainSettingsWindow
│   │   │   ├── server_page.dart        # 等价 ServerWindow
│   │   │   ├── file_transfer_panel.dart
│   │   │   └── about_page.dart
│   │   ├── widgets/
│   │   │   ├── peer_card.dart          # 等价 panels/*
│   │   │   ├── recent_connections_list.dart
│   │   │   └── connection_status_dialog.dart
│   │   └── theme/
│   └── platform/
│       ├── tray.dart                    # tray_manager
│       └── session_launcher.dart        # spawn crossdesk_session
├── windows/  macos/  linux/             # Flutter Desktop runner，CMake 集成 core lib
└── pubspec.yaml
```

### 5.2 关键页面与旧实现映射

| Flutter 页面 | 替代的旧实现 |
|---|---|
| `HomePage` | [main_window.cpp](../src/gui/windows/main_window.cpp) + [local_peer_panel.cpp](../src/gui/panels/local_peer_panel.cpp) + [remote_peer_panel.cpp](../src/gui/panels/remote_peer_panel.cpp) + [recent_connections_panel.cpp](../src/gui/panels/recent_connections_panel.cpp) + [title_bar.cpp](../src/gui/toolbars/title_bar.cpp) + [status_bar.cpp](../src/gui/toolbars/status_bar.cpp) |
| `SettingsPage` | [main_settings_window.cpp](../src/gui/windows/main_settings_window.cpp) |
| `ServerPage` | [server_window.cpp](../src/gui/windows/server_window.cpp), [server_settings_window.cpp](../src/gui/windows/server_settings_window.cpp) |
| `FileTransferPanel` | [file_transfer_window.cpp](../src/gui/windows/file_transfer_window.cpp) |
| `AboutPage` / `UpdateDialog` | [about_window.cpp](../src/gui/windows/about_window.cpp), [update_notification_window.cpp](../src/gui/windows/update_notification_window.cpp) |
| `RequestPermissionDialog` (mac) | [request_permission_window.mm](../src/gui/windows/request_permission_window.mm) — Flutter 调 macOS 权限 API |

### 5.3 SessionManager 流程（关键）

```dart
class SessionManager {
  Future<Session> connect(String peerId, {String? password, SessionOptions? opts}) async {
    // 1. 让 core 准备一段 bootstrap blob（含信令握手好的 SDP/ICE 凭据等）
    final blob = core.sessionPrepare(peerId, password ?? '');

    // 2. spawn 子进程
    final ipc = await IpcServer.bindUnique();   // UDS / NamedPipe
    final proc = await Process.start(
      sessionExecutablePath,
      ['--ipc', ipc.address, '--bootstrap-fd', '3'],
      // bootstrap blob 通过环境变量/stdin/匿名管道传，避免出现在命令行
    );
    await ipc.sendBootstrap(blob);

    // 3. 监听子进程事件 (connected/disconnected/stats/file-progress)
    final session = Session._(proc, ipc, peerId);
    sessions[session.id] = session;
    return session;
  }
}
```

### 5.4 IPC 消息 schema（节选）

| 方向 | 类型 | 字段 | 说明 |
|---|---|---|---|
| shell → session | `bootstrap` | blob bytes | 启动时一次性发送 |
| shell → session | `apply_settings` | quality, fps, codec | 用户在 Flutter 改设置时透传 |
| shell → session | `request_close` | reason | 用户关掉了任务栏窗口 |
| shell → session | `send_file` | path, label | 拖拽到 Flutter 的文件转发 |
| session → shell | `state` | `connecting / connected / failed / closed`, error_msg | 让 Flutter 更新 `ConnectionStatusDialog` |
| session → shell | `stats` | rtt_ms, bitrate, fps, packet_loss | 仅当 shell 订阅时推送 |
| session → shell | `file_progress` | token, sent, total, status | 复用现有进度模型 |
| session → shell | `clipboard` | text | 远端剪贴板同步给本地 |
| session → shell | `exit` | code | 进程退出钩子 |

> 协议层用 4-byte length-prefix + UTF-8 JSON（复用 `nlohmann/json`）。**已定稿，不引入 FlatBuffers/Protobuf**。

### 5.5 托盘

**保持现状**：
- Windows：继续使用 [win_tray.cpp](../src/gui/tray/win_tray.cpp)，由 Flutter shell 进程通过 FFI（`cd_tray_*` 系列）host。
- macOS / Linux：本次不引入新托盘（与现状一致——旧版本这两个平台本来就没有托盘）。

---

## 6. `crossdesk_session` 设计

### 6.1 目标
- 一进程 = 一个远程会话窗口。
- 窗口装饰、视频画面、悬浮控制条、连接错误页全部在该进程内绘制。
- 进程退出 = 会话结束。Flutter 不能强杀（先发 `request_close` 给 graceful shutdown 机会）。

### 6.2 复用 vs 新增

直接复用（裁剪后）：
- [stream_window.cpp](../src/gui/windows/stream_window.cpp) → 主画面绘制
- [control_window.cpp](../src/gui/windows/control_window.cpp) → 悬浮控制条
- [connection_status_window.cpp](../src/gui/windows/connection_status_window.cpp) → 连接中/失败提示
- [render_callback.cpp](../src/gui/render_callback.cpp) 中视频/音频/输入相关代码段
- [device_controller/](../src/device_controller/) 整个
- 字体、图标资源 [gui/assets/](../src/gui/assets/)

废弃（搬去 Flutter）：
- `MainWindow`、`LocalPeerPanel`、`RemotePeerPanel`、`RecentConnectionsPanel`
- `MainSettingsWindow`、`ServerWindow`、`ServerSettingsWindow`、`AboutWindow`、`UpdateNotificationWindow`
- `TitleBar`、`StatusBar`、托盘相关

### 6.3 启动

```
crossdesk_session --ipc /tmp/crossdesk.sock.<rand> [--debug]
```
- `argv` 不含敏感信息（密码、token）。bootstrap blob 通过 IPC 首帧发送。
- 一旦 bootstrap 解析成功 → `cd_peer_from_bootstrap` → 注册 video/audio/data 回调 → 创建 SDL 窗口 → 主循环。

### 6.4 类拆分（替换原 `Render`）

```
SessionApp                       // main loop, SDL3 window owner
├── SessionPeer                  // 持有 cd_peer_t*
├── VideoRenderer                // NV12 -> SDL_Texture (复用现有逻辑)
├── AudioSink                    // 复用 speaker_capturer 反向通道
├── InputForwarder               // SDL3 event -> RemoteAction JSON
├── ControlBar                   // ImGui overlay, 复用 control_window.cpp
├── FileReceiver                 // 落盘 + 进度上报 shell
└── ShellChannel                 // IPC, 收 settings/file/close, 发 state/stats
```

`Render` 类不再存在；其状态机以**多个小类 + 显式所有权**取代。

### 6.5 与现有 Windows 服务集成

[service/](../src/service/) 的 `service_host` 当前是 `Render` 持有 `PipeClient`。重构后由 `SessionApp` 持有；服务的"远端 SAS / UAC 桌面切换"功能不动。

---

## 7. 构建系统

### 7.1 现状
[xmake.lua](../xmake.lua) 单 target `crossdesk`。

### 7.2 改造

```lua
-- 共享库
target("crossdesk_core")
    set_kind("shared")
    add_files("src/common/**", "src/config_center/**", "src/screen_capturer/**",
              "src/device_controller/**", "src/thumbnail/**", "src/version_checker/**",
              "src/path_manager/**", "src/speaker_capturer/**", "src/log/**",
              "src/core_api/**")           -- 新增 C ABI 封装层
    add_deps("minirtc")
    if is_plat("windows") then add_files("src/service/**") end

-- 会话窗口可执行
target("crossdesk_session")
    set_kind("binary")
    add_files("src/session/**",            -- 新目录: SessionApp/VideoRenderer/...
              "src/gui/windows/stream_window.cpp",
              "src/gui/windows/control_window.cpp",
              "src/gui/windows/connection_status_window.cpp",
              "src/gui/windows/file_transfer_window.cpp")
    add_deps("crossdesk_core")
    add_packages("sdl3", "imgui", "libyuv")

-- Flutter shell 由 flutter build 驱动；shell 的原生 runner
-- 通过 CMake (Flutter Desktop 模板) link libcrossdesk_core
target("crossdesk_shell")
    set_kind("phony")
    on_build(function()
        os.exec("flutter build windows --release")
    end)
```

### 7.3 打包

- Windows：Inno Setup，把 `crossdesk_shell.exe`（Flutter 产物）、`crossdesk_session.exe`、`crossdesk_service.exe`、`crossdesk_core.dll` 放到同目录。
- macOS：`crossdesk.app/Contents/MacOS/` 主进程为 Flutter shell，`Resources/` 下放 `crossdesk_session`，签名/公证统一。
- Linux：AppImage 或 deb；同目录摆放。

`SessionLauncher` 通过相对可执行路径定位 `crossdesk_session`，避免硬编码。

---

## 8. 数据流详解

### 8.1 视频帧（关键性能路径）

```
remote camera
   │ (网络)
   ▼
minirtc decoder ─► NV12 frame ─► cd_peer video_cb (Session 进程)
                                     │
                                     ▼
                         VideoRenderer::Upload  (主线程, SDL_UpdateNVTexture)
                                     │
                                     ▼
                          SDL_RenderTexture (60Hz vsync)
```
**不经过 Flutter，不经过 IPC，不引入跨进程拷贝。**

### 8.2 输入

```
SDL3 KeyDown/MouseMove (Session 进程)
   │ InputForwarder::Translate
   ▼
RemoteAction JSON
   │ cd_peer_send_action
   ▼
minirtc data channel ─► 对端 device_controller 注入
```

### 8.3 设置变更

```
用户在 Flutter 改了"分辨率/帧率"
   │ riverpod onChange
   ▼
core.setStr("video.fps", "60")     (FFI, 本地配置持久化)
   │
   ▼
SessionManager.broadcast(applySettings(fps:60))
   │ IPC
   ▼
SessionApp::OnApplySettings → 调 cd_peer API 调整码率
```

### 8.4 文件传输

- 发起在 Flutter（拖拽 / 选择文件）→ `core.fileSend(peerId, path)` → core 内部直接走 minirtc DataChannel；同时 SessionApp 注册 `file_progress` 回调上报 shell。
- 接收落盘在 Session Window 进程；进度通过 IPC 同步给 Flutter 显示。
- 这样设计避免大文件 IO 与 UI 线程争抢。

---

## 9. 迁移计划（建议 6 个迭代）

| 迭代 | 内容 | 验收 |
|---|---|---|
| M1 | 抽取 `libcrossdesk_core`：把 `config_center`、`device_presence`、`thumbnail`、`version_checker`、`path_manager` 包成 C ABI；老 `Render` 改为通过 core API 访问这些模块 | 老 ImGui 版本编译运行无差异 |
| M2 | 抽取 `crossdesk_session` 二进制：先让它能被命令行直接拉起，复用现有 `Render` 中 stream/control 部分；信令仍由老主进程承担，session 通过 IPC 拿到 peer bootstrap | 老主进程能"外置"一个会话窗口（feature flag 切换） |
| M3 | Flutter 工程脚手架 + FFI bindings + `HomePage`/`SettingsPage` 静态 UI；read-only 显示 core 数据 | UI 走查通过 |
| M4 | Flutter `SessionManager` 完成 spawn + IPC 全链路；可端到端发起一路远程连接 | dogfood 一周 |
| M5 | 文件传输、剪贴板、托盘、升级提示、macOS 权限申请、Windows 服务对接 | 功能对齐老版本 |
| M6 | 旧 ImGui 主界面代码删除，CI 切换默认产物为 Flutter 版本；老路径仅保留在 `legacy/` tag | 发布 RC |

每个迭代都保证 main 分支可发布、老 UI 与新 UI 并存通过编译开关切换。

---

## 10. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| Flutter Linux 在某些发行版渲染慢 / 字体糊 | 主界面体验差 | 提供 `--use-impeller` / `--no-impeller` 开关；CI 跑 Ubuntu 22.04 截图回归 |
| Session 子进程崩溃但未通知 Shell | UI 显示"连接中"卡住 | Shell 端 `Process.exitCode` 监听 + IPC 心跳 (3s 间隔) |
| 多 Session 同时打开导致句柄/端口耗尽 | 用户连不上第 N 路 | IPC 用 UDS/NamedPipe (无端口)；core 内 PeerConnection 上限可配 |
| IPC bootstrap blob 泄漏密码 | 安全 | blob 用一次性 nonce 加密；只在父子进程间通过 stdin / anonymous pipe 传，不落盘、不进 argv |
| FFI 回调跨线程触发 Dart 异常 | 崩溃 | 所有 callback 用 `NativeCallable.listener`，UI 操作回到 root isolate |
| macOS 沙箱/公证下子进程无法独立签名 | 上架被拒 | `crossdesk_session` 作为 helper tool 放在 `Contents/Helpers/`，共享 entitlement |
| 老用户配置文件迁移 | 升级体验差 | core 在 `cd_core_create` 时调 `config_center` 现有逻辑读旧 INI，无需用户介入 |

---

## 11. 未决问题（需与你确认）

1. **Flutter 状态管理**：默认 `riverpod`，是否接受？
2. **IPC 序列化**：起步 JSON 是否 OK？还是直接上 FlatBuffers/Protobuf？
3. **Session 进程粒度**：一连接一进程（本文档默认） vs 一全局 session-host 进程托管所有会话——前者隔离性好、崩溃影响小；后者资源占用低。倾向前者。
4. **托盘**：统一 `tray_manager` vs Windows 保留原生 `win_tray.cpp`？
5. **Windows 服务对接**：`service_host` 跟 Session 进程一对一，还是仍由 Shell 进程持有？倾向 Session 持有，因为它才是真正发 SAS / 监控 interactive desktop 的角色。
6. **Web Client 与新协议**：本次重构不动 Web 侧；如果将来想让 Web 也复用同样的"控制面 / 数据面分离"思路，建议预留 IPC schema 的 WebSocket 版本。

---

## 12. 附录：关键文件清单（待裁剪/删除）

迁移完成后将从 src 移除：

- 全部 [gui/windows/main_window.cpp](../src/gui/windows/main_window.cpp), [main_settings_window.cpp](../src/gui/windows/main_settings_window.cpp), [server_window.cpp](../src/gui/windows/server_window.cpp), [server_settings_window.cpp](../src/gui/windows/server_settings_window.cpp), [about_window.cpp](../src/gui/windows/about_window.cpp), [update_notification_window.cpp](../src/gui/windows/update_notification_window.cpp)
- 全部 [gui/panels/](../src/gui/panels/), [gui/toolbars/](../src/gui/toolbars/)
- [gui/render.cpp](../src/gui/render.cpp) 主界面相关函数（保留 stream/control/connection-status/file-transfer 相关）
- ImGui 主面板使用的本地化字段（`localization_data.h` 中和主界面相关的条目）

新增：

- `src/core_api/`  —— C ABI 封装
- `src/session/`   —— `crossdesk_session` 入口与类
- `src/ipc/`       —— shell ↔ session 的 schema 和帧编解
- `flutter_shell/` —— Flutter 工程

---

*文档版本：v1.1 (2026-05-20) — 已确认，进入实施*

---

## 13. v1.1 决策记录（覆盖第 11 节）

| 条目 | 决策 | 说明 |
|---|---|---|
| Flutter 状态管理 | **riverpod** | 编译期类型安全、社区主流 |
| IPC 序列化 | **JSON**（4-byte length-prefix + UTF-8 JSON，复用 `nlohmann/json`） | 调试方便、零新依赖；将来个别高频消息再单独换 |
| Session 进程粒度 | **一连接一进程** | 隔离性优先 |
| 托盘 | **保留现状**：Windows 复用 [win_tray.cpp](../src/gui/tray/win_tray.cpp)（FFI 调用），macOS / Linux 不引入新托盘 | 不增加跨平台维护负担 |
| Windows 服务对接 | **Session 进程持有 `service_host`** | 因为 SAS / interactive desktop 监控本来就服务于活跃会话 |
| Web Client | **本次不动** | 不预留 WebSocket 版 IPC |

---

## 14. Flutter UI 内容清单（v1.1 新增）

### 14.1 主页 `HomePage`（等价旧 `MainWindow`）

包含：自绘标题栏、本机面板（设备 ID/密码/状态）、连接面板（远端 ID 输入 + 密码 + 连接按钮）、最近连接卡片网格（缩略图 + ID + 时间）、底部状态栏（版本/信令状态/带宽/CPU/快捷入口）。

替代旧：[main_window.cpp](../src/gui/windows/main_window.cpp) + [local_peer_panel.cpp](../src/gui/panels/local_peer_panel.cpp) + [remote_peer_panel.cpp](../src/gui/panels/remote_peer_panel.cpp) + [recent_connections_panel.cpp](../src/gui/panels/recent_connections_panel.cpp) + [title_bar.cpp](../src/gui/toolbars/title_bar.cpp) + [status_bar.cpp](../src/gui/toolbars/status_bar.cpp)。

### 14.2 设置页 `SettingsPage`（等价旧 `MainSettingsWindow`）

侧栏分组，**保留旧版所有配置项**：

| 分组 | 项 |
|---|---|
| 通用 | 语言、主题、开机自启、最小化到托盘、关闭按钮行为 |
| 视频 | 编码（H264/AV1）、硬件编解码、画质档位、帧率（15/30/60）、自适应码率上下限 |
| 音频 | 启用音频转发、麦克风、扬声器采集、降噪 |
| 网络 | 信令服务器地址、STUN/TURN 列表、自定义端口范围 |
| 安全 | 本机密码、白名单、连接需手动确认 |
| 设备 | 鼠标加速、键盘布局映射、剪贴板同步、文件传输保存目录 |
| 服务端 | Windows 服务安装/启动/停止、SAS 测试 |
| 高级 | 日志等级、日志目录、上报诊断、重置全部设置 |
| 关于 | 版本、检查更新、开源许可 |

替代旧：[main_settings_window.cpp](../src/gui/windows/main_settings_window.cpp), [server_window.cpp](../src/gui/windows/server_window.cpp), [server_settings_window.cpp](../src/gui/windows/server_settings_window.cpp), [about_window.cpp](../src/gui/windows/about_window.cpp), [update_notification_window.cpp](../src/gui/windows/update_notification_window.cpp)。

### 14.3 弹窗（仍在 Flutter 内）

- `ConnectionStatusDialog`（连接中 / 失败 / 等待对端确认 / 输入密码）—— 替代 [connection_status_window.cpp](../src/gui/windows/connection_status_window.cpp)
- `PasswordPrompt`
- `IncomingConnectionDialog`（被控端：是否同意连入）
- `FileTransferPanel`（右下抽屉，已传/进行中列表）—— 替代 [file_transfer_window.cpp](../src/gui/windows/file_transfer_window.cpp)
- `UpdateDialog` —— 替代 [update_notification_window.cpp](../src/gui/windows/update_notification_window.cpp)
- `MacPermissionDialog` —— 替代 [request_permission_window.mm](../src/gui/windows/request_permission_window.mm)

### 14.4 不在 Flutter 内（Session 进程负责）

远程画面、悬浮控制条（旋转/全屏/比例/截图/重启对端/键鼠模式切换等）、连接中转圈、断线提示——全部归 `crossdesk_session`，沿用现有 SDL3 + ImGui。

---

## 15. 配置与数据层（v1.1 新增）

**结论：本次重构不引入 SQLite，沿用 INI + JSON + PNG 文件。**

### 15.1 数据归属（**唯一所有者：`libcrossdesk_core`**）

| 数据 | 存储 | 读 | 写 |
|---|---|---|---|
| 全局设置（语言、视频、网络、安全...） | `config.ini`（现状） | Flutter (`cd_core_get_*`)、Session 启动时拿快照 | core 唯一写入者 |
| 本机 ID / 密码 | `config.ini` | Flutter、core 内部 | core |
| 最近连接列表 | `recent.json`（新增，结构化） | Flutter | core（每次会话结束写） |
| 缩略图 | `thumbnails/<peer_id>.png` | Flutter | core ([thumbnail/](../src/thumbnail/) 模块) |
| 白名单 / 已信任对端 | `trust.json`（新增） | Flutter 编辑、core 鉴权时读 | core |
| 日志 | 现有 `rd_log` 目录 | 用户/Flutter "打开日志目录" | core / Session |

### 15.2 并发与一致性

- **任何配置文件只有 core 写**——Flutter 和 Session 都通过 core 的 C ABI 间接操作，避免多进程同写。
- core 内部对配置加 `std::shared_mutex`：读多写少。
- Flutter 改设置：`cd_core_set_str` → core 落盘 → core 通过已建立的 IPC 主动把变更字段推给所有活跃 Session（`apply_settings` 消息）。
- Session 不持久化任何配置；启动时从 bootstrap blob 拿初始快照，运行时听 Flutter。

### 15.3 为什么暂不引入 SQLite

- 当前 K-V 设置 INI 已经胜任。
- 列表型数据（最近连接、白名单）规模 < 1000 条，JSON 足够。
- 引入 SQLite 会带来：依赖增加（`sqflite_common_ffi` 或 `drift`）、schema 迁移负担、Flutter / C++ 两端需要保持 schema 一致。
- 升级路径已留好：未来如需"按对端独立的设置 / 设备组 / 详细连接历史"，在 core 内加 `cd_core_db_*` 接口即可，外部 API 不变。

---

## 16. 实施 Kickoff 检查清单（M1）

进入 M1 时立刻动手：

1. 新建 [src/core_api/](../src/core_api/) 目录，添加 `crossdesk_core.h`（C ABI）和 `crossdesk_core.cpp`（实现包装现有模块）。
2. 修改 [xmake.lua](../xmake.lua)：新增 `crossdesk_core` shared target；老 `crossdesk` target 改为依赖它（先不删旧渲染逻辑）。
3. 旧 [render.cpp](../src/gui/render.cpp) 内访问 `config_center` / `device_presence` / `thumbnail` 的地方，全部改走 core API（保证老 UI 仍可工作，验证 API 充分性）。
4. 单元测试：core 启动/销毁、配置读写、presence 回调。
5. CI 加 `xmake b crossdesk_core` 一步。

M1 完成的标志：**老 ImGui 版本编译运行无功能差异**，但底层已经走 core 共享库。这是后续所有迭代的基石。

