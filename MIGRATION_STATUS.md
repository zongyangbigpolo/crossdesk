# CrossDesk Flutter 迁移进度

> 把 CrossDesk 从 ImGui 单体迁移到 **Flutter 主界面 + 每会话独立原生 session 进程** 的架构。
> 本文档记录截至本轮（M2 part 3）已完成、可验证、未完成的工作。

---

## 1. 目标架构

```
┌──────────────────────────┐
│   crossdesk_shell        │   Flutter Desktop (Dart + dart:ffi)
│   - 主界面 / 设置 / 远程页 │
│   - SessionManager × N    │
└───────────┬──────────────┘
            │ IPC (length-prefixed JSON)
            │   POSIX → Unix Domain Socket
            │   Windows → TCP loopback
            ▼
┌──────────────────────────┐
│   crossdesk_session      │   原生 C++ 进程（每个连接一个）
│   - SDL3 窗口 + NV12 渲染 │
│   - 鼠标/键盘 → RemoteAction
└───────────┬──────────────┘
            │ cd_peer_* C ABI
            ▼
┌──────────────────────────┐
│   libcrossdesk_core      │   共享 C ABI 动态库
│   - cd_core / cd_cfg_*    │
│   - cd_identity_*         │
│   - cd_peer_* (minirtc)   │
│   - cd_presence_*         │
└──────────────────────────┘
```

设计约束：
- **C ABI 永不破坏**：只追加签名，所有字符串 UTF-8，int 返回 0 成功 / 负数错误码。
- **绝不通过 argv 传敏感数据**（密码、bootstrap），全部走 IPC。
- **窗口/连接 1:1**：一个连接 = 一个 `crossdesk_session` 进程，崩了不影响主壳子。

---

## 2. 已完成里程碑

### M1 — Flutter 壳子 + Core C ABI ✅

| 模块 | 内容 |
|---|---|
| `libcrossdesk_core` | C ABI：`cd_core_create/destroy/version`、`cd_cfg_get/set_*`（语言、视频质量/帧率/编解码、TURN/SRTP、信令端口/主机、文件路径…）、`cd_presence_*` |
| Flutter shell | Riverpod 状态管理；主导航；设置页双向绑定 + 写回持久化（INI） |
| FFI 桥 | [lib/ffi/core_bindings.dart](flutter_shell/lib/ffi/core_bindings.dart) 手写 dart:ffi 绑定 |
| 打包 | macOS：Xcode Run Script [tool/bundle_macos_dylib.sh](flutter_shell/tool/bundle_macos_dylib.sh) 自动复制 dylib 进 .app，`@rpath` + `@executable_path/../Frameworks` |
| 验证 | [tool/ffi_smoke.dart](flutter_shell/tool/ffi_smoke.dart) |

### M2 part 1 — IPC + 子进程框架 ✅

| 模块 | 内容 |
|---|---|
| 帧协议 | 4 字节大端长度前缀 + UTF-8 JSON；上限 16 MiB；双端共用 |
| C++ 端 | [src/ipc/ipc_frame.cpp](src/ipc/ipc_frame.cpp)、[src/ipc/ipc_client.cpp](src/ipc/ipc_client.cpp)（阻塞 socket + 单 reader 线程 + 写锁） |
| Dart 端 | [lib/ipc/ipc_frame.dart](flutter_shell/lib/ipc/ipc_frame.dart)、[lib/ipc/ipc_server.dart](flutter_shell/lib/ipc/ipc_server.dart)（单 peer 强制，第二个连进来直接 destroy） |
| 消息类型 | [src/ipc/ipc_messages.h](src/ipc/ipc_messages.h) ↔ [lib/ipc/ipc_messages.dart](flutter_shell/lib/ipc/ipc_messages.dart)：`hello / state / stats / file_progress / clipboard / exit / bootstrap / apply_settings / request_close / send_file` |
| SessionManager | [lib/session/session_manager.dart](flutter_shell/lib/session/session_manager.dart)：bind → spawn → 等真实 `hello` 消息（不靠 TCP accept）→ 双向消息 → `request_close` → 优雅退出（超时 SIGTERM/SIGKILL） |
| crossdesk_session | [src/session/session_app.cpp](src/session/session_app.cpp)：SDL3 窗口 + IPC 客户端骨架 |
| 验证 | [tool/session_smoke.dart](flutter_shell/tool/session_smoke.dart) |

### M2 part 2 — Peer ABI + 端到端骨架 ✅

