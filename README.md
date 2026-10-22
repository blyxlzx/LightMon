# LightMon

原生 C++ / Win32 编写的 Windows 11 任务栏监控工具。固定两行显示，占用任务栏空位，CPU 与忙核同列，硬件指标在左，网速和流量在右。没有 Electron、WebView 或常驻脚本宿主。

[下载安装包 / 便携包](https://github.com/blyxlzx/LightMon/releases/latest) · [English](README.en.md) · [数据来源](docs/METRICS.md) · [CPU 温度配置](docs/CPU-TEMPERATURE.md)

## 安装与使用

支持 **Windows 11 x64（22000 或更新）**，底部水平任务栏；不支持 Windows 10、ARM64 或第三方任务栏替代程序。界面目前为中文。Windows 更新、任务栏布局和显卡驱动可能影响兼容性。

- 安装版：下载 `LightMon-1.1.28-Setup-x64.exe`，按向导安装到当前用户目录。开机启动和桌面快捷方式均可选，默认不勾选。
- 便携版：下载 `LightMon-1.1.28-Portable-x64.zip`，完整解压到可写目录，运行 `LightMon.exe`。请保留附带文件。
- 右键任务栏数值或托盘图标，选择监控项、主题、刷新间隔和详情。固定两行，不遮挡已识别的任务栏图标；所有任务栏空位均不足时暂时隐藏，可以减少显示项。
- 默认开启 CPU、最忙逻辑核、内存、网速和当日流量；GPU、磁盘采样和自动 CPU 温度读取默认关闭。启用更多提供程序会增加内存与 CPU 使用，实际占用可在“关于”查看。
- `总↓` / `总↑` 表示**当日累计**下载 / 上传，不是历年累计。程序未运行期间的流量无法补记；本地日期变化后重新累计。

发布文件尚未购买代码签名证书，Windows 可能显示未知发布者。只从本仓库 Release 下载，并对照附带的 `SHA256SUMS.txt` 校验；不要关闭系统安全保护。

## 开机启动与恢复

安装时可勾选登录启动。便携版可在解压目录打开 PowerShell，执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\Configure-Startup.ps1 -Action Enable
```

将 `Enable` 改为 `Status` 可检查，改为 `Disable` 可取消。它使用当前用户、普通权限的计划任务，登录后启动，并每分钟检查一次；已有实例只保留一个。主动退出后，本次登录内暂停自动恢复，手动启动或重新登录后恢复。脚本拒绝覆盖属于另一安装目录的同名任务。

此开关不授予硬件读取权限。CPU 温度有单独、可选的配置步骤，当前仅支持 AMD family `0x19` / model `0x61`；Intel 和其他 AMD 型号的 CPU 温度尚不支持。不会自动下载或安装 PawnIO 驱动。详见 [CPU 温度指南](docs/CPU-TEMPERATURE.md)。

## 指标与限制

| 指标 | 数据来源与限制 |
| --- | --- |
| CPU / 忙核 | Windows CPU 忙碌时间；不同于按性能归一化的 CPU utility，不能保证与每个版本任务管理器逐秒相等。频率放在详情。 |
| 独显 / 核显 | Windows GPU Engine；同一物理引擎跨进程汇总，再取最忙引擎。当前独显绑定依赖 NVIDIA NVML；核显由 DXCore 识别。不是任意多显卡列表。 |
| 显存 / 温度 | NVIDIA NVML、WDDM、Windows 存储温度接口；传感器和权限由驱动决定。未开放显示 `—`，不估算。 |
| 网速 / 流量 | 已连接的物理以太网 / Wi-Fi 接口字节计数；排除虚拟、VPN 和回环接口。是网卡传输量，包含局域网流量，不是运营商账单。 |
| 磁盘 | 最忙物理磁盘活动时间比例；盘温显示有效传感器中的最高值。 |

各项采样、聚合和缺失值语义见 [docs/METRICS.md](docs/METRICS.md)。

## 数据保存在本机

配置为程序目录内的 `LightMon.ini`，当日流量检查点为 `LightMon.traffic`。程序不上传遥测，不内置账号、广告或更新下载器。安装包不含开发者的配置、流量、日志或电脑标识。卸载会保留用户配置与流量数据；可在退出后自行删除剩余数据目录。若配置过可选 CPU 温度任务，请先按温度指南移除它。

## 从源码构建

安装 Visual Studio 2022 或更新版本的 C++ 桌面开发工具与 Windows SDK。在仓库根目录执行：

```powershell
.\source\build.cmd
```

输出为根目录 `LightMon.exe`、`LightMonCpu.exe`。构建会先生成温度 helper，再把其 SHA-256 写入主程序，因此不要混用不同构建的两个可执行文件。

完整打包需 [Inno Setup 6.5 或更新版本](https://jrsoftware.org/isdl.php)：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\Build-Release.ps1 -InnoCompilerPath "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
```

仅生成 ZIP 可加 `-SkipInstaller`。产物位于 `dist/`。图标为本项目原创，可通过 `scripts/Generate-Icon.ps1` 重建。打包脚本仅收集列明的公开文件，并拒绝常见的个人路径、凭据和运行记录。

## 许可与反馈

LightMon 自有代码采用 [MIT](LICENSE) 许可。附带 PawnIO 模块及接口通知保留各自许可，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)；安装包和便携包均附可重建源码与第三方材料。PawnIO 驱动不随包分发。

欢迎通过 [Issues](https://github.com/blyxlzx/LightMon/issues) 报告问题。请提供 Windows 版本、缩放比例、任务栏布局、硬件型号及复现步骤，先移除日志或截图中的个人信息。本版在开发机器完成构建和隔离安装验证，未覆盖所有 Windows 版本或硬件。
