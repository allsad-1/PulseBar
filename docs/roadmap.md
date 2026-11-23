# PulseBar — Windows 11 任务栏硬件与 AI 额度监控面板开发路线图

> **项目定位**：一款原生轻量、零侵入、高颜值的 Windows 11 任务栏常驻监控条。在任务栏空白区域以极简胶囊形态，实时展示 **CPU / GPU（占用与温度）、内存、游戏帧率（FPS）、实时网速**，以及 **Codex 剩余额度（含重置倒计时）** 与 **DeepSeek API 余额**。

---

## 1. 产品形态与视觉设计（对标截图风格）

### 1.1 任务栏内联布局示意图

贴合 Windows 11 任务栏高度（48px @ 100% DPI），采用暗色半透明圆角胶囊（Segmented Pill）样式，紧贴系统托盘左侧空白区，不遮挡左侧应用图标与右侧系统时间：

```text
┌───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│ [Win] [抖音] [Edge] [文件夹] ...    [CPU 15% 68°C] [GPU 1% 51°C] [RAM 60.5%] [FPS 60] [↑0.2 ↓4.8 Mbps] [CDX 82%] [DS ¥48.5]   23:53 │
└───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
                                      ▲                                                                                     ▲
                                      └──────────────────────── PulseBar 独立渲染区域 ───────────────────────────────────────┘
```

### 1.2 美观与细节规范
* **等宽数字（Tabular Numerals）**：使用 `Segoe UI Variable` / `Inter` 并开启 `tnum` OpenType 特性，数值从 `1%` 跳变到 `100%` 时胶囊宽度不抖动、不闪烁。
* **语义化动态配色**：
  * **温度**：`< 70°C` 翠绿 (`#22C55E`) / `70~84°C` 琥珀 (`#F59E0B`) / `≥ 85°C` 警戒红 (`#EF4444`)。
  * **AI 额度 / 余额**：充裕为青白/浅绿，剩余 `< 20%` 转琥珀色，`< 5%` 或余额低于阈值转红色提醒。
* **自适应空间防重叠**：
  * 通过 Windows UI Automation / Win32 实时检测任务栏图标区域右边界与系统托盘左边界。
  * 当打开应用过多导致空间不足时，自动切换为**紧凑模式**（合并上传/下载为双行微字、折叠空闲时为 `0` 的 FPS 胶囊）或支持滚动/轮播切换硬件组与 AI 组。
* **二级浮窗（Hover / Click Flyout）**：
  * 任务栏胶囊保持极度克制，**左键点击或悬停**时向上弹出 Win11 Fluent 风格详情卡片：
    * 硬件详情：CPU 单核负载/功耗、GPU 显存（VRAM）占用、最近 60 秒迷你折线图。
    * AI 详情：Codex 5 小时 / 7 天窗口进度条 + 精确重置倒计时、DeepSeek 赠送余额 vs 充值余额明细、手动立即刷新按钮。

---

## 2. 核心性能与安全指标预算（硬性红线）

为了实现“高效、安全、不占用太多资源”，开发全周期严格执行以下预算：

| 指标维度 | 目标预算 | 关键保障手段 |
| :--- | :--- | :--- |
| **常驻内存占用** | **≤ 15 MB**（峰值 ≤ 25 MB） | 禁用 Electron / WebView2，采用原生 Direct2D 渲染 |
| **空闲 CPU 占用** | **< 0.3%**（1s 刷新周期） | 增量脏区重绘（数值不变不渲染）、纯原生系统 API |
| **安装包体积** | **≤ 5 MB**（单文件绿色版可选） | Rust Release LTO 编译，无庞大运行时依赖 |
| **反作弊安全** | **零封号风险** | FPS 采集绝不注入 DLL，仅使用系统级 ETW 监听 |
| **驱动与系统安全** | **零高危驱动** | 默认不依赖已被 Defender 拦截的旧版 `WinRing0` |
| **凭据与隐私安全** | **零明文落盘 / 零遥测** | API Key 存入 Windows 凭据管理器（DPAPI 加密），仅直连官方域名 |

