# PulseBar

PulseBar 是 Windows 11 的原生任务栏监控面板。状态栏显示硬件和额度，大面板显示详情、逻辑核心和最近 60 秒趋势。

原始设计保存在 [项目路线图](docs/roadmap.md)，当前功能和构建方式以本 README 为准。

## 启动方式

把解压后的文件保留在同一目录。四个启动器共用 `PulseBar.exe`，可以分别创建快捷方式。

| 启动器 | 行为 |
| --- | --- |
| `PulseBar-Panel.exe` | 打开大面板。拖动左侧标题区移动，右上角 × 只关闭大面板。位置会保存。 |
| `PulseBar-Status.exe` | 启动任务栏状态栏。正常模式下鼠标点击和拖动穿透。 |
| `PulseBar-HideStatus.exe` | 关闭状态栏。已经打开的大面板继续显示。 |
| `PulseBar-Edit.exe` | 打开编辑窗口，可把状态栏拖到任意显示器。点「保存并锁定」或右上角 × 后，状态栏保持置顶和鼠标穿透，重启后恢复保存的位置。 |

直接双击 `PulseBar.exe` 等同启动状态栏。状态栏与大面板都关闭时，PulseBar 退出。托盘右键菜单也提供这些入口。「恢复任务栏停靠」会把状态栏移回主任务栏，任务栏暂时没有可用空间时会显示在桌面工作区的右下角。空间过窄时，状态栏先切换紧凑布局，再自动轮播。RAM 以 GiB 显示，完整布局与大面板同时显示已用容量和总容量。

## 数据和设置

- **DeepSeek 余额**通过官方 API 查询。HTTP 401 表示服务器拒绝了当前凭据。到 DeepSeek 开放平台手动创建 API Key，将完整值粘贴到编辑窗口的「更换 DeepSeek Key」。平台列表里的截断预览无法验证。HTTP 401 或 403 会暂停自动请求，可用「刷新额度」重新查询。密钥保存在当前用户的 Windows 凭据管理器。[DeepSeek API Key 页面](https://platform.deepseek.com/api_keys) · [余额接口](https://api-docs.deepseek.com/api/get-user-balance/)
- **FPS** 默认开启自动采集。绿色包附带 Intel PresentMon 2.6.0 控制台版。在有 3D 活动的前台应用中等待几秒后会读到显示帧率。若提示权限不足，请让 Windows 管理员将当前账号加入 `Performance Log Users`（性能日志用户）组，然后注销并重新登录。编辑窗口也有权限说明和自定义 PresentMon 路径入口。[PresentMon 官方文档](https://github.com/GameTechDev/PresentMon#troubleshooting)
- **CPU 温度**默认读取固件的 CPU 热区，也支持读取正在运行的 Libre Hardware Monitor 发布的 CPU 封装温度。两个来源都无读数时显示「CPU 温度暂无数据」。Libre Hardware Monitor 能否访问 CPU 传感器取决于硬件和它的驱动配置。[Libre Hardware Monitor](https://github.com/LibreHardwareMonitor/LibreHardwareMonitor)
- **GPU** 默认读取 Windows GPU Engine 占用率。托盘里的「读取 NVIDIA 温度和显存」会按需加载 NVML。
- **Codex** 在托盘开启「读取本机 Codex 限额记录」后显示本机支持的 5 小时或 7 天窗口。过期的记录等待新的限额事件。

「更多设置」打开 `%LOCALAPPDATA%\PulseBar\settings.ini`。配置文件支持项目顺序、网速单位、主题、透明度、刷新频率、温度和余额阈值。修改文件后，在托盘选「重新读取设置」。托盘也可开关登录自启。

## 从源码构建

需要 Visual Studio 2022 或 Build Tools 的「使用 C++ 的桌面开发」工作负载和 Windows SDK。

```powershell
.\build.ps1 -Package -IncludePresentMon
```

脚本运行自动化检查，并在 `dist` 生成启动器、绿色包和 SHA-256 校验文件。`-IncludePresentMon` 附带固定版本的官方控制台 EXE 和许可证。去掉此参数可构建不带 PresentMon 的版本。

```powershell
.\dist\PulseBar.exe --demo --details
Start-Process .\dist\PulseBar.exe -ArgumentList '--probe', 'build\probe.txt' -Wait
```

`--demo` 使用演示数据。`--probe` 将本机传感器和任务栏诊断写入指定文件。
