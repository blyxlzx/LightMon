@echo off
setlocal
if not exist "%~dp0build\temp" mkdir "%~dp0build\temp"
set "TEMP=%~dp0build\temp"
set "TMP=%~dp0build\temp"
set "VSROOT="
for /f "usebackq delims=" %%V in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%V"
if not defined VSROOT (
  echo Visual Studio C++ build tools are required.
  exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%~dp0src"
rc /nologo /fo "..\build\cpu-temperature-helper.res" cpu-temperature-helper.rc
if errorlevel 1 exit /b 1
cl /nologo /O1 /GL /std:c++17 /utf-8 /MT /EHsc /W4 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /Fo"..\build\\" cpu-temperature-helper.cpp "..\build\cpu-temperature-helper.res" /link /OUT:"..\..\LightMonCpu.exe" /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /MANIFEST:NO advapi32.lib
if errorlevel 1 exit /b 1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "..\generate-helper-hash.ps1"
if errorlevel 1 exit /b 1
rc /nologo /fo "..\build\app.res" app.rc
if errorlevel 1 exit /b 1
cl /nologo /O1 /GL /std:c++17 /utf-8 /MT /EHsc /W4 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /Fo"..\build\\" main.cpp metrics.cpp "..\build\app.res" /link /OUT:"..\..\LightMon.exe" /SUBSYSTEM:WINDOWS /LTCG /OPT:REF /OPT:ICF /MANIFEST:NO user32.lib gdi32.lib shell32.lib psapi.lib iphlpapi.lib advapi32.lib ole32.lib oleaut32.lib uiautomationcore.lib taskschd.lib
exit /b %errorlevel%