---

## 3. 技术栈选型与对比

**首选推荐架构：`Rust` + `windows-rs` (Win32) + `Direct2D / DirectWrite` + `DirectComposition`**

| 方案对比 | 内存占用 | CPU 占用 | 渲染质感 | 安全性与体积 | 综合评估 |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Rust + Win32 + Direct2D（推荐）** | **8 ~ 15 MB** | **< 0.3%** | 极佳（原生硬件加速 + ClearType） | 内存安全、单文件 ~4 MB | **综合最优，完美契合需求** |
| C++20 + Win32 + Direct2D | 6 ~ 12 MB | < 0.3% | 极佳 | 需谨慎处理内存与异步网络 | 备选（适合 C++ 团队） |
| C# (.NET 8/9 NativeAOT) + WinUI/WPF | 35 ~ 70 MB | 0.5 ~ 1% | 极佳 | 生态友好，但体积与内存略高 | 追求极速开发时的备选 |
| Tauri 2 (Rust + WebView2) | 60 ~ 120 MB | 0.5 ~ 1.5% | 优秀（CSS 编写快） | WebView2 进程常驻内存偏高 | 不推荐用于极简任务栏常驻 |
| Electron | 150 ~ 300 MB | 1 ~ 3% | 优秀 | 资源浪费严重 | **直接排除** |

---

## 4. 系统分层架构设计

```text
+-----------------------------------------------------------------------+
|                         UI 渲染与交互层 (UI Layer)                     |
|  Direct2D 胶囊渲染引擎 │ 任务栏位置同步器 │ 托盘菜单 │ Fluent 详情浮窗  |
+-----------------------------------------------------------------------+
                                   ▲  (无锁/低频 Channel 推送 Snapshot)
+-----------------------------------------------------------------------+
|                      状态聚合与调度中心 (Core Hub)                     |
|   1Hz 硬件调度器 │ 按需 FPS 开关 │ 低频 AI 轮询器 (60s~300s) │ 告警判定 |
+-------------------+----------------------------+----------------------+
                    ▲                            ▲
+-------------------+------------+  +------------+----------------------+
|    本机遥测采集层 (Telemetry)   |  |     AI 额度插件层 (AI Providers)  |
| - CPU/RAM: Win32 Native API    |  | - DeepSeek: 官方 REST Balance API |
| - Net: IP Helper (GetIfTable2) |  | - Codex: 本地会话/CLI只读+官方轮询 |
| - GPU: NVML / ADLX / PDH       |  | - 扩展接口: Trait Provider (支持未来|
| - CPU温度: WMI / PawnIO(可选)  |  |   接入 Claude Code / OpenAI 等)   |
| - FPS: ETW PresentMon 引擎     |  |                                   |
+--------------------------------+  +-----------------------------------+
                                                 ▲
+------------------------------------------------+----------------------+
|                     安全与配置基础设施 (Infra)                         |
|   Windows DPAPI / Credential Manager │ TLS 1.3 直连 │ TOML 轻量配置    |
+-----------------------------------------------------------------------+
```

---

## 5. 核心模块技术实现细则

### 5.1 Windows 11 任务栏嵌入与防崩溃机制
Windows 11 已弃用传统的 `DeskBand` COM 接口，强行使用 `SetParent` 挂载到 `Shell_TrayWnd` 在部分 Win11 版本更新后容易引发 DPI 错位、焦点劫持或资源管理器异常。推荐采用**无侵入浮层协同方案**：

1. **窗口样式**：创建无边框分层窗口，配置 `WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST`，配合 `DirectComposition` 实现真正的硬件加速每像素透明（无毛边）。
2. **精准定位**：
   * 监听 `Shell_TrayWnd`（任务栏）与 `TrayNotifyWnd`（系统托盘）的几何矩形。
   * 监听 `WinEventHook`（任务栏尺寸变化、显示器分辨率/DPI 变化 `WM_DPICHANGED`、任务栏自动隐藏状态）。
   * 兼容 **居中图标** 与 **左对齐图标** 两种模式，支持手动微调水平偏移量（像素级锁位）。
