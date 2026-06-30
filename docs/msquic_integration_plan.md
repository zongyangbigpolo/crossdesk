# MsQuic 集成方案 (CrossDesk × microsoft/msquic)

> 目标：将 [microsoft/msquic](https://github.com/microsoft/msquic) 作为新的网络传输层集成进 CrossDesk，
> 与现有 `minirtc` 子模块的 ICE/SRTP/RTP 传输共存，并在合适场景（信令、控制通道、Relay 数据回退等）逐步替换。
>
> 状态：草案 v0.1（待评审）

---

## 0. 背景与现状

### 0.1 CrossDesk 现有网络栈

| 层 | 实现 | 位置 |
|---|---|---|
| 信令 (Signaling) | WebSocket (`ws_client`) + 自托管服务器 | `submodules/minirtc/src/ws/` |
| NAT 穿透 | STUN/TURN + Trickle ICE | `submodules/minirtc/src/ice/ice_agent.{h,cpp}` |
| 数据传输 | RTP / RTCP over UDP，可选 SRTP 加密 | `submodules/minirtc/src/{rtp,rtcp,srtp,transport}/` |
| 数据通道 | 自研 DataChannel (paced sender + FEC + ringbuffer) | `submodules/minirtc/src/transport/{datachannel_transport,channel}/` |
| 拥塞控制 / QoS | 自研 | `submodules/minirtc/src/qos/` |
| TLS 库 | OpenSSL 3.3.2 (xmake 拉取) | `xmake/options.lua` |
| HTTP 客户端 | cpp-httplib v0.26.0 | `thirdparty/cpp-httplib/` |

构建：xmake；C++17；目标平台 Windows 10+ / macOS 14+ (arm64) / Ubuntu 22.04+。

### 0.2 为什么引入 MsQuic

1. **统一 TLS+多路复用传输**：QUIC 内置 TLS 1.3，单条 UDP 连接多 stream，原生支持 0-RTT，可替代「TURN/Relay + 自研可靠层 + SRTP」组合，降低 Relay 模式下的实现复杂度。
2. **更稳的对抗弱网能力**：QUIC 的连接迁移（Connection Migration）、PTO/Loss recovery、BBR/CUBIC CC 比当前自研路径更成熟。
3. **跨平台一致性**：MsQuic 同时支持 Windows (Schannel)、Linux (OpenSSL/QUICTLS)、macOS；与现有 OpenSSL 依赖路线一致。
4. **未来 HTTP/3 演进**：自托管服务器（cpp-httplib → HTTP/1.1）后续可平滑迁移到 HTTP/3。
5. **保留 P2P 优势**：P2P 场景仍走 ICE/SRTP；MsQuic 主攻 **Relay 模式 / 信令 / 控制通道**，避免一刀切。

### 0.3 非目标 (Non-Goals)

- **不**在第一阶段移除 minirtc 或 ICE/SRTP。
- **不**重写视频/音频编解码、采集、渲染管线。
- **不**强制改造 GUI 层接口。
- **不**追求一次性替换全部 RTP/RTCP；保留二者并行能力。

---

## 1. 集成范围与分阶段目标

| 阶段 | 范围 | 风险 | 验收标准 |
|---|---|---|---|
| **P0 调研与构建** | 把 MsQuic 编进 crossdesk 构建系统；产出 hello-world 收发样例 | 低 | xmake b 在三平台均可链接 msquic；单元测试 echo 通过 |
| **P1 控制通道试点** | 用 QUIC stream 实现一个非关键控制信道（如剪贴板同步 / 文件传输预留通道），与现有 datachannel 二选一 | 中 | 在 release 模式下可稳定收发 ≥ 1 小时；丢包 5% 下成功率 ≥ 99% |
| **P2 信令替换** | 用 HTTP/3 (msquic + 自研 minimal h3) 或 QUIC stream 替换 WsClient；server 端同步改造 | 中高 | 与现有 WebSocket 信令行为一致；可灰度切换 |
| **P3 Relay 数据面** | TURN/Relay 场景下，将 SRTP-over-UDP 换为 QUIC datagram + 可靠 stream 混合 | 高 | Relay 端到端延迟 ≤ 现状 +10%；丢包/抖动表现优于现状 |
| **P4 自托管 server** | server 端引入 MsQuic listener，对外暴露 QUIC/HTTP3 端口 | 高 | Docker 镜像中包含 msquic；现网部署可灰度 |

> 推荐先完整交付 **P0 + P1**，再评审是否启动 P2/P3/P4。

---

## 2. 构建系统集成 (P0)

### 2.1 引入方式选型

| 方案 | 优点 | 缺点 | 结论 |
|---|---|---|---|
| A. xmake `add_requires("msquic ...")` | 与现有依赖风格一致 | xmake-repo 暂无官方 msquic 包，需自行写 package 脚本 | **首选**，写一个本地 package |
| B. git submodule + CMake `ExternalProject` | 源码可控、易调试 | 与 xmake 集成复杂 | 备选 |
| C. 预编译二进制 (官方 release) | 最快 | macOS arm64 / 旧 Ubuntu 兼容性差；版本锁定难 | 否 |

**决定**：在 `thirdparty/msquic/` 下放一个 xmake package 描述文件，源码以 git submodule 拉取到 `submodules/msquic/`（与 minirtc 一致）。MsQuic 内部使用 CMake，xmake package 通过 `package:add("deps", "cmake")` + `on_install` 调 CMake 构建。

### 2.2 目录与文件变更

```
crossdesk/
├── submodules/
│   ├── minirtc/                 # 现有
│   └── msquic/                  # 新增 git submodule -> microsoft/msquic, pin 至 release/2.4
├── thirdparty/
│   └── msquic/
│       └── xmake.lua            # 新增：local xmake package 描述
├── xmake/
│   ├── options.lua              # 新增 option("USE_MSQUIC")
│   └── targets.lua              # 新增 target("quic_transport")
└── src/
    └── quic_transport/          # 新增模块（见 §3）
        ├── quic_client.{h,cpp}
        ├── quic_server.{h,cpp}
        ├── quic_stream.{h,cpp}
        ├── quic_config.{h,cpp}
        └── platform/            # 平台相关 cert/keychain 工具
```

### 2.3 关键 xmake 改动

`xmake/options.lua`：
```lua
option("USE_MSQUIC")
    set_default(false)
    set_showmenu(true)
    set_description("Enable MsQuic-based transport (QUIC/HTTP3)")
option_end()
add_defines("USE_MSQUIC=" .. (is_config("USE_MSQUIC", true) and "1" or "0"))
```

`thirdparty/msquic/xmake.lua`（草案）：
```lua
package("msquic")
    set_homepage("https://github.com/microsoft/msquic")
    set_description("Microsoft's cross-platform QUIC implementation")
    set_license("MIT")
    add_deps("cmake")
    add_deps("openssl3 3.3.2")
    on_install(function (package)
        local configs = {
            "-DQUIC_TLS=openssl",
            "-DQUIC_BUILD_SHARED=OFF",
            "-DQUIC_BUILD_TOOLS=OFF",
            "-DQUIC_BUILD_TEST=OFF",
            "-DQUIC_BUILD_PERF=OFF",
        }
        import("package.tools.cmake").install(package, configs)
    end)
package_end()
```

`xmake/targets.lua` 增加：
```lua
if is_config("USE_MSQUIC", true) then
    target("quic_transport")
        set_kind("object")
        add_packages("msquic", "openssl3")
        add_deps("rd_log", "common")
        add_files("src/quic_transport/*.cpp")
        if is_os("macosx") then
            add_files("src/quic_transport/platform/*.mm")
        end
        add_includedirs("src/quic_transport", {public = true})
    -- gui target 增加 add_deps("quic_transport")
end
```

### 2.4 各平台注意

- **macOS**：MsQuic 在 macOS 上需 OpenSSL/QUICTLS 提供 QUIC API 扩展。CrossDesk 已经依赖 openssl3 3.3.2 —— 但 **stock OpenSSL 3.x 默认不带 QUIC server-side API**，需要确认是否需要切到 [quictls/openssl](https://github.com/quictls/openssl) fork 或 OpenSSL 3.5+。**这是 P0 必须先验证的关键风险点**。
- **Windows**：可选 Schannel 后端，绕开 OpenSSL；但要保持与 Linux/mac 一致以减少差异，仍走 OpenSSL/QUICTLS。
- **Linux**：Ubuntu 22.04 自带 OpenSSL 3.0.2，没有 QUIC API；必须用 xmake 拉取的私有 openssl3 3.3.2 / quictls。
- **CUDA / Wayland 互不冲突**：MsQuic 与现有 USE_CUDA / USE_WAYLAND 配置正交。

---

## 3. 抽象层设计 (P1 核心)

为了避免上层（`gui`、`service`、`device_controller`）直接耦合 MsQuic 或 minirtc，新增一个 **`ITransport` 抽象**。

### 3.1 接口（草案）

`src/quic_transport/transport_interface.h`：
```cpp
namespace crossdesk::net {

enum class Reliability { Reliable, Unreliable, PartialReliable };

struct StreamMessage {
    const uint8_t* data;
    size_t         size;
    uint64_t       timestamp_us;
};

class ITransportStream {
public:
    virtual ~ITransportStream() = default;
    virtual int  Send(const uint8_t* data, size_t size) = 0;
    virtual void Close() = 0;
    using OnDataCb  = std::function<void(const StreamMessage&)>;
    using OnCloseCb = std::function<void(int reason)>;
    virtual void SetCallbacks(OnDataCb on_data, OnCloseCb on_close) = 0;
};

class ITransport {
public:
    virtual ~ITransport() = default;
    virtual int  Connect(const std::string& host, uint16_t port) = 0;
    virtual void Close() = 0;
    virtual std::shared_ptr<ITransportStream>
        OpenStream(Reliability r) = 0;
    using OnStreamCb = std::function<void(std::shared_ptr<ITransportStream>)>;
    virtual void SetOnIncomingStream(OnStreamCb cb) = 0;
};

std::unique_ptr<ITransport> CreateQuicTransport(/* config */);
std::unique_ptr<ITransport> CreateLegacyMiniRtcTransport(/* config */);

} // namespace crossdesk::net
```

### 3.2 与 minirtc 的集成点

- **不动** `IceTransport` 内部；为它实现一个 adapter `MiniRtcTransportAdapter : ITransport`，对外暴露相同 stream 抽象。
- 新增 `QuicTransport : ITransport`，基于 MsQuic C API (`MsQuicOpen2`, `MsQuicConfigurationOpen`, `MsQuicConnectionOpen` ...) 实现。
- 上层（如未来「剪贴板同步」「文件传输」「控制信令」）调用 `ITransport::OpenStream(Reliable)` 而不再 `#include "minirtc.h"`。
- 配置入口加一个开关：`config_center` 中新增 `transport_backend = { "auto" | "ice" | "quic" }`。

### 3.3 线程模型

- MsQuic 自带 worker 线程池，回调来自 MsQuic 线程。`QuicTransport` 必须把回调 marshal 到 CrossDesk 现有 reactor（`submodules/minirtc/src/thread/`），保持线程一致性，避免和 GUI 渲染线程混用。
- 提供一个 `PostToLoop(std::function<void()>)` 工具，所有 ITransportStream 回调通过它派发。

---

## 4. 详细任务清单

### P0 — 构建与冒烟（建议 1 个里程碑）

- [ ] 评估 OpenSSL/QUICTLS 选型；必要时把 `xmake/options.lua` 中 openssl3 替换为 quictls 或 openssl ≥ 3.5。
- [ ] `git submodule add https://github.com/microsoft/msquic submodules/msquic`，pin 到稳定 tag（如 `v2.4.x`）。
- [ ] 撰写 `thirdparty/msquic/xmake.lua` 本地 package。
- [ ] 在 `xmake/options.lua` / `targets.lua` 中加入 `USE_MSQUIC` 与 `quic_transport` target。
- [ ] 在 `tests/` 下新增 `quic_echo_test`：本机 client/server，单 stream 收发 1 MB 数据。
- [ ] 三平台 CI（或手工）验证 `xmake f --USE_MSQUIC=true && xmake b -vy crossdesk` 通过。

### P1 — 抽象层 + 控制通道试点

- [ ] 落地 §3.1 `ITransport` / `ITransportStream` 接口及单测（mock transport）。
- [ ] 实现 `QuicTransport`：连接建立、单/多 stream、证书校验（开发期允许 self-signed，生产强制 CA 校验）、graceful close。
- [ ] 实现 `MiniRtcTransportAdapter`，包装现有 `IceTransport` 的 datachannel。
- [ ] 在 `src/service/` 中新增一个非关键功能（推荐：**远端剪贴板同步** 或 **文件预览缩略图传输**）作为首个 ITransport 用户。
- [ ] 配置项 `transport_backend` 接入 `config_center` 与设置面板（`src/gui/panels/`）。
- [ ] 弱网测试：用 `tc netem` (Linux) / Network Link Conditioner (mac) 模拟 5% / 20% 丢包 + 200ms 抖动，对比 QUIC vs DataChannel。

### P2 — 信令替换（独立评审后启动）

- [ ] 设计 QUIC 上的信令协议（仍用 JSON over reliable stream，或迁到 HTTP/3 + cpp-httplib h3 替代）。
- [ ] server 端改造：自托管 docker 镜像新增 QUIC listener、暴露 UDP 端口。
- [ ] 客户端 `WsClient` 替换为 `SignalingClient`（接口不变，底层多实现）。
- [ ] 灰度开关与协议探测（先尝试 QUIC，失败回落 WSS）。

### P3 — Relay 数据面（独立评审后启动）

- [ ] Relay 模式 SRTP→QUIC datagram 迁移评估（A/V 仍用 datagram，控制走 reliable stream）。
- [ ] 拥塞控制集成：MsQuic CC 与现有 `src/qos/` 的协调策略。
- [ ] 端到端延迟基准测试。

### P4 — 服务端 / 部署 （独立评审后启动）

- [ ] crossdesk-server Docker 镜像内置 msquic。
- [ ] 证书签发流程（Let's Encrypt / 自签 CA）与现有 `/var/lib/crossdesk` 证书目录对齐。
- [ ] 端口规划与 NAT/防火墙文档更新（README §自托管服务器）。

---

## 5. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| OpenSSL 3.3.2 缺少 QUIC server API | P0 阻塞 | 提前 PoC；必要时切 quictls/openssl 或升级至 3.5+ |
| macOS 上 MsQuic CMake 构建踩坑（codesign / dylib） | 中 | 优先静态库 (`QUIC_BUILD_SHARED=OFF`)；docs/FAQ 增补 |
| Windows 默认 Schannel 与 Linux/mac OpenSSL 行为差异 | 中 | 统一强制使用 OpenSSL TLS provider |
| 与 minirtc 双栈带来的二进制膨胀 | 低 | 用 `USE_MSQUIC` 选项控制，发布版按需开关 |
| Relay 切换后服务器需同步升级（兼容性断裂） | 高 | 协议版本协商；保留双协议过渡期至少 2 个 release |
| QUIC UDP 443 在企业网络被防火墙阻断 | 中 | 实现 fallback 到 WSS/TURN-TCP 的路径 |
| msquic 自带线程模型与 CrossDesk reactor 冲突 | 中 | 全部回调 marshal 到主 loop（§3.3） |
| 许可证：MsQuic = MIT，CrossDesk = LGPL-3.0 | 低 | 兼容；在 LICENSE / README 中补充 NOTICE |

---

## 6. 验证与质量门禁

- **构建**：`xmake f --USE_MSQUIC=true -m release && xmake b -vy crossdesk` 在 macOS arm64 / Ubuntu 22.04 / Windows 11 三平台 0 警告关键链。
- **单元测试**：`tests/quic_echo_test`、`tests/transport_interface_test`（mock）；覆盖率目标 ≥ 70%（针对新增 `src/quic_transport/`）。
- **集成测试**：脚本驱动两端 client，跑 1 小时压力收发，验证内存（`leaks` / valgrind）与 fd 无泄漏。
- **回归**：所有 P1 改动必须保留 `USE_MSQUIC=false` 默认编译路径，确保现网用户零影响。
- **性能基线**：使用 [iperf3-quic](https://github.com/microsoft/msquic/tree/main/src/tools/perf) 风格基准；记录 P50/P95/P99 RTT、吞吐、CPU 占用。

---

## 7. 时间与人力估算（参考）

- 单人投入下，P0 完整跑通 ~1-2 个迭代；P1 ~2-3 个迭代；P2+ 进入独立项目评审。
- 关键路径：**OpenSSL/QUICTLS 选型 → xmake package 打通 → 三平台 CI 绿** 是后续一切的前置。

---

## 8. 决策记录 (ADR 待补)

需要在 `docs/adr/` 下补充以下决策记录（建议在 P0 启动前完成）：

1. **ADR-001** TLS provider 选型：OpenSSL 3.5 vs quictls。
2. **ADR-002** MsQuic 引入方式：xmake local package + git submodule。
3. **ADR-003** 抽象层 `ITransport` 引入与 minirtc 并存策略。
4. **ADR-004** 是否在 P2 引入 HTTP/3 替换 WebSocket 信令。

---

## 9. 待确认问题（请项目维护者回答后再启动 P0）

1. **目标 MsQuic 版本**：建议锁定 `v2.4.x`（最新 LTS 风格分支），是否可接受？
2. **是否允许引入 quictls/openssl fork**？还是必须留在 upstream openssl3？
3. **首个试点功能**：建议剪贴板同步，是否同意？或是否已有别的需求优先级更高？
4. **Server 端是否同步开工**？还是先纯客户端 PoC？
5. **是否保留 `USE_MSQUIC=false` 作为默认值**至少到 P3 完成（推荐：是）？

---

_文档作者：(待填)_ ｜ _最后更新：2026-05-19_
