# CPU 温度与可选自动读取

LightMon 的 CPU 占用、最忙逻辑核、频率与 CPU 温度是不同的数据源。CPU 占用等常规指标不需要安装驱动或管理员权限。

## 当前支持范围

CPU 温度读取目前只支持 **AMD Family 19h、Model 61h** 的 Tctl/Tdie 传感器路径，例如已验证的 Ryzen 7 7800X3D；这不是对所有 AMD 或 Intel 处理器的支持承诺。其余 CPU 显示不支持或缺失，不估算温度。

它使用单独的只读温度助手和已经安装的 [PawnIO](https://pawnio.eu/) 驱动。发布包包含 PawnIO.Modules 的签名模块及对应开源材料，**不包含、不下载、不自动安装 PawnIO 驱动**。如果尚未安装，请先自行审阅 PawnIO 官方说明，决定是否安装。传感器值和访问结果仍取决于硬件、驱动及系统配置。

本版本的自动温度助手固定安装在 `C:\Program Files\LightMon\LightMonCpu.exe`，只支持 Windows 使用默认 `C:\Program Files` 路径的情况。该目录由配置脚本专用，请把普通监控程序安装在其他目录；其他指标不受此限制。

## 手动读取

在任务栏数值上右键，选择“启用 CPU 温度（需要 Windows 授权）”。仅温度助手请求 Windows UAC 授权，主程序继续以普通权限运行。助手随对应的主程序结束而退出。

## 配置随软件自动读取

此功能默认关闭。可选配置仅支持**同一个已登录管理员账户**的 UAC 提权；不支持标准用户在 UAC 中输入另一个管理员账户的凭据。

1. 从 Release 下载完整安装包或便携包，保留 `LightMonCpu.exe`、`LightMonCpu.sha256` 和 `scripts` 目录。将便携包解压后再操作。
2. 确认上述硬件和 PawnIO 条件满足。更新已有自动助手前，先退出 LightMon。
3. 在安装目录打开 **64 位 Windows PowerShell**，运行：

   ```powershell
   powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\Configure-CpuTemperature.ps1 -Action Install
   ```

   该命令仅在此次执行中允许运行这个下载的脚本，不修改系统执行策略。Windows 会为此配置显示一次 UAC。脚本验证调用账户、软件包助手哈希及受保护目录，不安装驱动。

4. 配置成功后，普通方式打开 LightMon，右键勾选“随软件自动读取 CPU 温度”。设置由普通权限主程序保存到自己的配置文件。以后随主程序启动读取，不重复请求 UAC。

配置脚本创建的是当前用户的按需计划任务 `LightMon CPU Temperature <用户 SID>`，没有登录/定时触发器；主程序需要读取时才启动。任务只运行固定受保护助手，不接受程序路径、脚本或任意命令参数。助手校验主进程身份、创建时间、用户、会话及私有共享页，并在主程序结束后退出。无需保存 Windows 密码，不更改全局 UAC 设置。

自动助手有独立的机器级受保护目录，因此本版本配置脚本只管理最初配置它的那个用户。它会拒绝覆盖没有本脚本所有权记录的已有助手或同名任务，也不会迁移其他工具创建的温度任务。遇到这种情况可以继续使用手动读取，或由管理员检查原有配置。

## 状态、更新与移除

只读检查配置状态：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\Configure-CpuTemperature.ps1 -Action Status
```

更新程序后，主程序会校验温度助手必须与自身构建匹配。如果新版更换了助手，先退出 LightMon，再从新版完整目录重新执行 `-Action Install`。脚本只更新由本脚本为同一用户创建、且所有权、哈希及权限均通过检查的配置。拒绝更新时不会通过放宽权限来绕过检查。

移除自动温度读取：先关闭主程序，再从安装目录运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\Configure-CpuTemperature.ps1 -Action Remove
```

它只移除本脚本拥有的当前用户温度任务及固定助手文件；若检测到其他温度任务，会保留共享文件。不删除 PawnIO、不删除普通监控程序、不删除个人流量或配置。重新打开主程序后，取消“随软件自动读取 CPU 温度”。若以后要彻底卸载 LightMon，建议先完成这一步，再运行普通卸载程序。

若非提升窗口报告配置失败，可在**同一账户**的管理员 PowerShell 中运行上述同一个命令查看完整错误；不要改用另一个管理员账户。取消 UAC 不会启用自动温度。

## 验证边界

发布前对脚本做了 Windows PowerShell 5.1 语法、隔离的任务身份/权限边界及只读状态检查；未在本机执行新脚本的实际安装、更新或移除，也没有进行陌生电脑的 UAC 完整流程测试。脚本会拒绝可由普通用户写入的 Program Files/PawnIO 路径，但这不等同于验证第三方驱动的健康或所有硬件兼容性。普通指标可以独立使用。
