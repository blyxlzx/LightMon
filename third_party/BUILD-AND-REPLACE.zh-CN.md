# 源码、重建与模块替换

这些材料用于查看、修改和重新构建。安装程序不运行这里的编译器或源码，也不安装 PawnIO 驱动。

## 对应关系

PawnIO.Modules 为 **0.2.11**，源码提交为 `52a7e536dff3e53c96917a28caac5e0fa6510696`。

- `PawnIO.Modules-0.2.11/AMDFamily17.p`：上游原样源码，SHA256 `8A1808A2E51E184F96D17362991AE900C65A1A23135A68C256EC6759E8297BB9`。
- `PawnIO.Modules-0.2.11/AMDFamily17.bin`：官方发布模块，10652 字节，SHA256 `DAE74615761B78BDF064DFB3E136252DDCC6FC727D88F14738D0E5800D427A91`。
- `PawnIO.Modules-0.2.11/include/`：该提交的全部 7 个 include 文件及其原始通知。
- `archives/PawnIO.Modules-0.2.11-source-no-toolchain.zip`：该提交的 37 个非 RPM 文件，包含其他模块源码、许可证、原始 CI 配置。仅省略两个编译器 RPM，其余文件内容原样保留。
- 仓库根目录 `source/`：本版 LightMon 和 CPU 温度 helper 的构建输入。安装包及便携包在 `source/` 内同样提供这些构建输入。

已核对发布资产与对应源码提交，未从源码重新生成官方签名 `.bin`，也未执行修改后的模块。

- [0.2.11 发布页](https://github.com/namazso/PawnIO.Modules/releases/tag/0.2.11)
- [完整原始源码归档](https://codeload.github.com/namazso/PawnIO.Modules/zip/52a7e536dff3e53c96917a28caac5e0fa6510696)
- [原始 CI 构建指令](https://github.com/namazso/PawnIO.Modules/blob/52a7e536dff3e53c96917a28caac5e0fa6510696/.github/workflows/ci.yml)

## 编译 Pawn 模块

解压所附源码归档。上游 CI 使用 Pawn compiler **4.1.7152** 和 64 位 cell；在解压后的上游仓库根目录运行：

```sh
pawncc AMDFamily17.p '-iinclude' '-C64' '-;+' '-(+' '-p'
```

输出为 `AMDFamily17.amx`，不包含上游私钥产生的官方签名，不能据此声称重现了签名发布文件。归档内 `.github/workflows/ci.yml` 保留了 Oracle Linux 9 环境和完整命令。

工具链按需另行获取，不随安装包部署：

| 文件 | 字节数 | SHA256 |
| --- | ---: | --- |
| [pawn-4.1.7152-1.el9.x86_64.rpm](https://raw.githubusercontent.com/namazso/PawnIO.Modules/52a7e536dff3e53c96917a28caac5e0fa6510696/_pawn/pawn-4.1.7152-1.el9.x86_64.rpm) | 265961 | `7B02FA557FA2E3DCDC40A76579F30B2D7387566C102C98F105316CE0AB052F25` |
| [pawn-4.1.7152-1.el9.src.rpm](https://raw.githubusercontent.com/namazso/PawnIO.Modules/52a7e536dff3e53c96917a28caac5e0fa6510696/_pawn/pawn-4.1.7152-1.el9.src.rpm) | 659082 | `6426BE835416DF4AC8296739289825FDA03388324B15E677CEC3589B2A22C032` |

也可从 [Pawn 官方网站](https://www.compuphase.com/pawn/pawn.htm) 获取编译器。其他版本可能生成不同产物。

## 重建 LightMon 和 helper

需要 Windows x64、Visual Studio C++ Build Tools、x64 编译工具和 Windows SDK。`source/build.cmd` 通过 `vswhere.exe` 查找工具。

1. 把整个项目复制到可写的开发目录，保留 `source/` 及其相对结构；不要直接在正在运行的安装目录构建。
2. 在命令提示符运行 `source\build.cmd`。
3. 脚本先构建 `LightMonCpu.exe`，再通过 `generate-helper-hash.ps1` 把 helper 的实际 SHA256 写入 `source/src/cpu_temperature_helper_hash.h`，最后编译 `LightMon.exe`。
4. 两个 EXE 输出到 `source/` 的父目录，中间文件在 `source/build/`。两份 EXE 必须成对使用；只替换 helper 会被原主程序的完整性检查拒绝。
5. 退出旧实例后，在可写文件夹测试这组新 EXE。LightMon 同一登录会话只允许一个实例。

本说明不宣称跨工具链逐字节复现 EXE；编译器、链接器及构建元数据会影响文件字节。

## 使用修改后的模块

模块作为 `LightMonCpu.exe` 的 RCDATA 资源内嵌，不能靠替换安装目录的单独 `.bin` 改变已编译的 helper。

1. 修改 `.p`／include 并编译，准备目标 PawnIO 驱动接受、IOCTL 接口兼容的模块 blob。
2. 把 blob 放到工作副本 `source/src/AMDFamily17.bin`，或调整 `cpu-temperature-helper.rc` 的资源路径。
3. 更新 `cpu-temperature-helper.cpp` 的 `kModuleSha256` 及资源长度检查（上游原值 10652），匹配新 blob。如改变 IOCTL、协议或硬件支持范围，同时修改 helper、主程序及通信协议。
4. 重新运行完整 `source\build.cmd`，生成匹配的新 helper 和主程序，再测试两份新 EXE。

官方 PawnIO 驱动核验模块签名。[上游贡献指南](https://github.com/namazso/PawnIO.Modules/wiki/Contribution-guidelines) 说明，供官方驱动使用的模块需要合入上游，由上游签名发布。自己编译的 `.amx` 不等同于官方签名 `.bin`；重建 LightMon 不提供上游签名私钥，也不解除驱动检查。

上游另提供开发／非受限版本及测试文档。具体要求见 [开发文档](https://github.com/namazso/PawnIO.Modules/wiki/Getting-started-with-PawnIO) 和 [使用说明](https://github.com/namazso/PawnIO.Modules/wiki/Using-PawnIO-Modules)。此安装包不安装这些驱动，不改变系统保护或签名设置。

当前 CPU 温度实现仅接受 AMD family `0x19`、model `0x61`，且要求已安装的 PawnIO 2.x。重建本身不会扩大硬件支持范围。