| 模块 | 内容 |
|---|---|
| `cd_peer_*` C ABI | [src/core_api/crossdesk_core.h](src/core_api/crossdesk_core.h)：`open / set_video_cb / set_conn_cb / set_signal_cb / set_net_stats_cb / connect / send_action / close / destroy`；`cd_peer_opts_t`（local_id, remote_id, signal_host/port, coturn_port, turn/srtp, video quality/codec, log_dir）；NV12 帧结构 |
| 实现 | [src/core_api/cd_peer.cpp](src/core_api/cd_peer.cpp)：minirtc PeerPtr 包装，回调桥接（加锁），流标签 primary_display/audio/data/mouse/keyboard |
| SessionApp Bootstrap | 收 bootstrap JSON → `cd_core_create` → `cd_peer_open` → 注册回调 → `cd_peer_connect` |
| NV12 渲染 | `SDL_PIXELFORMAT_NV12` 流纹理，`SDL_UpdateNVTexture(tex, NULL, Y, w, UV, w)`，分辨率变化自动重建 |
| 输入转发 | SDL3 mouse/key 事件 → `RemoteAction` JSON（type=0 mouse / type=1 keyboard）→ `cd_peer_send_action` |
| Stats 上报 | `cd_peer_set_net_stats_cb` → 把 minirtc `XNetTrafficStats` 转 kbps → 发 `kStats` IPC → shell 卡片显示 bitrate/loss/mode |
| Shell UI | [lib/ui/pages/sessions_page.dart](flutter_shell/lib/ui/pages/sessions_page.dart)：peer id + 6 位密码输入、phase chip（颜色编码）、状态卡片、stats 行 |

### M2 part 3 — 高优先级补齐 ✅（本轮）

#### 真实设备身份
- **C ABI**：`cd_identity_ensure / get_client_id / get_password / set_client_id / set_password`
- **存储**：`<config_dir>/identity.json`，首次自动生成 10 位 client_id + 6 位 password
- **Dart 暴露**：`core.ensureIdentity / clientId / password / setClientId / setPassword`
- **Bootstrap 修正**：
  - `local_id = "<my_client_id>@<my_password>"`（本机身份，他人连接我时使用）
  - `remote_id = "<peerId>@<userPw>"`（传给 minirtc `JoinConnection`）

#### Windows IPC（TCP loopback）
- Endpoint 解析统一支持：`tcp://host:port` 或 UDS 路径（可选 `unix:` 前缀）
- C++ `ipc::Client`：Winsock 跨平台，POSIX 走 UDS
- Dart `IpcServer.bind`：TCP 时 OS 自动分配端口，解析后的 endpoint 通过 `server.endpoint` 暴露
- `SessionManager`：Windows 自动选 `tcp://127.0.0.1:0`，POSIX 保 UDS

#### End-to-end bootstrap smoke
- [tool/bootstrap_smoke.dart](flutter_shell/tool/bootstrap_smoke.dart)：spawn → 发真 bootstrap（带 identity + 不可达信令地址）→ 验证收到 minirtc 产生的 `signal_connecting`
- 期间修复：`log_dir` 为空让 minirtc 崩溃（[src/session/session_app.cpp](src/session/session_app.cpp) 默认到 `$TMPDIR`）

#### 同期清理
- [tool/ffi_smoke.dart](flutter_shell/tool/ffi_smoke.dart) 12 条 `avoid_print` 全部消除

---

## 3. 如何验证

### 准备
```bash
cd /Users/pli/srv_code/myrepo/ShowUI-Aloha-POC/crossdesk
xmake build crossdesk_core
xmake build crossdesk_session
```

### A) Flutter UI
```bash
cd flutter_shell
flutter run -d macos
```
能看到主窗口、设置页持久化、"远程"页输入框。

### B) FFI 持久化
```bash
CROSSDESK_CORE_LIB=$(pwd)/../build/macosx/arm64/release/libcrossdesk_core.dylib \
CROSSDESK_CONFIG_DIR=/tmp/cd_smoke \
dart run tool/ffi_smoke.dart
# 期望: OK: FFI roundtrip + persistence verified.
```

### C) Session 进程生命周期
```bash
CROSSDESK_CORE_LIB=$(pwd)/../build/macosx/arm64/release/libcrossdesk_core.dylib \
CROSSDESK_SESSION_BIN=$(pwd)/../build/macosx/arm64/release/crossdesk_session \
dart run tool/session_smoke.dart
# 期望: OK: spawn → hello → request_close → exit verified.
```

### D) Bootstrap → minirtc 管线
```bash
rm -rf /tmp/cd_bootstrap_smoke
CROSSDESK_CORE_LIB=$(pwd)/../build/macosx/arm64/release/libcrossdesk_core.dylib \
CROSSDESK_SESSION_BIN=$(pwd)/../build/macosx/arm64/release/crossdesk_session \
CROSSDESK_CONFIG_DIR=/tmp/cd_bootstrap_smoke \
dart run tool/bootstrap_smoke.dart
# 期望: observed remote state: signal_connecting
#       OK: bootstrap reached cd_peer / minirtc and produced state.
```

