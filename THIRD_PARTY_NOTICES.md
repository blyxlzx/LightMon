# 第三方软件与出处

LightMon 自有代码按仓库根目录 `LICENSE` 的 MIT 许可发布。下列第三方文件保留各自的版权和许可，不因放入本仓库而改为 MIT。分发安装包或便携包时，请一并保留本文件、`LICENSES/` 和 `third_party/`。

## PawnIO.Modules 0.2.11

CPU 温度辅助程序的资源中内嵌了上游 `AMDFamily17.bin`。模块由 [PawnIO.Modules](https://github.com/namazso/PawnIO.Modules) 提供，版权所有者为 namazso；`AMDFamily17.p` 采用 **LGPL-2.1-or-later**。本项目没有修改该模块的源码或二进制。

- [上游发布 0.2.11](https://github.com/namazso/PawnIO.Modules/releases/tag/0.2.11)，对应提交 `52a7e536dff3e53c96917a28caac5e0fa6510696`。
- 完整 LGPL 2.1 文本：[`LICENSES/LGPL-2.1.txt`](LICENSES/LGPL-2.1.txt)。上游原始通知也保留在对应源码中。
- [`third_party/PawnIO.Modules-0.2.11/`](third_party/PawnIO.Modules-0.2.11/) 提供模块源码、原始发布二进制、全部 7 个 include 文件和上游许可证。
- 六个 PawnIO header（`core.inc`、`extra.inc`、`native.inc`、`ntstatus.inc`、`pawnio.inc`、`util.inc`）采用 **0BSD**；`registry.inc` 采用 **LGPL-2.1-or-later**。每个文件的原始通知均已保留。
- [`third_party/archives/PawnIO.Modules-0.2.11-source-no-toolchain.zip`](third_party/archives/PawnIO.Modules-0.2.11-source-no-toolchain.zip) 保留该提交的 37 个非 RPM 文件，包含其他模块源码和上游构建配置；各文件内容未修改。两个编译器 RPM 不在此精简归档内，下载地址及校验值见重建说明。
- 对应关系、校验值与来源见 [`third_party/provenance.json`](third_party/provenance.json)；修改、重建和替换方法见 [`third_party/BUILD-AND-REPLACE.zh-CN.md`](third_party/BUILD-AND-REPLACE.zh-CN.md)。

LightMon 使用的签名模块逐字节对应官方发布资产。随包提供源码不表示已独立重建出相同的签名二进制；官方驱动的签名要求由上游规定，本项目不提供上游签名私钥。

**PawnIO 驱动和 PawnIOLib.dll 不随 LightMon 分发或自动安装。** CPU 温度功能需要用户另行安装适用的 PawnIO 2.x。当前 helper 仅接受 AMD family `0x19`、model `0x61`；其他硬件仍可使用不依赖此温度功能的监控项。使用条件与驱动来源见 [PawnIO 上游说明](https://github.com/namazso/PawnIO.Modules/wiki/Using-PawnIO-Modules)。

## NVIDIA NVML 接口声明

`source/src/metrics.cpp` 含与 NVIDIA NVML ABI 对应的精简类型和函数声明，用于动态调用已安装的 NVIDIA 驱动组件。接口布局参考 NVIDIA 官方 `nvml.h`，完整版权、免责声明及美国政府最终用户通知保留在 [`LICENSES/NVIDIA-NVML-Notice.txt`](LICENSES/NVIDIA-NVML-Notice.txt) 和代码注释中。

核对的官方头文件为 [NVIDIA/go-nvml 的 nvml.h](https://github.com/NVIDIA/go-nvml/blob/6b38eea7ed6854b19001f44231e9385661e90c46/pkg/nvml/nvml.h)，标注 NVML 13.4.92。本项目不分发 NVIDIA 驱动或 NVML DLL，也不代表 NVIDIA。

## Inno Setup

Windows 安装程序使用 [Inno Setup](https://jrsoftware.org/) 制作。Inno Setup 版权所有：Jordan Russell，部分版权属于 Martijn Laan。其许可全文见 [`LICENSES/Inno-Setup-License.txt`](LICENSES/Inno-Setup-License.txt)。本仓库不包含 Inno Setup 编译器；构建安装包需要另行安装它。

`packaging/ChineseSimplified.isl` 来自 [Inno Setup 官方源码](https://github.com/jrsoftware/issrc/blob/6ef32198ef1f7b7b375cd4b6b90896c2a58eb4c2/Files/Languages/ChineseSimplified.isl)，维护者为 Zhenghan Yang (Kira)，文件头中的作者信息原样保留。[翻译维护项目](https://github.com/kira-96/Inno-Setup-Chinese-Simplified-Translation) 的 MIT 许可及原始版权见 [`LICENSES/Inno-ChineseSimplified-MIT.txt`](LICENSES/Inno-ChineseSimplified-MIT.txt)。

## 参考项目

LightMon 的任务栏显示曾参考 [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) 的公开渲染方案进行分析。LightMon 自行实现 Win32 窗口、GDI 绘制和数据采样；此处仅列为方案参考，未捆绑 TrafficMonitor 源码或二进制。本项目与 TrafficMonitor、Microsoft、NVIDIA、PawnIO 均无官方隶属关系。