3. **全屏智能隐藏**：调用 `SHQueryUserNotificationState` 检测全屏游戏/视频（`QUNS_RUNNING_D3D_FULL_SCREEN`），全屏时自动隐藏任务栏条，绝不挡画面；退出全屏立即恢复。
4. **Explorer 重启自愈**：注册 `RegisterWindowMessageW("TaskbarCreated")`，当 Windows 资源管理器重启时自动重新对齐并恢复显示。

### 5.2 硬件与帧率（FPS）采集策略

| 监控项 | 首选采集方案（零侵入 / 低开销） | 降级兜底方案 | 刷新频率 |
| :--- | :--- | :--- | :--- |
| **CPU 占用** | Win32 `GetSystemTimes` / `NtQuerySystemInformation` | PDH `\Processor(_Total)\% Processor Time` | 1000 ms |
| **CPU 温度** | 优先读取非特权传感器 / 独立签名轻量服务（如 **PawnIO** 替代旧版 WinRing0） | 无权限时优雅隐藏温度字段，提示一键授权模式 | 2000 ms |
| **GPU 占用与温度** | **NVIDIA**: 动态加载系统自带 `nvml.dll`<br>**AMD**: `ADLX` SDK<br>**Intel**: `IGCL` | Windows PDH `GPU Engine`（占用）+ 隐藏不支持的温度 | 1000 ms |
| **内存占用** | Win32 `GlobalMemoryStatusEx` | 无（系统原生 100% 可用） | 1000 ms |
| **实时网速** | IP Helper API `GetIfTable2`（差值计算上传/下载，过滤回环与虚拟网卡） | 自适应单位 `KB/s` / `MB/s` / `Mbps` 可切换 | 1000 ms |
| **实时 FPS** | **Intel PresentMon (ETW 模式)**：监听系统 `Microsoft-Windows-DXGI` / `D3D` 呈现事件，按前台焦点窗口 PID 过滤统计帧率 | **智能休眠**：检测到桌面空闲/非 3D 应用时自动暂停 ETW 监听，显示 `0` 或隐藏，彻底省电省 CPU | 500~1000 ms（游戏时激活） |

> **为什么坚持 ETW 方案获取 FPS？**  
> 传统 Hook（类似 RTSS/DLL 注入）容易触发腾讯 ACE、Riot Vanguard、EAC 等反作弊系统拦截。ETW 是 Windows 内核原生性能追踪机制，只读事件流、不修改任何进程内存，对所有网游和系统绝对安全。

### 5.3 AI 额度监控模块（Codex + DeepSeek）

抽象统一 `QuotaProvider` 接口，异步非阻塞拉取，独立容错（断网或单家超时绝不影响本机硬件刷新）：

1. **DeepSeek API 余额**：
   * **接口**：官方只读端点 `GET https://api.deepseek.com/user/balance`
   * **认证**：Header `Authorization: Bearer <API_KEY>`
   * **解析字段**：`is_available`、`balance_infos[].currency`（CNY / USD）、`total_balance`（总余额）、`granted_balance`（赠金）、`topped_up_balance`（充值余额）。
   * **轮询策略**：默认每 **5 分钟** 刷新一次，支持手动点击刷新与指数退避重试（429/5xx 错误时自动拉长间隔）。
2. **Codex 额度与重置倒计时**：
   * **双模式兼容策略**（兼顾无感体验与稳定性）：
     * **模式 A（推荐 · 被动只读本地元数据）**：监听本机 Codex CLI 本地状态文件/会话日志元数据或官方本地 `app-server` 状态接口，提取 5 小时 / 7 天限额百分比与重置时间戳，**零额外网络请求、不触碰用户对话内容**。
     * **模式 B（可选 · 官方授权轮询）**：由用户在本地独立配置受保护的只读凭证，严格限流（默认每 **5~10 分钟** 拉取一次），展示剩余百分比与 `2h 14m` 倒计时。
   * **健壮性设计**：记录数据“新鲜度时间戳（Last Updated）”，当超过 15 分钟未更新时胶囊数字略微降暗，悬停提示上次同步时间，避免展示过时假数据。

