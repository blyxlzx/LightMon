# LightMon

A native C++ / Win32 two-row taskbar monitor for Windows 11 x64. Hardware metrics appear on the left; network speed and daily traffic appear on the right. No Electron, WebView, telemetry, account, or automatic update downloader.

[Downloads](https://github.com/blyxlzx/LightMon/releases/latest) · [中文说明](README.md)

## Getting started

Use the Setup executable for a per-user installation, or extract the complete Portable ZIP into a writable directory and launch `LightMon.exe`. The interface is currently Chinese. Right-click the values or tray icon for options. Login startup is optional and off by default. CPU temperature setup is separate and optional.

Requires Windows 11 x64 build 22000 or later and a horizontal bottom taskbar. Windows 10, ARM64 and replacement taskbars are unsupported. Taskbar integration depends on Windows shell behavior; insufficient space temporarily hides the values. This release has no Authenticode signature; verify downloads using `SHA256SUMS.txt` without disabling OS protections.

The default configuration enables CPU, busiest logical CPU, memory, network speed and daily traffic. GPU and disk providers are optional and consume additional resources. `总↓` / `总↑` mean today's recorded download / upload totals, not lifetime totals. Traffic while the program is closed cannot be reconstructed. Configuration and traffic stay beside the executable.

CPU utilization measures busy time and is not guaranteed to match performance-normalized Task Manager CPU utility. GPU Engine counters are grouped by adapter and physical engine; different engines are not summed. The discrete-GPU binding currently requires NVIDIA NVML, while integrated GPUs are identified through DXCore. Missing counters and temperatures are shown as unavailable rather than estimated. CPU temperature currently supports AMD family `0x19`, model `0x61` only, with a separately installed PawnIO driver. See [metric details](docs/METRICS.md) and [temperature setup](docs/CPU-TEMPERATURE.md).

## Build

Install Visual Studio C++ Desktop Build Tools with a Windows SDK, then run `source\build.cmd`. It builds both executables and embeds the helper hash into the main program. Do not mix builds.

Run `scripts\Build-Release.ps1 -InnoCompilerPath <path-to-ISCC.exe>` to create the installer, portable ZIP, source ZIP and checksums in `dist/`. Inno Setup 6.5+ is required for installers; `-SkipInstaller` builds ZIPs only. `scripts\Generate-Icon.ps1` rebuilds the original icon.

The startup helper (`scripts\Configure-Startup.ps1 -Action Enable|Disable|Status`) manages a current-user non-elevated scheduled task. It refuses tasks owned by another installation. Deliberate exit pauses automatic recovery until manual launch or a new login.

## License

LightMon's own code is MIT licensed. Bundled third-party modules retain their original licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Release packages include source and notices. PawnIO drivers are neither bundled nor silently installed. Testing has not covered every Windows build or hardware combination.