### E) UI 触发 session 子进程
Flutter 主窗口"远程"页 → 输入任意 peer ID + 密码 → 连接 →
- 弹出 SDL3 黑屏窗口（session 进程）
- 卡片显示 phase=helloed/running，stats 行出现
- **不会有真视频**（见下）

---

## 4. 当前状态总览

| 项 | 状态 |
|---|---|
| `xmake build` 全工程 | ✅ build ok ~20s |
| `flutter analyze` | ✅ 0 issues |
| `tool/ffi_smoke.dart` | ✅ |
| `tool/session_smoke.dart` | ✅ |
| `tool/bootstrap_smoke.dart` | ✅（到 `signal_connecting`） |
| macOS 打包 | ✅ |
| Windows 打包 | ⚠️ 未测，但 IPC 路径已就绪 |
| Linux 打包 | ⚠️ 未测 |
| 真实远程连接 | ❌（见 §5 阻塞项） |

---

## 5. 已知阻塞 / 未完成

### 真端到端连接的两个阻塞
1. **`cd_peer_connect` 同步调 `JoinConnection`**：minirtc 信令此时还在握手，返回 `-100`。
   ```
   [minirtc] Signal service not ready for join yet, status = [connecting]
   [session] cd_peer_connect failed: -100
   ```
   **修法**：改成异步——`cd_peer_connect` 只调 Init，等 `signal_connected` 回调里再 `JoinConnection`。
2. **需要真信令服务器 + 另一台运行 legacy `crossdesk` 的机器**才能看到画面。

### 中优先级 TODO
- 异步 connect（上一条）+ 失败重连策略
- 音频 sink（SessionApp 接 minirtc 音频回调 → SDL3 audio device）
- SDL3 scancode → Windows VK 翻译表
- minirtc RTT 暴露（目前 stats 里 `rtt_ms` 恒为 0；fps 也未计算）
- session 端 stats 中 fps 从渲染帧间隔估算

### M5（功能补齐）
- 文件传输 IPC（`send_file` + `file_progress`）
- 剪贴板同步
- 系统托盘（FFI）
- macOS 屏幕录制 / 辅助功能权限弹窗

### M6（清理）
- 删除 legacy `src/gui/`（ImGui 主界面）
- CI 默认切到 Flutter 构建
- Windows 一键安装包

---

## 6. 关键文件索引

### C++ 新增 / 改动
- [src/core_api/crossdesk_core.h](src/core_api/crossdesk_core.h) — C ABI 总入口
- [src/core_api/crossdesk_core.cpp](src/core_api/crossdesk_core.cpp) — Core + Identity 实现
- [src/core_api/cd_peer.cpp](src/core_api/cd_peer.cpp) — Peer ABI（minirtc 包装）
- [src/ipc/ipc_frame.h](src/ipc/ipc_frame.h) / [.cpp](src/ipc/ipc_frame.cpp) — 帧协议
- [src/ipc/ipc_client.h](src/ipc/ipc_client.h) / [.cpp](src/ipc/ipc_client.cpp) — IPC 客户端（POSIX + Windows）
- [src/ipc/ipc_messages.h](src/ipc/ipc_messages.h) — 消息类型常量
- [src/session/session_app.h](src/session/session_app.h) / [.cpp](src/session/session_app.cpp) — session 进程主体
- [src/session/session_main.cpp](src/session/session_main.cpp) — 入口
- [xmake/targets.lua](xmake/targets.lua) — 构建定义

### Dart 新增
- [flutter_shell/lib/ffi/core_bindings.dart](flutter_shell/lib/ffi/core_bindings.dart)
- [flutter_shell/lib/core/core.dart](flutter_shell/lib/core/core.dart)
- [flutter_shell/lib/ipc/ipc_frame.dart](flutter_shell/lib/ipc/ipc_frame.dart)
- [flutter_shell/lib/ipc/ipc_server.dart](flutter_shell/lib/ipc/ipc_server.dart)
- [flutter_shell/lib/ipc/ipc_messages.dart](flutter_shell/lib/ipc/ipc_messages.dart)
- [flutter_shell/lib/session/session_manager.dart](flutter_shell/lib/session/session_manager.dart)
- [flutter_shell/lib/state/providers.dart](flutter_shell/lib/state/providers.dart)
- [flutter_shell/lib/ui/pages/sessions_page.dart](flutter_shell/lib/ui/pages/sessions_page.dart)
- [flutter_shell/lib/main.dart](flutter_shell/lib/main.dart)
- [flutter_shell/tool/ffi_smoke.dart](flutter_shell/tool/ffi_smoke.dart)
- [flutter_shell/tool/session_smoke.dart](flutter_shell/tool/session_smoke.dart)
- [flutter_shell/tool/bootstrap_smoke.dart](flutter_shell/tool/bootstrap_smoke.dart)
- [flutter_shell/tool/bundle_macos_dylib.sh](flutter_shell/tool/bundle_macos_dylib.sh)