### 5.4 安全与隐私设计清单
* **凭据加密存储**：所有 API Key 与 Token 通过 Windows Credential Manager（`CredWriteW` / `CredReadW`）基于当前登录用户 DPAPI 加密存储，配置文件 `config.toml` 中**仅存开关、布局和样式，绝不存密钥**。
* **网络白名单策略**：内置严格域名校验，除用户开启的官方 API 端点（`api.deepseek.com` 等）外，不发起任何外部请求，无埋点、无自动上报。
* **最小权限原则**：主程序以普通标准用户权限运行；仅在用户主动开启“CPU 核心温度”或“ETW 帧率监控”时，通过受控的轻量辅助服务或标准提示请求必要权限（`Performance Log Users` 组可免管理员运行 ETW）。
* **可验证构建**：开源代码 + GitHub Actions CI 自动构建发布 Release + 附带 SHA-256 校验值。

---

## 6. 建议项目目录结构（Rust 架构示例）

```text
pulsebar/
├── Cargo.toml
├── docs/
│   └── roadmap.md                 # 本文档
├── src/
│   ├── main.rs                    # 程序入口、单实例互斥锁、日志与崩溃捕获
│   ├── config/
│   │   ├── mod.rs                 # TOML 配置读写与热重载
│   │   └── secrets.rs             # Windows Credential Manager (DPAPI) 封装
│   ├── core/
│   │   ├── state.rs               # 全局不可变指标快照 (MetricsSnapshot)
│   │   └── scheduler.rs           # 异步多频调度器 (1s 硬件 / 按需 FPS / 5m AI)
│   ├── telemetry/
│   │   ├── cpu_ram.rs             # Win32 CPU & RAM 采集
│   │   ├── gpu_nvml.rs            # NVIDIA NVML 动态加载采集
│   │   ├── gpu_amd_intel.rs       # AMD ADLX / Intel / PDH 兜底
│   │   ├── network.rs             # IP Helper 实时网速计算
│   │   ├── temp_sensor.rs         # CPU 温度安全读取封装
│   │   └── fps_etw.rs             # ETW Present 帧率采集与空闲休眠
│   ├── providers/
│   │   ├── mod.rs                 # QuotaProvider Trait 定义
│   │   ├── deepseek.rs            # DeepSeek /user/balance 客户端
│   │   └── codex.rs               # Codex 本地状态监听 / 额度解析器
│   └── ui/
│       ├── taskbar_anchor.rs      # Win11 任务栏坐标跟踪、DPI适配、全屏隐藏
│       ├── renderer_d2d.rs        # Direct2D 胶囊绘制、动态配色、字体排版
│       ├── flyout.rs              # 点击展开的 Fluent 详情面板
│       ├── tray.rs                # 系统托盘右键菜单（设置/开关/自启/退出）
│       └── settings_wnd.rs        # 极简原生设置窗口（配置 Key 与显示项）
└── assets/
    └── icons/                     # 矢量图标与托盘图标
```

---

## 7. 分阶段开发路线图（Milestones）

预计单人业余时间 **5 ~ 6 周**（或全职 **2 ~ 3 周**）可完成从 MVP 到 1.0 正式版发布：

### Phase 0：工程初始化与窗口原型（第 1 周前半）
* [ ] 搭建 Rust Workspace 与 `windows-rs` + `tokio` 异步基础框架。
* [ ] 实现单实例互斥锁（防止重复启动）与系统托盘图标。
* [ ] 跑通 **Direct2D + DirectComposition 透明无边框窗口**，在 Win11 任务栏指定位置渲染静态胶囊原型（还原截图视觉效果）。
* [ ] 完成 DPI 缩放（100% / 125% / 150% / 200%）与 `TaskbarCreated` 重启自愈测试。

### Phase 1：核心硬件遥测闭环 — MVP 达成（第 1 周后半 ~ 第 2 周）
* [ ] 实现 CPU 占用、RAM 占用、上传/下载实时网速采集（纯 Win32 API）。
* [ ] 接入 NVIDIA NVML 动态加载，读取 GPU 占用、温度与显存。
* [ ] 接入 Direct2D 脏区重绘循环，实现数值平滑刷新与温度阈值变色。
* [ ] 实现任务栏左右边界检测与全屏游戏自动隐藏。
* **阶段验收标准**：任务栏稳定显示 `CPU / GPU / RAM / ↑↓网速`，内存 < 12 MB，CPU < 0.3%。

### Phase 2：安全 FPS 监控与 CPU 温度补全（第 3 周）
* [ ] 集成 ETW DXGI/Present 帧率监听模块，精确绑定前台焦点进程。
* [ ] 实现 3D 活动检测与空闲自动休眠（桌面状态零 ETW 开销）。
* [ ] 补全 AMD / Intel GPU 支持（ADLX / PDH 兜底）。
* [ ] 接入安全 CPU 温度模块（无特权源优先 + 可选签名驱动辅助服务模式）。
* **阶段验收标准**：主流 DX11/DX12/Vulkan 游戏准确显示实时 FPS，无任何反作弊告警。

### Phase 3：AI 额度模块 — DeepSeek & Codex（第 4 周）
* [ ] 封装 Windows Credential Manager（DPAPI）密钥安全存取模块。
* [ ] 实现 DeepSeek Balance API 客户端（CNY/USD 自动识别、余额低阈值变色、断网重试退避）。
* [ ] 实现 Codex 额度适配器（支持本地 Codex 会话/状态只读 + 可选授权轮询、5h/7d 重置倒计时）。
* [ ] 在任务栏胶囊栏无缝追加 `[CDX 82%]` 与 `[DS ¥48.5]` 胶囊，支持一键隐藏/显示任意单项。
* **阶段验收标准**：密钥零明文保存，AI 额度准确同步且断网不阻塞硬件刷新。

### Phase 4：交互打磨、详情浮窗与个性化设置（第 5 周）
* [ ] 开发点击/悬停弹出的 **Fluent 风格二级浮窗**（展示 VRAM、Codex 详细进度条与重置倒计时、DeepSeek 赠金/充值明细、一键刷新）。
* [ ] 开发简洁设置界面：
  * 自由勾选/拖拽排序要显示的胶囊模块；
  * 自定义网速单位（`Mbps` vs `MB/s`）、刷新频率、温度/余额告警阈值；
  * 自定义任务栏对齐位置微调（X 轴偏移）、透明度与浅色/深色主题跟随系统；
  * 开机自启（通过任务计划程序或注册表 Run 项一键开关）。
* [ ] 完善空间不足时的**自动紧凑/折叠策略**。

### Phase 5：性能压测、安全审计与发布（第 6 周）
* [ ] **72 小时连续稳定性与内存泄漏压测**（验证休眠唤醒、拔插外接显示器、切换分辨率、网络断连恢复）。
* [ ] 安全自检：抓包确认无非白名单外联、检查磁盘无明文密钥、VirusTotal 误报排查。
* [ ] 配置 GitHub Actions 自动编译打包（绿色免安装 `.zip` + MSIX/Inno Setup 安装包 + SHA256 校验）。
* [ ] 撰写中英文 `README.md` 与截图演示，发布 `v1.0.0`。

---

## 8. 未来可扩展规划（v1.1+）

* **更多 AI 提供商插件**：一键扩展 Claude Code、OpenAI API Credits、OpenRouter、Cursor 等额度监控。
* **游戏内迷你 HUD 开关**：可选轻量悬浮条，在全屏无边框模式下仍可瞥见 FPS 与温度。
* **多显示器支持**：允许将监控条固定在副屏任务栏，主屏全屏游戏时副屏持续监控硬件与 AI 额度。
* **自定义插件脚本（Local JSON Feed）**：允许高级用户通过本地脚本输出自定义指标（如 NAS 温度、服务器负载）挂载为新胶囊。