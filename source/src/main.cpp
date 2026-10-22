#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <psapi.h>
#include <ole2.h>
#include <UIAutomationClient.h>
#include <dwmapi.h>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "metrics.h"
#include "cpu_temperature_protocol.h"
#include "taskbar_gap_cache.h"
#include "process_lifetime.h"

using lightmon::MetricsSampler;
using lightmon::MetricsSample;
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_SHOW_PANEL = WM_APP + 2;
constexpr UINT WM_GAP_READY = WM_APP + 3;
constexpr UINT WM_GAP_STOPPED = WM_APP + 4;
constexpr UINT WM_DIAGNOSTICS_STOP = WM_APP + 5;
constexpr UINT WM_QUERY_TASKBAR_WINDOW = WM_APP + 6;
constexpr wchar_t CLASS_NAME[] = L"LightMon.Native.Panel.v1";
enum { ID_SHOW=100, ID_GPU, ID_DISK, ID_LITE, ID_TOP, ID_LOCK, ID_COMPACT, ID_THEME,
       ID_1SEC, ID_2SEC, ID_5SEC, ID_RESET, ID_ABOUT, ID_EXIT };
struct Config { bool gpu=false,disk=false,compact=false,smoothFont=true,gpuSplit=true,integratedGpu=true,dailyTraffic=true,cpuTemperatureAuto=false; unsigned gpuEngineMask=31; int theme=-1,interval=1000; } cfg;
constexpr wchar_t PANEL_CLASS[]=L"LightMon.Native.Taskbar.v2";
constexpr wchar_t INPUT_CLASS[]=L"LightMon.Native.Taskbar.Input.v1";
constexpr UINT ID_AUTO_THEME=200;
constexpr UINT ID_SMOOTH_FONT=201;
constexpr UINT ID_GPU_SPLIT=202;
constexpr UINT ID_INTEGRATED_GPU=203;
constexpr UINT ID_TEMPERATURE_DETAILS=204;
constexpr UINT ID_CPU_TEMPERATURE=205;
constexpr UINT ID_CPU_DETAILS=206;
constexpr UINT ID_DAILY_TRAFFIC=207;
constexpr UINT ID_DAILY_TRAFFIC_DETAILS=208;
constexpr UINT ID_CPU_TEMPERATURE_AUTO=209;
constexpr UINT ID_ENGINE_FIRST=210;
constexpr const wchar_t* ENGINE_LABELS[]={L"3D",L"计算",L"复制",L"编码",L"解码"};
constexpr const wchar_t* ENGINE_MENU_LABELS[]={L"3D",L"计算",L"复制",L"视频编码",L"视频解码"};
static_assert(std::size(ENGINE_LABELS)==lightmon::kGpuEngineCategoryCount);
MetricsSampler* sampler=nullptr;
MetricsSample sample;
HWND wnd=nullptr,panel=nullptr,inputPanel=nullptr,networkPanel=nullptr,networkInputPanel=nullptr;
HINSTANCE instance=nullptr;
HFONT valueFont=nullptr,labelFont=nullptr;
int fontCellHeight=0;
HICON trayIcon=nullptr;
UINT dpi=96,taskbarCreated=0;
bool trayAdded=false,hidden=false,testing=false,lightTheme=false,spaceAvailable=false;
bool benchmark=false,smoke=false;
bool closing=false;
bool menuOpen=false;
ULONGLONG menuGapDeadline=0;
std::uint64_t menuRequiredGeneration=0;
constexpr UINT_PTR MENU_RESUME_TIMER=2;
int elapsedTicks=0,runSeconds=30,renderWidth=0,renderHeight=0;
ULONGLONG startTick=0;
FILE* report=nullptr;
wchar_t configFile[MAX_PATH]{};
int Scale(int x) { return MulDiv(x,dpi,96); }
COLORREF PanelColorKey() { return lightTheme?RGB(210,210,211):RGB(1,0,0); }
ULONGLONG FileTimeValue(FILETIME f) { return (ULONGLONG(f.dwHighDateTime)<<32)|f.dwLowDateTime; }
namespace automaticRecovery {
constexpr wchar_t kParentKey[]=L"Software\\LightMon";
constexpr wchar_t kSessionKey[]=L"RecoverySession";
constexpr wchar_t kSessionPath[]=L"Software\\LightMon\\RecoverySession";
constexpr wchar_t kPauseValue[]=L"PausedAuthenticationId";

bool CurrentLogon(ULONGLONG& id) noexcept {
    HANDLE token=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) {
        lightmon::lifetime::LogEvent("recovery_token_open_failed",GetLastError());
        return false;
    }
    TOKEN_STATISTICS statistics{}; DWORD size=0;
    const BOOL read=GetTokenInformation(token,TokenStatistics,&statistics,sizeof(statistics),&size);
    const DWORD error=read?ERROR_SUCCESS:GetLastError();
    CloseHandle(token);
    if(!read || size<sizeof(statistics)) {
        lightmon::lifetime::LogEvent("recovery_logon_read_failed",read?ERROR_INVALID_DATA:error);
        return false;
    }
    id=(static_cast<ULONGLONG>(static_cast<DWORD>(statistics.AuthenticationId.HighPart))<<32)|
        statistics.AuthenticationId.LowPart;
    return true;
}

bool PausedForCurrentLogon() noexcept {
    ULONGLONG paused=0; DWORD size=sizeof(paused);
    const LSTATUS read=RegGetValueW(HKEY_CURRENT_USER,kSessionPath,kPauseValue,
        RRF_RT_REG_QWORD|RRF_ZEROONFAILURE,nullptr,&paused,&size);
    if(read==ERROR_FILE_NOT_FOUND || read==ERROR_PATH_NOT_FOUND) return false;
    if(read!=ERROR_SUCCESS || size!=sizeof(paused)) {
        lightmon::lifetime::LogEvent("recovery_pause_read_failed",
            read==ERROR_SUCCESS?ERROR_INVALID_DATA:static_cast<DWORD>(read));
        return false;
    }
    ULONGLONG current=0;
    const bool pausedHere=CurrentLogon(current) && current==paused;
    if(pausedHere) lightmon::lifetime::LogEvent("automatic_start_paused",0,0,current);
    return pausedHere;
}

void PauseForCurrentLogon() noexcept {
    ULONGLONG current=0;
    if(!CurrentLogon(current)) return;
    HKEY parent=nullptr,key=nullptr;
    LSTATUS status=RegCreateKeyExW(HKEY_CURRENT_USER,kParentKey,0,nullptr,
        REG_OPTION_NON_VOLATILE,KEY_CREATE_SUB_KEY,nullptr,&parent,nullptr);
    if(status==ERROR_SUCCESS) {
        // AuthenticationId is unique during this boot. A volatile child key
        // prevents an old value from affecting a reused LUID after a reboot.
        status=RegCreateKeyExW(parent,kSessionKey,0,nullptr,REG_OPTION_VOLATILE,
            KEY_SET_VALUE,nullptr,&key,nullptr);
        RegCloseKey(parent);
    }
    if(status==ERROR_SUCCESS) {
        status=RegSetValueExW(key,kPauseValue,0,REG_QWORD,
            reinterpret_cast<const BYTE*>(&current),sizeof(current));
        RegCloseKey(key);
    }
    lightmon::lifetime::LogEvent(status==ERROR_SUCCESS?"automatic_recovery_paused":
        "recovery_pause_write_failed",static_cast<DWORD>(status),0,current);
}

void ClearPause() noexcept {
    HKEY key=nullptr;
    LSTATUS status=RegOpenKeyExW(HKEY_CURRENT_USER,kSessionPath,0,KEY_SET_VALUE,&key);
    if(status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND) return;
    if(status==ERROR_SUCCESS) {
        status=RegDeleteValueW(key,kPauseValue);
        RegCloseKey(key);
    }
    if(status==ERROR_FILE_NOT_FOUND) return;
    lightmon::lifetime::LogEvent(status==ERROR_SUCCESS?"automatic_recovery_resumed":
        "recovery_pause_clear_failed",static_cast<DWORD>(status));
}
} // namespace automaticRecovery

void WriteConfig() {
    if(testing) return;
    wchar_t b[32];
    auto write=[&](const wchar_t* key,int n){ swprintf_s(b,L"%d",n); WritePrivateProfileStringW(L"LightMon",key,b,configFile); };
    write(L"GPU",cfg.gpu); write(L"Disk",cfg.disk); write(L"Compact",cfg.compact);
    write(L"SmoothFont",cfg.smoothFont);
    write(L"GpuSplit",cfg.gpuSplit); write(L"GpuEngineMask",cfg.gpuEngineMask);
    write(L"IntegratedGpu",cfg.integratedGpu);
    write(L"DailyTraffic",cfg.dailyTraffic);
    write(L"CpuTemperatureAuto",cfg.cpuTemperatureAuto);
    write(L"Theme",cfg.theme); write(L"Interval",cfg.interval); write(L"TaskbarOnly",1);
    const wchar_t* oldKeys[]={L"Topmost",L"Locked",L"LightTheme",L"X",L"Y"};
    for(auto key:oldKeys) WritePrivateProfileStringW(L"LightMon",key,nullptr,configFile);
}
void ReadConfig() {
    GetModuleFileNameW(nullptr,configFile,MAX_PATH);
    wchar_t* p=wcsrchr(configFile,L'\\'); if(p) wcscpy_s(p+1,MAX_PATH-(p+1-configFile),L"LightMon.ini");
    auto read=[&](const wchar_t* k,int n){ return (int)GetPrivateProfileIntW(L"LightMon",k,n,configFile); };
    cfg.gpu=read(L"GPU",0)!=0; cfg.disk=read(L"Disk",0)!=0;
    cfg.compact=false; // Preserve the old INI key, but always use two rows.
    cfg.smoothFont=read(L"SmoothFont",1)!=0;
    cfg.gpuSplit=read(L"GpuSplit",1)!=0; cfg.gpuEngineMask=static_cast<unsigned>(read(L"GpuEngineMask",31))&31u;
    cfg.integratedGpu=read(L"IntegratedGpu",1)!=0;
    cfg.dailyTraffic=read(L"DailyTraffic",1)!=0;
    cfg.cpuTemperatureAuto=read(L"CpuTemperatureAuto",0)!=0;
    cfg.theme=std::clamp(read(L"Theme",-1),-1,1);
    cfg.interval=read(L"Interval",1000); if(cfg.interval!=1000&&cfg.interval!=2000&&cfg.interval!=5000) cfg.interval=1000;
}
void UpdateTheme() {
    DWORD light=0,size=sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme",RRF_RT_REG_DWORD,nullptr,&light,&size);
    lightTheme=cfg.theme<0?light!=0:cfg.theme!=0;
    if(panel&&IsWindow(panel)) SetLayeredWindowAttributes(panel,PanelColorKey(),0,LWA_COLORKEY);
    if(networkPanel&&IsWindow(networkPanel)) SetLayeredWindowAttributes(networkPanel,PanelColorKey(),0,LWA_COLORKEY);
}
void Fonts() {
    if(valueFont) DeleteObject(valueFont);
    if(labelFont) DeleteObject(labelFont);
    // Larger native text; retain regular YaHei and system-selected smoothing.
    // Measure the new font for both rows and taskbar placement, without resizing.
    const int height=-MulDiv(10,dpi,72);
    const DWORD quality=cfg.smoothFont?DEFAULT_QUALITY:NONANTIALIASED_QUALITY;
    valueFont=CreateFontW(height,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,quality,DEFAULT_PITCH|FF_SWISS,L"微软雅黑");
    labelFont=CreateFontW(height,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,quality,DEFAULT_PITCH|FF_SWISS,L"微软雅黑");
    HDC dc=GetDC(nullptr); HGDIOBJ old=SelectObject(dc,valueFont); TEXTMETRICW metrics{};
    GetTextMetricsW(dc,&metrics); fontCellHeight=metrics.tmHeight;
    SelectObject(dc,old); ReleaseDC(nullptr,dc);
}
// Widths are measured in physical pixels with the same native font as Paint.
// Budget stable maximum tokens, so changing readings never shifts the columns.
int TextWidth(const wchar_t* text) {
    HDC dc=GetDC(nullptr); HGDIOBJ old=SelectObject(dc,valueFont); SIZE size{};
    GetTextExtentPoint32W(dc,text,static_cast<int>(wcslen(text)),&size);
    SelectObject(dc,old); ReleaseDC(nullptr,dc); return size.cx;
}
int CellWidth(const wchar_t* label,const wchar_t* budget) {
    return TextWidth(label)+Scale(4)+TextWidth(budget);
}
// Leave equal breathing room around the one-physical-pixel separator.
// Use the same gap for measurement, placement, and the two-row painting path.
int ColumnGap() { return 2*Scale(4)+1; }
int CpuWidth() { return std::max(CellWidth(L"CPU",L"100%"),CellWidth(L"忙核",L"100%")); }
int NetworkWidth() {
    int width=0;
    for(const wchar_t* token:{L"↑99.9MB/s",L"↑1023MB/s",L"↓1023KB/s",L"↑1023GB/s",L"↓1023B/s"})
        width=std::max(width,TextWidth(token));
    return width;
}
int DailyTrafficWidth() {
    int width=0;
    for(const wchar_t* unit:{L"B",L"KB",L"MB",L"GB",L"TB",L"PB",L"EB"}) {
        wchar_t token[32];
        for(const wchar_t* number:{L"99.9",L"1024"}) {
            swprintf_s(token,L"总↓%s%s",number,unit); width=std::max(width,TextWidth(token));
            swprintf_s(token,L"总↑%s%s",number,unit); width=std::max(width,TextWidth(token));
        }
    }
    return width;
}
struct GpuCell { const wchar_t* label=L""; wchar_t value[32]{}; int width=0; };
void FormatRate(double rate,wchar_t* out,size_t count);
std::size_t GpuCells(std::array<GpuCell,12>& cells) {
    std::size_t count=0;
    auto percent=[&](const wchar_t* label,bool available,double value) {
        auto& cell=cells[count++]; cell.label=label; cell.width=CellWidth(label,L"100%");
        if(available) swprintf_s(cell.value,L"%.0f%%",value); else wcscpy_s(cell.value,L"—");
    };
    if(cfg.gpu) {
        if(cfg.gpuSplit) {
            for(std::size_t i=0;i<lightmon::kGpuEngineCategoryCount;++i) {
                if(!(cfg.gpuEngineMask&(1u<<i))) continue;
                const auto& engine=sample.gpuEngines[i];
                percent(ENGINE_LABELS[i],engine.available,engine.percent);
                cells[count-1].width=std::max(cells[count-1].width,CellWidth(ENGINE_LABELS[i],L"暂无"));
                if(!engine.available&&engine.supportKnown&&!engine.supported) {
                    auto& cell=cells[count-1]; wcscpy_s(cell.value,L"暂无");
                }
            }
        } else percent(L"GPU",sample.gpuUtilizationAvailable,sample.gpuPercent);
        // Pair adjacent metrics vertically. The busiest logical processor is
        // below CPU; the final pair here is disk / memory in the usual layout.
        if(cfg.integratedGpu) percent(L"核显",sample.integratedGpu.utilizationAvailable,sample.integratedGpu.percent);
        auto& vram=cells[count++]; vram.label=L"显存";
        const wchar_t* budget=sample.gpuTotalBytes>=100ull*1073741824?L"999.9G":L"99.9G";
        vram.width=CellWidth(vram.label,budget);
        if(sample.gpuMemoryAvailable) swprintf_s(vram.value,L"%.1fG",sample.gpuUsedBytes/1073741824.0);
        else wcscpy_s(vram.value,L"—");
    }
    if(cfg.disk) percent(L"磁盘",sample.diskAvailable,sample.diskBusyPercent);
    percent(L"内存",sample.ramAvailable,sample.ramPercent);
    return count;
}
std::size_t TemperatureCells(std::array<GpuCell,12>& cells) {
    if(!cfg.gpu&&!cfg.disk) return 0;
    std::size_t count=0;
    auto add=[&](const wchar_t* label,bool available,int value) {
        auto& cell=cells[count++]; cell.label=label; cell.width=CellWidth(label,L"100°");
        if(available) swprintf_s(cell.value,L"%d°",value); else wcscpy_s(cell.value,L"—");
    };
    add(L"CPU温",sample.cpuTemperatureAvailable,sample.cpuTemperatureC);
    if(!sample.cpuTemperatureAvailable&&
       (sample.cpuTemperatureStatus==lightmon::CpuTemperatureStatus::NeedsPermission||
        sample.cpuTemperatureStatus==lightmon::CpuTemperatureStatus::AccessDenied||
        sample.cpuTemperatureStatus==lightmon::CpuTemperatureStatus::LaunchCancelled))
        wcscpy_s(cells[count-1].value,L"权限");
    if(cfg.gpu&&cfg.integratedGpu) add(L"核显温",sample.integratedGpuTemperatureAvailable,sample.integratedGpuTemperatureC);
    if(cfg.gpu) add(L"独显温",sample.gpuTemperatureAvailable,static_cast<int>(sample.gpuTemperatureC));
    if(cfg.disk) add(L"盘温",sample.diskTemperatureAvailable,sample.diskTemperatureC);
    return count;
}
int CellsWidth(const std::array<GpuCell,12>& cells,std::size_t count) {
    int width=0;
    for(std::size_t i=0;i<count;i+=2)
        width+=i+1<count?std::max(cells[i].width,cells[i+1].width):cells[i].width;
    return width;
}
int PanelWidth(int group=0) {
    std::array<GpuCell,12> metrics{},temperatures{};
    const auto metricCount=GpuCells(metrics),temperatureCount=TemperatureCells(temperatures);
    // All load metrics and temperatures stay left; network rates and today's
    // traffic stay right. Both groups retain the native two-row 10 pt layout.
    const int metricColumns=1+static_cast<int>((metricCount+1)/2)+static_cast<int>((temperatureCount+1)/2);
    const int networkColumns=1+(cfg.dailyTraffic?1:0);
    const int columns=(group!=2?metricColumns:0)+(group!=1?networkColumns:0);
    if(!columns) return 0;
    return 2*Scale(1)+(group!=2?CpuWidth()+CellsWidth(metrics,metricCount)+
        CellsWidth(temperatures,temperatureCount):0)+(group!=1?NetworkWidth()+
        (cfg.dailyTraffic?DailyTrafficWidth():0):0)+(columns-1)*ColumnGap();
}
HWND FindDescendant(HWND parent,const wchar_t* className) {
    // Prefer native direct children before the compatibility enumeration path.
    if(HWND direct=FindWindowExW(parent,nullptr,className,nullptr)) return direct;
    // Follow the bounded primary/secondary chains observed on this host.
    // Other Shell layouts still use the enumeration fallback below.
    if(!wcscmp(className,L"MSTaskListWClass")||!wcscmp(className,L"MSTaskSwWClass")) {
        HWND rebar=FindWindowExW(parent,nullptr,L"ReBarWindow32",nullptr);
        HWND tasks=rebar?FindWindowExW(rebar,nullptr,L"MSTaskSwWClass",nullptr):nullptr;
        if(tasks) {
            if(!wcscmp(className,L"MSTaskSwWClass")) return tasks;
            if(HWND list=FindWindowExW(tasks,nullptr,L"MSTaskListWClass",nullptr)) return list;
        }
        HWND worker=FindWindowExW(parent,nullptr,L"WorkerW",nullptr);
        if(worker&&!wcscmp(className,L"MSTaskListWClass"))
            if(HWND list=FindWindowExW(worker,nullptr,L"MSTaskListWClass",nullptr)) return list;
    }
    struct Search { const wchar_t* name; HWND result; } search{className,nullptr};
    EnumChildWindows(parent,[](HWND h,LPARAM l)->BOOL {
        auto* q=reinterpret_cast<Search*>(l); wchar_t name[128]; GetClassNameW(h,name,128);
        if(!wcscmp(name,q->name)) { q->result=h; return FALSE; } return TRUE;
    },reinterpret_cast<LPARAM>(&search));
    return search.result;
}
LRESULT CALLBACK PanelProc(HWND,UINT,WPARAM,LPARAM);
LRESULT CALLBACK InputProc(HWND,UINT,WPARAM,LPARAM);
struct GapRequest {
    HWND bar=nullptr; RECT barRect{},notifyRect{}; UINT dpi=96;
    int width=0,anchor=0,height=0,leftWidth=0,rightWidth=0;
};
struct GapBatch { std::array<GapRequest,8> items{}; std::size_t count=0; };
enum class NativeMenuState { Unknown=-1, Closed=0, Open=1 };
bool TaskbarPopupOwnerMatches(HWND owner,HWND bar,DWORD explorer,DWORD thread) {
    // Follow actual child parents only; an unrelated owned popup is not a taskbar child.
    for(unsigned depth=0;owner&&depth<8;++depth) {
        if(!IsWindow(owner)) return false;
        DWORD ownerProcess=0;
        if(GetWindowThreadProcessId(owner,&ownerProcess)!=thread||ownerProcess!=explorer) return false;
        if(owner==bar) return true;
        if(!(GetWindowLongPtrW(owner,GWL_STYLE)&WS_CHILD)) return false;
        owner=GetParent(owner);
    }
    return false;
}
NativeMenuState TaskbarPopupVisibility(HWND popup) {
    using DwmAttributeFn=HRESULT (WINAPI*)(HWND,DWORD,PVOID,DWORD);
    static const auto attribute=[]()->DwmAttributeFn {
        // Retain the system module for the process lifetime; no new linker dependency.
        const auto module=LoadLibraryExW(L"dwmapi.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module?reinterpret_cast<DwmAttributeFn>(GetProcAddress(module,"DwmGetWindowAttribute")):nullptr;
    }();
    DWORD cloaked=0;
    if(!attribute||FAILED(attribute(popup,DWMWA_CLOAKED,&cloaked,sizeof(cloaked))))
        return NativeMenuState::Unknown;
    return !cloaked&&IsWindow(popup)&&IsWindowVisible(popup)?NativeMenuState::Open:NativeMenuState::Closed;
}
NativeMenuState XamlTaskbarMenu(HWND bar,DWORD explorer,DWORD thread) {
    struct Search {
        HWND bar; DWORD explorer,thread; unsigned count=0;
        bool open=false,unknown=false,capped=false;
    } search{bar,explorer,thread};
    const BOOL complete=EnumThreadWindows(thread,[](HWND popup,LPARAM data)->BOOL {
        auto& s=*reinterpret_cast<Search*>(data);
        ++s.count;
        DWORD process=0; wchar_t name[64]{};
        if(IsWindow(popup)&&IsWindowVisible(popup)&&
           GetWindowThreadProcessId(popup,&process)==s.thread&&process==s.explorer&&
           GetClassNameW(popup,name,static_cast<int>(std::size(name)))&&
           wcscmp(name,L"Xaml_WindowedPopupClass")==0&&
           (GetWindowLongPtrW(popup,GWL_STYLE)&WS_POPUP)&&
           TaskbarPopupOwnerMatches(GetWindow(popup,GW_OWNER),s.bar,s.explorer,s.thread)) {
            const auto state=TaskbarPopupVisibility(popup);
            // Ownership and visibility must still hold after the DWM query.
            if(state==NativeMenuState::Open&&
               TaskbarPopupOwnerMatches(GetWindow(popup,GW_OWNER),s.bar,s.explorer,s.thread)) {
                s.open=true; return FALSE;
            }
            s.unknown|=state==NativeMenuState::Unknown;
        }
        if(s.count>=128) { s.capped=true; return FALSE; }
        return TRUE;
    },reinterpret_cast<LPARAM>(&search));
    if(search.open) return NativeMenuState::Open;
    if(!complete||search.capped||search.unknown) return NativeMenuState::Unknown;
    return NativeMenuState::Closed;
}
NativeMenuState ClassicTaskbarMenu(HWND bar) {
    if(!bar||!IsWindow(bar)) return NativeMenuState::Unknown;
    DWORD explorer=0; const DWORD thread=GetWindowThreadProcessId(bar,&explorer);
    if(!thread||!explorer) return NativeMenuState::Unknown;
    GUITHREADINFO info{sizeof(info)};
    NativeMenuState classic=NativeMenuState::Unknown;
    constexpr DWORD menuFlags=GUI_INMENUMODE|GUI_POPUPMENUMODE|GUI_SYSTEMMENUMODE;
    if(GetGUIThreadInfo(thread,&info)) {
        classic=NativeMenuState::Closed;
        if(info.flags&menuFlags) {
            if(!info.hwndMenuOwner||!IsWindow(info.hwndMenuOwner)) classic=NativeMenuState::Unknown;
            else {
                DWORD owner=0; GetWindowThreadProcessId(info.hwndMenuOwner,&owner);
                if(owner==explorer&&(info.hwndMenuOwner==bar||IsChild(bar,info.hwndMenuOwner)))
                    classic=NativeMenuState::Open;
            }
        }
    }
    if(classic==NativeMenuState::Open) return classic;
    // This machine's real taskbar menu is an owned XAML popup with GUI flags==0.
    const auto xaml=XamlTaskbarMenu(bar,explorer,thread);
    if(xaml==NativeMenuState::Open) return xaml;
    return classic==NativeMenuState::Unknown||xaml==NativeMenuState::Unknown?
        NativeMenuState::Unknown:NativeMenuState::Closed;
}
bool BatchHasClassicTaskbarMenu(const GapBatch& batch) {
    for(std::size_t i=0;i<batch.count;++i)
        if(ClassicTaskbarMenu(batch.items[i].bar)==NativeMenuState::Open) return true;
    return false;
}
struct Placement {
    GapRequest request{}; DWORD explorer=0;
    int x=0,y=0,networkX=0; bool split=false,valid=false;
} lastPlacement;
bool systemMenuObserved=false,menuHoldValid=false;
ULONGLONG systemMenuUnknownDeadline=0;
NativeMenuState nativeMenuState=NativeMenuState::Closed;
std::mutex gapMutex;
std::condition_variable gapWake;
std::thread gapThread;
GapBatch gapRequest;
std::array<GapRequest,8> gapRejectedRequests{}; // Protected by gapMutex.
lightmon::TaskbarGapCache<GapBatch> gapCache;
std::size_t gapSelected=0;
bool gapSplit=false;
int gapNetworkX=0;
bool gapPending=false,gapStop=false;
ULONGLONG gapRequestTick=0;
std::uint64_t gapRequestGeneration=0;
bool SameGap(const GapRequest& a,const GapRequest& b) {
    return a.bar==b.bar&&EqualRect(&a.barRect,&b.barRect)&&EqualRect(&a.notifyRect,&b.notifyRect)&&
        a.dpi==b.dpi&&a.width==b.width&&a.anchor==b.anchor&&a.height==b.height&&
        a.leftWidth==b.leftWidth&&a.rightWidth==b.rightWidth;
}
bool SameBatch(const GapBatch& a,const GapBatch& b) {
    if(a.count!=b.count) return false;
    for(std::size_t i=0;i<a.count;++i) if(!SameGap(a.items[i],b.items[i])) return false;
    return true;
}
lightmon::TaskbarGapState QueryTaskbarGap(const GapRequest& request,int& x,int& networkX,bool& split) {
    // Windows 11's legacy task-list bounds omit some XAML buttons. Query their
    // actual bounds on an MTA worker; querying our own child on the GUI thread
    // can deadlock its accessibility provider. Never wait for UIA in a window proc.
    HWND bar=request.bar; const RECT& barRect=request.barRect;
    const RECT& notifyRect=request.notifyRect;
    auto scale=[&](int n){ return MulDiv(n,request.dpi,96); };
    IUIAutomation2* automation=nullptr; IUIAutomationElement* root=nullptr;
    IUIAutomationCondition* condition=nullptr; IUIAutomationElementArray* buttons=nullptr;
    IUIAutomationCacheRequest* cache=nullptr;
    int leftBegin=barRect.left+scale(8),leftEnd=request.anchor-scale(8);
    int rightBegin=request.anchor+scale(8),rightEnd=notifyRect.left-scale(8),count=0;
    bool valid=false; int visibleButtons=0; split=false;
    HRESULT hr=CoCreateInstance(CLSID_CUIAutomation8,nullptr,CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&automation));
    if(SUCCEEDED(hr)) hr=automation->put_ConnectionTimeout(200);
    if(SUCCEEDED(hr)) hr=automation->put_TransactionTimeout(500);
    if(SUCCEEDED(hr)) hr=automation->ElementFromHandle(bar,&root);
    VARIANT type{}; type.vt=VT_I4; type.lVal=UIA_ButtonControlTypeId;
    if(SUCCEEDED(hr)) hr=automation->CreatePropertyCondition(UIA_ControlTypePropertyId,type,&condition);
    if(SUCCEEDED(hr)) hr=automation->CreateCacheRequest(&cache);
    if(SUCCEEDED(hr)) hr=cache->AddProperty(UIA_BoundingRectanglePropertyId);
    if(SUCCEEDED(hr)) hr=cache->AddProperty(UIA_IsOffscreenPropertyId);
    if(SUCCEEDED(hr)) hr=cache->put_TreeScope(TreeScope_Element);
    if(SUCCEEDED(hr)) hr=cache->put_AutomationElementMode(AutomationElementMode_None);
    if(SUCCEEDED(hr)) hr=root->FindAllBuildCache(TreeScope_Descendants,condition,cache,&buttons);
    if(SUCCEEDED(hr)&&buttons) hr=buttons->get_Length(&count);
    else if(SUCCEEDED(hr)) hr=E_FAIL;
    if(SUCCEEDED(hr)&&count>0&&count<=256) {
        valid=true;
        for(int i=0;i<count;++i) {
            IUIAutomationElement* button=nullptr; RECT rect{}; BOOL offscreen=TRUE;
            if(FAILED(buttons->GetElement(i,&button))||!button) { valid=false; break; }
            HRESULT bounds=button->get_CachedBoundingRectangle(&rect);
            HRESULT visible=button->get_CachedIsOffscreen(&offscreen); button->Release();
            if(FAILED(bounds)||FAILED(visible)) { valid=false; break; }
            if(!offscreen&&rect.right>rect.left&&rect.bottom>barRect.top&&rect.top<barRect.bottom&&
                rect.right>barRect.left&&rect.left<notifyRect.left) {
                ++visibleButtons;
                if(rect.right<=request.anchor) leftBegin=std::max<int>(leftBegin,rect.right+scale(8));
                else {
                    rightBegin=std::max<int>(rightBegin,rect.right+scale(8));
                    if(rect.left<request.anchor) leftEnd=std::min<int>(leftEnd,rect.left-scale(8));
                }
            }
        }
    }
    if(buttons) buttons->Release(); if(condition) condition->Release();
    if(cache) cache->Release();
    if(root) root->Release(); if(automation) automation->Release();
    // Keep other programs' taskbar windows outside this candidate gap as well.
    struct Limits { int leftBegin,leftEnd,rightBegin,rightEnd; DWORD explorer; UINT dpi; }
        gap{leftBegin,leftEnd,rightBegin,rightEnd,0,request.dpi};
    GetWindowThreadProcessId(bar,&gap.explorer);
    EnumChildWindows(bar,[](HWND h,LPARAM l)->BOOL {
        auto* g=reinterpret_cast<Limits*>(l); DWORD owner=0; RECT r{};
        GetWindowThreadProcessId(h,&owner);
        if(owner!=g->explorer&&owner!=GetCurrentProcessId()&&IsWindowVisible(h)&&GetWindowRect(h,&r)&&r.right>r.left) {
            if(r.right>g->leftBegin&&r.left<g->leftEnd)
                g->leftEnd=std::min<int>(g->leftEnd,r.left-MulDiv(6,g->dpi,96));
            if(r.right>g->rightBegin&&r.left<g->rightEnd)
                g->rightEnd=std::min<int>(g->rightEnd,r.left-MulDiv(6,g->dpi,96));
        }
        return TRUE;
    },reinterpret_cast<LPARAM>(&gap));
    if(!valid||!visibleButtons) return lightmon::TaskbarGapState::Unknown;
    // Preserve the requested sides even when a complete panel could fit in
    // one gap: metrics and temperatures left, all network rows right.
    if(request.leftWidth>0&&request.rightWidth>0&&gap.leftEnd-gap.leftBegin>=request.leftWidth&&
       gap.rightEnd-gap.rightBegin>=request.rightWidth) {
        x=gap.leftBegin; networkX=gap.rightEnd-request.rightWidth; split=true;
        return lightmon::TaskbarGapState::Available;
    }
    return lightmon::TaskbarGapState::NoSpace;
}
void GapWorker() {
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    for(;;) {
        GapBatch request;
        std::uint64_t generation=0;
        {
            std::unique_lock<std::mutex> lock(gapMutex);
            gapWake.wait(lock,[]{ return gapStop||gapPending; });
            if(gapStop) break;
            request=gapRequest; generation=gapRequestGeneration; gapPending=false;
        }
        int x=0,networkX=0; bool split=false; std::size_t selected=0;
        std::array<lightmon::TaskbarGapState,8> results{};
        auto state=lightmon::TaskbarGapState::NoSpace;
        bool menuBlocked=BatchHasClassicTaskbarMenu(request);
        if(FAILED(initialized)||menuBlocked) state=lightmon::TaskbarGapState::Unknown;
        else for(std::size_t i=0;i<request.count;++i) {
            const auto result=QueryTaskbarGap(request.items[i],x,networkX,split); results[i]=result;
            if(result==lightmon::TaskbarGapState::Available) { state=result; selected=i; break; }
            if(result==lightmon::TaskbarGapState::Unknown) state=result;
        }
        menuBlocked=menuBlocked||BatchHasClassicTaskbarMenu(request);
        if(menuBlocked) state=lightmon::TaskbarGapState::Unknown;
        bool committed=false;
        {
            std::lock_guard<std::mutex> lock(gapMutex);
            if(gapStop) break;
            if(generation==gapRequestGeneration&&SameBatch(gapRequest,request)) {
                if(!menuBlocked) for(std::size_t i=0;i<request.count;++i) {
                    if(results[i]==lightmon::TaskbarGapState::Unknown) continue;
                    GapRequest* rejected=nullptr;
                    for(auto& known:gapRejectedRequests) {
                        if(known.bar==request.items[i].bar) { rejected=&known; break; }
                        if(!known.bar&&!rejected) rejected=&known;
                    }
                    if(results[i]==lightmon::TaskbarGapState::NoSpace) {
                        if(!rejected) rejected=&gapRejectedRequests[0];
                        *rejected=request.items[i];
                    } else if(rejected&&rejected->bar==request.items[i].bar) *rejected=GapRequest{};
                }
                // A different monitor's failed query cannot conceal a proven
                // NoSpace result for the previously displayed monitor.
                int previousX=0;
                if(!menuBlocked&&state==lightmon::TaskbarGapState::Unknown&&gapSelected<request.count&&
                   gapCache.Resolve(request,GetTickCount64(),previousX,SameBatch)&&
                   results[gapSelected]==lightmon::TaskbarGapState::NoSpace) state=lightmon::TaskbarGapState::NoSpace;
                gapCache.Commit(request,state,x,GetTickCount64(),SameBatch);
                if(state==lightmon::TaskbarGapState::Available) { gapSelected=selected; gapSplit=split; gapNetworkX=networkX; }
                committed=true;
            }
        }
        if(committed) PostMessageW(wnd,WM_GAP_READY,static_cast<WPARAM>(generation),0);
    }
    if(SUCCEEDED(initialized)) CoUninitialize();
    PostMessageW(wnd,WM_GAP_STOPPED,0,0);
}
bool FindTaskbarGap(const GapBatch& request,GapRequest& selected,int& x,int& networkX,bool& split) {
    const ULONGLONG now=GetTickCount64();
    if(!gapThread.joinable()) gapThread=std::thread(GapWorker);
    std::lock_guard<std::mutex> lock(gapMutex);
    const bool changed=!SameBatch(gapRequest,request);
    if(changed||now-gapRequestTick>=1000) {
        // A periodic refresh of the same rectangle may overlap a slow UIA
        // query. Only geometry changes and explicit post-menu requests retire
        // earlier results; otherwise successful queries could be starved.
        if(changed) ++gapRequestGeneration;
        gapRequest=request; gapRequestTick=now; gapPending=true; gapWake.notify_one();
    }
    if(!gapCache.Resolve(request,now,x,SameBatch)||gapSelected>=request.count) return false;
    selected=request.items[gapSelected];
    split=gapSplit; networkX=gapNetworkX;
    // Reusing a successful UIA location must never cover another app's newly
    // visible taskbar child. Geometry is already part of the cache key.
    struct Check { RECT area,network; DWORD explorer; bool split=false,clear=true; }
        check{{x,selected.barRect.top,x+(split?selected.leftWidth:selected.width),selected.barRect.bottom},
        {networkX,selected.barRect.top,networkX+selected.rightWidth,selected.barRect.bottom},0,split,true};
    GetWindowThreadProcessId(selected.bar,&check.explorer);
    EnumChildWindows(selected.bar,[](HWND h,LPARAM l)->BOOL {
        auto* c=reinterpret_cast<Check*>(l); DWORD owner=0; RECT r{},overlap{};
        GetWindowThreadProcessId(h,&owner);
        if(owner!=c->explorer&&owner!=GetCurrentProcessId()&&IsWindowVisible(h)&&
            GetWindowRect(h,&r)&&(IntersectRect(&overlap,&r,&c->area)||
            (c->split&&IntersectRect(&overlap,&r,&c->network)))) { c->clear=false; return FALSE; }
        return TRUE;
    },reinterpret_cast<LPARAM>(&check));
    return check.clear;
}
void PanelDimensionsForDpi(UINT target,int& width,int& height,int& leftWidth,int& rightWidth) {
    // Cache the few monitor DPI layouts; keep painting fonts intact while
    // measuring another monitor. No per-sample font creation is needed.
    struct Layout { UINT dpi=0; std::uint64_t key=0; int width=0,height=0,leftWidth=0,rightWidth=0; };
    static std::array<Layout,8> layouts{};
    const std::uint64_t key=std::uint64_t(cfg.gpu)|(std::uint64_t(cfg.disk)<<1)|
        (std::uint64_t(cfg.smoothFont)<<2)|(std::uint64_t(cfg.gpuSplit)<<3)|
        (std::uint64_t(cfg.integratedGpu)<<4)|(std::uint64_t(cfg.dailyTraffic)<<5)|
        (std::uint64_t(cfg.gpuEngineMask)<<6)|(std::uint64_t(sample.gpuTotalBytes>=100ull*1073741824)<<11);
    Layout* slot=nullptr;
    for(auto& layout:layouts) {
        if(layout.dpi==target&&layout.key==key) {
            width=layout.width; height=layout.height; leftWidth=layout.leftWidth; rightWidth=layout.rightWidth; return;
        }
        if(layout.dpi==target||(!slot&&!layout.dpi)) slot=&layout;
    }
    if(!slot) slot=&layouts[0];
    const UINT savedDpi=dpi; const int savedHeight=fontCellHeight;
    const HFONT savedValue=valueFont,savedLabel=labelFont;
    dpi=target; valueFont=nullptr; labelFont=nullptr; Fonts();
    width=PanelWidth(); height=std::max(Scale(32),fontCellHeight*2);
    leftWidth=PanelWidth(1); rightWidth=PanelWidth(2);
    if(valueFont) DeleteObject(valueFont); if(labelFont) DeleteObject(labelFont);
    dpi=savedDpi; fontCellHeight=savedHeight; valueFont=savedValue; labelFont=savedLabel;
    *slot=Layout{target,key,width,height,leftWidth,rightWidth};
}
bool TaskbarRequest(HWND bar,bool primary,GapRequest& result) {
    if(!bar||!IsWindow(bar)||!IsWindowVisible(bar)) return false;
    RECT barRect{},client{},taskRect{},notifyRect{};
    if(!GetWindowRect(bar,&barRect)||!GetClientRect(bar,&client)) return false;
    MONITORINFO mi{sizeof(mi)};
    if(!GetMonitorInfoW(MonitorFromWindow(bar,MONITOR_DEFAULTTONULL),&mi)||
       barRect.right-barRect.left<barRect.bottom-barRect.top||
       barRect.top<mi.rcMonitor.top+(mi.rcMonitor.bottom-mi.rcMonitor.top)/2) return false;
    HWND tasks=FindDescendant(bar,L"MSTaskListWClass");
    if(!tasks) tasks=FindDescendant(bar,L"MSTaskSwWClass");
    if(!tasks||!GetWindowRect(tasks,&taskRect)) return false;
    HWND notify=FindDescendant(bar,L"TrayNotifyWnd");
    if(notify) { if(!GetWindowRect(notify,&notifyRect)) return false; }
    else {
        if(primary) return false;
        // Secondary taskbars expose the clock through XAML, without a native
        // TrayNotifyWnd. UIA still excludes its real button bounds below.
        notifyRect=RECT{barRect.right,barRect.top,barRect.right,barRect.bottom};
    }
    HWND start=FindDescendant(bar,L"Start"); RECT startRect{};
    int anchor=taskRect.left;
    if(start&&GetWindowRect(start,&startRect)&&startRect.left>barRect.left) anchor=startRect.left;
    UINT targetDpi=GetDpiForWindow(bar); if(!targetDpi) targetDpi=96;
    int width=0,height=0,leftWidth=0,rightWidth=0; PanelDimensionsForDpi(targetDpi,width,height,leftWidth,rightWidth);
    if(height<=0||height>client.bottom) return false;
    result=GapRequest{bar,barRect,notifyRect,targetDpi,width,anchor,height,leftWidth,rightWidth}; return true;
}
GapBatch CurrentTaskbarRequests() {
    GapBatch batch; GapRequest candidate;
    HWND primary=FindWindowW(L"Shell_TrayWnd",nullptr);
    if(!primary) return batch;
    if(TaskbarRequest(primary,true,candidate)) batch.items[batch.count++]=candidate;
    DWORD explorer=0; GetWindowThreadProcessId(primary,&explorer);
    for(HWND secondary=nullptr;batch.count<batch.items.size();) {
        secondary=FindWindowExW(nullptr,secondary,L"Shell_SecondaryTrayWnd",nullptr);
        if(!secondary) break;
        DWORD owner=0; GetWindowThreadProcessId(secondary,&owner);
        if(owner==explorer&&TaskbarRequest(secondary,false,candidate)) batch.items[batch.count++]=candidate;
    }
    return batch;
}
void CancelMenuGrace() {
    menuGapDeadline=0; menuRequiredGeneration=0; KillTimer(wnd,MENU_RESUME_TIMER);
}
void RequestPostMenuRefresh() {
    if(closing||hidden) return;
    const ULONGLONG now=GetTickCount64();
    const GapBatch fresh=CurrentTaskbarRequests();
    if(!gapThread.joinable()) gapThread=std::thread(GapWorker);
    menuGapDeadline=now+1000;
    {
        std::lock_guard<std::mutex> lock(gapMutex);
        if(fresh.count) gapRequest=fresh;
        gapRequestTick=now; menuRequiredGeneration=++gapRequestGeneration;
        gapPending=true; gapWake.notify_one();
    }
    SetTimer(wnd,MENU_RESUME_TIMER,1000,nullptr);
}
bool ExactGroup(HWND visual,HWND input,HWND bar,const RECT& expected,int group) {
    for(HWND h:{visual,input}) {
        DWORD owner=0; RECT rect{}; wchar_t name[128]{};
        if(!h||!IsWindow(h)||!IsWindowVisible(h)||GetParent(h)!=bar||
           !GetWindowThreadProcessId(h,&owner)||owner!=GetCurrentProcessId()||
           !GetClassNameW(h,name,128)||wcscmp(name,h==visual?PANEL_CLASS:INPUT_CLASS)||
           !GetWindowRect(h,&rect)||!EqualRect(&rect,&expected)) return false;
    }
    return reinterpret_cast<INT_PTR>(GetPropW(visual,L"LightMon.Group"))==group;
}
bool PlacementHasNoForeignChild(HWND bar,const RECT& area,const RECT& network,bool split) {
    struct Check { RECT area,network; DWORD explorer; bool split,clear=true; } check{area,network,0,split};
    GetWindowThreadProcessId(bar,&check.explorer);
    EnumChildWindows(bar,[](HWND h,LPARAM l)->BOOL {
        auto* c=reinterpret_cast<Check*>(l); DWORD owner=0; RECT rect{},overlap{};
        GetWindowThreadProcessId(h,&owner);
        if(owner!=c->explorer&&owner!=GetCurrentProcessId()&&IsWindowVisible(h)&&
           GetWindowRect(h,&rect)&&(IntersectRect(&overlap,&rect,&c->area)||
           (c->split&&IntersectRect(&overlap,&rect,&c->network)))) { c->clear=false; return FALSE; }
        return TRUE;
    },reinterpret_cast<LPARAM>(&check));
    return check.clear;
}
bool ValidHeldPlacement() {
    if(hidden||closing||!lastPlacement.valid) return false;
    const auto& saved=lastPlacement.request; DWORD explorer=0; wchar_t name[128]{};
    if(!IsWindow(saved.bar)||!GetWindowThreadProcessId(saved.bar,&explorer)||
       explorer!=lastPlacement.explorer||!GetClassNameW(saved.bar,name,128)) return false;
    const bool primary=!wcscmp(name,L"Shell_TrayWnd");
    if(!primary&&wcscmp(name,L"Shell_SecondaryTrayWnd")) return false;
    GapRequest fresh;
    if(!TaskbarRequest(saved.bar,primary,fresh)||!SameGap(saved,fresh)) return false;
    {
        std::lock_guard<std::mutex> lock(gapMutex);
        for(const auto& rejected:gapRejectedRequests) if(SameGap(saved,rejected)) return false;
    }
    const auto scale=[&](int n){ return MulDiv(n,saved.dpi,96); };
    RECT area{lastPlacement.x,lastPlacement.y,
        lastPlacement.x+(lastPlacement.split?saved.leftWidth:saved.width),lastPlacement.y+saved.height};
    RECT network{lastPlacement.networkX,lastPlacement.y,
        lastPlacement.networkX+saved.rightWidth,lastPlacement.y+saved.height};
    const auto inside=[&](const RECT& r) {
        return r.left>=saved.barRect.left&&r.top>=saved.barRect.top&&
            r.right<=saved.barRect.right&&r.bottom<=saved.barRect.bottom&&r.right>r.left&&r.bottom>r.top;
    };
    const auto leftGap=[&](const RECT& r) {
        return r.left>=saved.barRect.left+scale(8)&&r.right<=saved.anchor-scale(8);
    };
    const auto rightGap=[&](const RECT& r) {
        return r.left>=saved.anchor+scale(8)&&r.right<=saved.notifyRect.left-scale(8);
    };
    if(!inside(area)||!ExactGroup(panel,inputPanel,saved.bar,area,lastPlacement.split?1:0)) return false;
    if(lastPlacement.split) {
        RECT overlap{};
        if(!inside(network)||!leftGap(area)||!rightGap(network)||IntersectRect(&overlap,&area,&network)||
           !ExactGroup(networkPanel,networkInputPanel,saved.bar,network,2)) return false;
    } else if(networkPanel||networkInputPanel||(!leftGap(area)&&!rightGap(area))) return false;
    return PlacementHasNoForeignChild(saved.bar,area,network,lastPlacement.split);
}
bool CoveringEarlierNativeSibling(HWND visual,HWND bar) {
    RECT area{}; DWORD explorer=0;
    if(!visual||!GetWindowRect(visual,&area)||!GetWindowThreadProcessId(bar,&explorer)) return false;
    HWND previous=GetWindow(visual,GW_HWNDPREV);
    for(unsigned i=0;previous&&i<128;++i) {
        DWORD owner=0; RECT rect{},overlap{};
        GetWindowThreadProcessId(previous,&owner);
        if(owner==explorer&&GetParent(previous)==bar&&IsWindowVisible(previous)&&
           GetWindowRect(previous,&rect)&&IntersectRect(&overlap,&area,&rect)) return true;
        previous=GetWindow(previous,GW_HWNDPREV);
    }
    return false;
}
void HideTaskbarPanels() {
    if(panel) ShowWindow(panel,SW_HIDE);
    if(inputPanel) ShowWindow(inputPanel,SW_HIDE);
    if(networkPanel) ShowWindow(networkPanel,SW_HIDE);
    if(networkInputPanel) ShowWindow(networkInputPanel,SW_HIDE);
}
bool PlaceGroup(HWND bar,int x,int y,int width,int height,int group,HWND& visual,HWND& input,bool force) {
    if(visual&&(!IsWindow(visual)||GetParent(visual)!=bar)) { if(IsWindow(visual)) DestroyWindow(visual); visual=nullptr; }
    if(input&&(!IsWindow(input)||GetParent(input)!=bar)) { if(IsWindow(input)) DestroyWindow(input); input=nullptr; }
    if(!input||!visual) {
        DPI_AWARENESS_CONTEXT old=SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(bar));
        if(!input) input=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,INPUT_CLASS,L"LightMon 任务栏操作区域",
            WS_CHILD,0,0,width,height,bar,nullptr,instance,nullptr);
        if(input&&!visual) visual=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_LAYERED,PANEL_CLASS,L"LightMon 任务栏数值",
            WS_CHILD|WS_CLIPSIBLINGS,0,0,width,height,bar,nullptr,instance,nullptr);
        if(old) SetThreadDpiAwarenessContext(old);
        if(!input||!visual) return false;
        SetLayeredWindowAttributes(visual,PanelColorKey(),0,LWA_COLORKEY); force=true;
    }
    if(reinterpret_cast<INT_PTR>(GetPropW(visual,L"LightMon.Group"))!=group) {
        SetPropW(visual,L"LightMon.Group",reinterpret_cast<HANDLE>(static_cast<INT_PTR>(group))); force=true;
    }
    POINT point{x,y}; ScreenToClient(bar,&point); RECT oldRect{}; GetWindowRect(visual,&oldRect);
    if(force||oldRect.left!=x||oldRect.top!=y||oldRect.right-oldRect.left!=width||oldRect.bottom-oldRect.top!=height||
       CoveringEarlierNativeSibling(visual,bar)) {
        SetWindowPos(visual,HWND_TOP,point.x,point.y,width,height,SWP_NOACTIVATE);
        SetWindowPos(input,visual,point.x,point.y,width,height,SWP_NOACTIVATE); InvalidateRect(visual,nullptr,FALSE);
    }
    ShowWindow(visual,hidden?SW_HIDE:SW_SHOWNOACTIVATE); ShowWindow(input,hidden?SW_HIDE:SW_SHOWNOACTIVATE);
    return true;
}
void RestoreHeldPlacementZ() {
    const auto& p=lastPlacement; const auto& r=p.request;
    PlaceGroup(r.bar,p.x,p.y,p.split?r.leftWidth:r.width,r.height,p.split?1:0,panel,inputPanel,false);
    if(p.split) PlaceGroup(r.bar,p.networkX,p.y,r.rightWidth,r.height,2,networkPanel,networkInputPanel,false);
}
void PlaceTaskbar(bool force=false) {
    menuHoldValid=false;
    if(closing) return;
    if(hidden) {
        systemMenuObserved=false; systemMenuUnknownDeadline=0; nativeMenuState=NativeMenuState::Closed;
        CancelMenuGrace(); HideTaskbarPanels(); return;
    }
    if(menuOpen) return;
    const ULONGLONG now=GetTickCount64();
    nativeMenuState=ClassicTaskbarMenu(lastPlacement.valid?lastPlacement.request.bar:nullptr);
    if(nativeMenuState==NativeMenuState::Open) {
        systemMenuObserved=true; systemMenuUnknownDeadline=0;
        menuHoldValid=ValidHeldPlacement();
        if(menuHoldValid) { RestoreHeldPlacementZ(); return; }
    } else if(systemMenuObserved&&nativeMenuState==NativeMenuState::Unknown) {
        if(!systemMenuUnknownDeadline) systemMenuUnknownDeadline=now+1000;
        menuHoldValid=now<systemMenuUnknownDeadline&&ValidHeldPlacement();
        if(menuHoldValid) { RestoreHeldPlacementZ(); return; }
        if(now>=systemMenuUnknownDeadline) { systemMenuObserved=false; systemMenuUnknownDeadline=0; }
    } else if(systemMenuObserved) {
        systemMenuObserved=false; systemMenuUnknownDeadline=0; RequestPostMenuRefresh();
    }
    if(menuGapDeadline) {
        menuHoldValid=now<menuGapDeadline&&ValidHeldPlacement();
        if(menuHoldValid) { RestoreHeldPlacementZ(); return; }
        CancelMenuGrace();
    }
    const GapBatch batch=CurrentTaskbarRequests();
    GapRequest selected; int x=0,networkX=0; bool split=false;
    spaceAvailable=batch.count&&FindTaskbarGap(batch,selected,x,networkX,split);
    if(!spaceAvailable) { HideTaskbarPanels(); return; }
    HWND bar=selected.bar; const RECT& barRect=selected.barRect;
    const int width=selected.width,height=selected.height;
    const int y=barRect.top+(barRect.bottom-barRect.top-height)/2;
    if(selected.dpi!=dpi||!valueFont) { dpi=selected.dpi; Fonts(); force=true; }
    spaceAvailable=PlaceGroup(bar,x,y,split?selected.leftWidth:width,height,split?1:0,panel,inputPanel,force);
    if(split&&spaceAvailable) spaceAvailable=PlaceGroup(bar,networkX,y,selected.rightWidth,height,2,networkPanel,networkInputPanel,force);
    if(!split) {
        if(networkPanel) DestroyWindow(networkPanel); if(networkInputPanel) DestroyWindow(networkInputPanel);
        networkPanel=nullptr; networkInputPanel=nullptr;
    }
    if(!spaceAvailable) HideTaskbarPanels();
    else {
        DWORD explorer=0; GetWindowThreadProcessId(bar,&explorer);
        lastPlacement=Placement{selected,explorer,x,y,networkX,split,true};
    }
}
void FormatRate(double rate,wchar_t* out,size_t count) {
    const wchar_t* units[]={L"B/s",L"KB/s",L"MB/s",L"GB/s",L"TB/s",L"PB/s",L"EB/s"};
    std::size_t unit=0;
    while(rate>=1024&&unit+1<std::size(units)) { rate/=1024; ++unit; }
    // Keep a decimal at low rates, including the rounding boundary at 100.
    // Large rates use integers; the fixed column never shifts per sample.
    swprintf_s(out,count,unit&&rate<99.95?L"%.1f%s":L"%.0f%s",rate,units[unit]);
}
void FormatTraffic(std::uint64_t bytes,wchar_t* out,size_t count) {
    const wchar_t* units[]={L"B",L"KB",L"MB",L"GB",L"TB",L"PB",L"EB"};
    double amount=static_cast<double>(bytes); std::size_t unit=0;
    while(amount>=1024&&unit+1<std::size(units)) { amount/=1024; ++unit; }
    swprintf_s(out,count,unit&&amount<99.95?L"%.1f%s":L"%.0f%s",amount,units[unit]);
}
HICON MakeIcon() {
    int s=GetSystemMetricsForDpi(SM_CXSMICON,dpi);
    HDC dc=GetDC(nullptr), mem=CreateCompatibleDC(dc);
    HBITMAP color=CreateCompatibleBitmap(dc,s,s), mask=CreateBitmap(s,s,1,1,nullptr);
    HGDIOBJ old=SelectObject(mem,color);
    RECT r{0,0,s,s}; HBRUSH bg=CreateSolidBrush(RGB(20,29,38)); FillRect(mem,&r,bg); DeleteObject(bg);
    SetBkMode(mem,TRANSPARENT); SetTextColor(mem,RGB(75,221,191));
    HFONT font=CreateFontW(-std::max(10,s-5),0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Segoe UI");
    HGDIOBJ of=SelectObject(mem,font); wchar_t text[12];
    if(sample.cpuAvailable) swprintf_s(text,L"%.0f",sample.cpuPercent); else wcscpy_s(text,L"M");
    DrawTextW(mem,text,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SelectObject(mem,of); DeleteObject(font); SelectObject(mem,old);
    HGDIOBJ om=SelectObject(mem,mask); PatBlt(mem,0,0,s,s,BLACKNESS); SelectObject(mem,om);
    ICONINFO info{TRUE,0,0,mask,color}; HICON result=CreateIconIndirect(&info);
    DeleteObject(color); DeleteObject(mask); DeleteDC(mem); ReleaseDC(nullptr,dc); return result;
}
void UpdateTray(bool force=false) {
    static int lastCpu=-999;
    int cpu=sample.cpuAvailable?(int)(sample.cpuPercent+0.5):-1;
    bool change=force||!trayIcon||lastCpu!=cpu;
    HICON fresh=change?MakeIcon():trayIcon;
    wchar_t down[32],up[32],cpuText[16],ramText[16]; FormatRate(sample.downloadBytesPerSec,down,32); FormatRate(sample.uploadBytesPerSec,up,32);
    if(!sample.networkAvailable) { wcscpy_s(down,L"—"); wcscpy_s(up,L"—"); }
    if(sample.cpuAvailable) swprintf_s(cpuText,L"%.0f%%",sample.cpuPercent); else wcscpy_s(cpuText,L"—");
    if(sample.ramAvailable) swprintf_s(ramText,L"%.0f%%",sample.ramPercent); else wcscpy_s(ramText,L"—");
    NOTIFYICONDATAW ni{sizeof(ni)}; ni.hWnd=wnd; ni.uID=1; ni.uCallbackMessage=WM_TRAY;
    ni.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP|NIF_SHOWTIP; ni.hIcon=fresh;
    wchar_t todayDown[32],todayUp[32];
    FormatTraffic(sample.dailyTraffic.downloadBytes,todayDown,32);
    FormatTraffic(sample.dailyTraffic.uploadBytes,todayUp,32);
    _snwprintf_s(ni.szTip,std::size(ni.szTip),_TRUNCATE,L"轻量监控  CPU %s  内存 %s\n下载 %s  上传 %s\n今日下载 %s  上传 %s\n%s · 右键设置",cpuText,ramText,down,up,todayDown,todayUp,
        hidden?L"任务栏数值已隐藏":spaceAvailable?L"底部任务栏内显示":L"空间不足，数值暂时隐藏");
    bool ok=Shell_NotifyIconW(trayAdded?NIM_MODIFY:NIM_ADD,&ni)!=FALSE;
    if(ok&&!trayAdded) { trayAdded=true; ni.uVersion=NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION,&ni); }
    if(change) { if(trayIcon) DestroyIcon(trayIcon); trayIcon=fresh; lastCpu=cpu; }
}
enum class MetricKind { Cpu,Memory,Gpu,Disk,Temperature,Network };
COLORREF MetricColor(MetricKind kind) {
    // Dark text for the light taskbar, light text for the dark taskbar.
    // Classification remains visible in the labels as well as the colors.
    static constexpr COLORREF light[]={RGB(53,92,152),RGB(52,105,71),RGB(124,72,151),
        RGB(139,79,42),RGB(155,61,94),RGB(30,103,117)};
    static constexpr COLORREF dark[]={RGB(169,200,242),RGB(165,214,177),RGB(213,183,235),
        RGB(242,195,160),RGB(239,180,202),RGB(163,220,227)};
    return (lightTheme?light:dark)[static_cast<unsigned>(kind)];
}
MetricKind MetricKindForLabel(const wchar_t* label) {
    if(!wcscmp(label,L"CPU")||!wcscmp(label,L"忙核")) return MetricKind::Cpu;
    if(!wcscmp(label,L"内存")) return MetricKind::Memory;
    if(!wcscmp(label,L"磁盘")) return MetricKind::Disk;
    if(wcsstr(label,L"温")) return MetricKind::Temperature;
    return MetricKind::Gpu;
}
void Paint(HWND h,HDC target=nullptr) {
    const int group=static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(h,L"LightMon.Group")));
    PAINTSTRUCT ps{}; HDC destination=target?target:BeginPaint(h,&ps); RECT r{}; GetClientRect(h,&r);
    if(r.right<=0||r.bottom<=0) { if(!target) EndPaint(h,&ps); return; }
    HDC screen=GetDC(nullptr),mem=CreateCompatibleDC(screen);
    HBITMAP bitmap=CreateCompatibleBitmap(screen,r.right,r.bottom);
    if(!destination||!mem||!bitmap) {
        if(bitmap) DeleteObject(bitmap); if(mem) DeleteDC(mem); ReleaseDC(nullptr,screen);
        if(!target) EndPaint(h,&ps); return;
    }
    HGDIOBJ ob=SelectObject(mem,bitmap),of=SelectObject(mem,labelFont);
    const COLORREF background=PanelColorKey();
    HBRUSH brush=CreateSolidBrush(background); FillRect(mem,&r,brush); DeleteObject(brush);
    SetBkColor(mem,background); SetBkMode(mem,TRANSPARENT);
    SetTextColor(mem,lightTheme?RGB(0,0,0):RGB(255,255,255));
    int x=Scale(1);
    auto row=[&](const wchar_t* label,const wchar_t* value,int y,int width){
        const int rowHeight=r.bottom/2;
        RECT labelRect{x,y,x+width,y+rowHeight};
        SetTextColor(mem,MetricColor(MetricKindForLabel(label)));
        SelectObject(mem,labelFont);
        DrawTextW(mem,label,-1,&labelRect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        SelectObject(mem,valueFont); SIZE extent{};
        GetTextExtentPoint32W(mem,value,int(wcslen(value)),&extent);
        RECT valueRect{x+width-extent.cx,y,x+width,y+rowHeight};
        DrawTextW(mem,value,-1,&valueRect,DT_RIGHT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    };
    auto separator=[&](int position) {
        HGDIOBJ oldPen=SelectObject(mem,GetStockObject(DC_PEN));
        SetDCPenColor(mem,lightTheme?RGB(140,140,140):RGB(110,110,110));
        MoveToEx(mem,position,Scale(4),nullptr); LineTo(mem,position,r.bottom-Scale(4));
        SelectObject(mem,oldPen);
    };
    int previousRight=0; bool previousColumn=false;
    // Bounds are the union of both rows. Split all free pixels between adjacent
    // visible content equally, including unused network-token budget. One
    // shared vertical line can differ by one pixel when the gap is even.
    auto beginColumn=[&](int left) {
        if(previousColumn) separator(previousRight+(left-previousRight-1)/2);
    };
    auto endColumn=[&](int right,int width) {
        previousRight=right; previousColumn=true; x+=width+ColumnGap();
    };
    auto column=[&](const wchar_t* label1,const wchar_t* value1,const wchar_t* label2,const wchar_t* value2,int width){
        beginColumn(x);
        row(label1,value1,0,width); row(label2,value2,r.bottom/2,width);
        endColumn(x+width,width);
    };
    wchar_t cpu[32],peak[32],downRate[32],upRate[32],down[48],up[48];
    if(sample.cpuAvailable) swprintf_s(cpu,L"%.0f%%",sample.cpuPercent); else wcscpy_s(cpu,L"—");
    if(sample.cpuPeakAvailable) swprintf_s(peak,L"%.0f%%",sample.cpuPeakPercent); else wcscpy_s(peak,L"—");
    FormatRate(sample.downloadBytesPerSec,downRate,32); FormatRate(sample.uploadBytesPerSec,upRate,32);
    if(!sample.networkAvailable) { wcscpy_s(downRate,L"—"); wcscpy_s(upRate,L"—"); }
    swprintf_s(down,L"↓%s",downRate); swprintf_s(up,L"↑%s",upRate);
    if(group!=2) column(L"CPU",cpu,L"忙核",peak,CpuWidth());
    // Center the two-row arrow+rate group together; arrows stay aligned with
    // each other and attached to their values even when the rates are short.
    if(group!=1) {
    const int networkWidth=NetworkWidth();
    const int tokenWidth=std::max(TextWidth(down),TextWidth(up));
    const int networkLeft=x+(networkWidth-tokenWidth)/2;
    beginColumn(networkLeft);
    SelectObject(mem,valueFont);
    SetTextColor(mem,MetricColor(MetricKind::Network));
    RECT downRect{networkLeft,0,networkLeft+tokenWidth,r.bottom/2};
    RECT upRect{networkLeft,r.bottom/2,networkLeft+tokenWidth,r.bottom};
    DrawTextW(mem,down,-1,&downRect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    DrawTextW(mem,up,-1,&upRect,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    endColumn(networkLeft+tokenWidth,networkWidth);
    if(cfg.dailyTraffic) {
        wchar_t amountDown[32],amountUp[32],todayDown[48],todayUp[48];
        FormatTraffic(sample.dailyTraffic.downloadBytes,amountDown,32);
        FormatTraffic(sample.dailyTraffic.uploadBytes,amountUp,32);
        if(!sample.dailyTrafficAvailable) { wcscpy_s(amountDown,L"—"); wcscpy_s(amountUp,L"—"); }
        swprintf_s(todayDown,L"总↓%s",amountDown); swprintf_s(todayUp,L"总↑%s",amountUp);
        const int width=DailyTrafficWidth();
        const int token=std::max(TextWidth(todayDown),TextWidth(todayUp));
        const int left=x+(width-token)/2;
        beginColumn(left);
        SetTextColor(mem,MetricColor(MetricKind::Network));
        RECT downDaily{left,0,left+token,r.bottom/2};
        RECT upDaily{left,r.bottom/2,left+token,r.bottom};
        DrawTextW(mem,todayDown,-1,&downDaily,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        DrawTextW(mem,todayUp,-1,&upDaily,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
        endColumn(left+token,width);
    }
    }
    auto paintCells=[&](const std::array<GpuCell,12>& cells,std::size_t count) {
        if(!count) return;
        for(std::size_t i=0;i<count;i+=2) {
            const int width=i+1<count?std::max(cells[i].width,cells[i+1].width):cells[i].width;
            column(cells[i].label,cells[i].value,i+1<count?cells[i+1].label:L"",
                   i+1<count?cells[i+1].value:L"",width);
        }
    };
    std::array<GpuCell,12> metricCells{},temperatureCells{};
    const auto metricCount=GpuCells(metricCells),temperatureCount=TemperatureCells(temperatureCells);
    if(group!=2) paintCells(metricCells,metricCount);
    if(group!=2) paintCells(temperatureCells,temperatureCount);
    // Keep GDI's native RGB text edges. Only the exact background color is
    // keyed out by the layered taskbar child; no grayscale/alpha conversion.
    BitBlt(destination,0,0,r.right,r.bottom,mem,0,0,SRCCOPY);
    SelectObject(mem,of); SelectObject(mem,ob); DeleteObject(bitmap); DeleteDC(mem);
    ReleaseDC(nullptr,screen); if(!target) EndPaint(h,&ps);
}
void ToggleShow() {
    hidden=!hidden;
    CancelMenuGrace();
    if(hidden) HideTaskbarPanels(); else PlaceTaskbar(true);
    UpdateTray(true);
}
void Menu() {
    if(menuOpen||closing) return;
    HMENU menu=CreatePopupMenu();
    auto add=[&](int id,const wchar_t* text,bool checked=false){ AppendMenuW(menu,MF_STRING|(checked?MF_CHECKED:0),id,text); };
    add(ID_SHOW,hidden?L"显示任务栏数值":L"隐藏任务栏数值"); AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    add(ID_LITE,L"省内存模式",!cfg.gpu&&!cfg.disk); add(ID_GPU,L"显卡负载（独显／核显）",cfg.gpu);
    add(ID_GPU_SPLIT,L"分开显示独显引擎",cfg.gpuSplit);
    add(ID_INTEGRATED_GPU,L"核显总体占用",cfg.integratedGpu);
    HMENU engines=CreatePopupMenu();
    for(std::size_t i=0;i<lightmon::kGpuEngineCategoryCount;++i)
        AppendMenuW(engines,MF_STRING|((cfg.gpuEngineMask&(1u<<i))?MF_CHECKED:0),ID_ENGINE_FIRST+i,ENGINE_MENU_LABELS[i]);
    AppendMenuW(menu,MF_POPUP,(UINT_PTR)engines,L"独显引擎显示项");
    add(ID_DISK,L"磁盘活动",cfg.disk); AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    add(ID_DAILY_TRAFFIC,L"今日下载／上传（任务栏）",cfg.dailyTraffic);
    add(ID_SMOOTH_FONT,L"系统字体平滑",cfg.smoothFont);
    add(ID_AUTO_THEME,L"文字颜色跟随任务栏",cfg.theme<0); add(ID_THEME,L"深色文字（浅色任务栏）",cfg.theme==1);
    HMENU refresh=CreatePopupMenu(); AppendMenuW(refresh,MF_STRING|(cfg.interval==1000?MF_CHECKED:0),ID_1SEC,L"1 秒");
    AppendMenuW(refresh,MF_STRING|(cfg.interval==2000?MF_CHECKED:0),ID_2SEC,L"2 秒");
    AppendMenuW(refresh,MF_STRING|(cfg.interval==5000?MF_CHECKED:0),ID_5SEC,L"5 秒（更省电）");
    AppendMenuW(menu,MF_POPUP,(UINT_PTR)refresh,L"刷新间隔");
    add(ID_RESET,L"重新定位任务栏"); AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    add(ID_TEMPERATURE_DETAILS,L"详细温度（全部传感器）");
    add(ID_CPU_TEMPERATURE,sampler->CpuTemperatureHelperActive()?L"停用 CPU 温度":L"启用 CPU 温度（需要 Windows 授权）",sampler->CpuTemperatureHelperActive());
    add(ID_CPU_TEMPERATURE_AUTO,L"随软件自动读取 CPU 温度",cfg.cpuTemperatureAuto);
    add(ID_CPU_DETAILS,L"CPU 详细数值（含频率）");
    add(ID_DAILY_TRAFFIC_DETAILS,L"今日流量详情（含合计）");
    add(ID_ABOUT,L"详细数值 / 关于"); add(ID_EXIT,L"退出");
    POINT p{}; GetCursorPos(&p); SetForegroundWindow(wnd);
    menuOpen=true;
    UINT result=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,wnd,nullptr);
    menuOpen=false;
    RequestPostMenuRefresh();
    PostMessageW(wnd,WM_NULL,0,0); DestroyMenu(menu); if(result) SendMessageW(wnd,WM_COMMAND,result,0);
}
void Diagnostics() {
    if(!report) return;
    PROCESS_MEMORY_COUNTERS_EX pm{sizeof(pm)}; GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&pm,sizeof(pm));
    FILETIME c,e,k,u; GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u);
    fprintf(report,"%.3f,%zu,%zu,%llu,%lu,%lu,%.2f,%.2f,%.2f,%.2f,%.2f,%u,%llu,%llu,%.2f,%d,%d,%d,%d,%d,",
        (GetTickCount64()-startTick)/1000.0,pm.WorkingSetSize,pm.PrivateUsage,FileTimeValue(k)+FileTimeValue(u),
        GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS),GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS),
        sample.cpuPercent,sample.ramPercent,sample.downloadBytesPerSec,sample.uploadBytesPerSec,sample.gpuPercent,
        sample.gpuTemperatureC,(unsigned long long)sample.gpuUsedBytes,(unsigned long long)sample.gpuTotalBytes,sample.diskBusyPercent,
        sample.cpuAvailable,sample.networkAvailable,sample.gpuUtilizationAvailable,sample.gpuTemperatureAvailable,sample.diskAvailable);
    char engineType[64]{};
    WideCharToMultiByte(CP_UTF8,0,sample.gpuEngineType,-1,engineType,sizeof(engineType),nullptr,nullptr);
    fprintf(report,"%llu,%llu,%llu,0x%08x,0x%08x,%u,%u,%s,%u,%u,%u,%u,%.2f,%d,%s,%s",
        (unsigned long long)sample.sampledAtTickMs,(unsigned long long)sample.cpuSampleIntervalMs,
        (unsigned long long)sample.gpuSampleIntervalMs,sample.gpuLuidHigh,sample.gpuLuidLow,
        sample.gpuPhysicalIndex,sample.gpuEngineIndex,engineType,sample.gpuEngineValidItems,
        sample.gpuEngineInvalidItems,sample.gpuEngineCount,static_cast<unsigned int>(sample.gpuStatus),
        sample.nvmlKernelPercent,sample.nvmlKernelAvailable,sample.gpuPciBusId,sample.gpuUuid);
    for(const auto& engine:sample.gpuEngines) fprintf(report,",%.4f,%d,%u,%u,%u,%d,%d",
        engine.percent,engine.available,engine.engineCount,engine.physicalIndex,engine.engineIndex,engine.supportKnown,engine.supported);
    char integratedEngine[96]{};
    WideCharToMultiByte(CP_UTF8,0,sample.integratedGpu.engineType,-1,integratedEngine,sizeof(integratedEngine),nullptr,nullptr);
    fprintf(report,",%.4f,%d,%d,%u,%llu,0x%08x,0x%08x,%u,%u,%s,%u,%u,%u,%s",
        sample.integratedGpu.percent,sample.integratedGpu.utilizationAvailable,sample.integratedGpu.deviceAvailable,
        static_cast<unsigned int>(sample.integratedGpu.status),(unsigned long long)sample.integratedGpu.sampleIntervalMs,
        sample.integratedGpu.luidHigh,sample.integratedGpu.luidLow,sample.integratedGpu.physicalIndex,sample.integratedGpu.engineIndex,
        integratedEngine,sample.integratedGpu.validItems,sample.integratedGpu.invalidItems,sample.integratedGpu.engineCount,sample.integratedGpu.pciBusId);
    fprintf(report,",%d,%d,%d,%d,%d,%d,%u,%u,%zu,%u,%d,%u,%d",
        sample.cpuTemperatureC,sample.cpuTemperatureAvailable,sample.integratedGpuTemperatureC,
        sample.integratedGpuTemperatureAvailable,sample.diskTemperatureC,sample.diskTemperatureAvailable,
        sample.hottestDiskIndex,sample.hottestDiskSensorIndex,sample.diskTemperatureCount,
        sample.gpuHotspotTemperatureC,sample.gpuHotspotTemperatureAvailable,
        sample.gpuMemoryTemperatureC,sample.gpuMemoryTemperatureAvailable);
    fprintf(report,",%u,0x%08x,0x%08x,%llu,%d",
        static_cast<unsigned>(sample.cpuTemperatureStatus),static_cast<unsigned>(sample.cpuTemperatureError),
        sample.cpuTemperatureRaw,static_cast<unsigned long long>(sample.cpuTemperatureSampledAtTickMs),sample.cpuTemperatureHelperActive);
    fprintf(report,",%.6f,%d,%u,%u,%u,%u,%u,%llu,%u,%.6f,%.6f,%.6f,%d,%u,%u,%llu,%u,%.6f,%.6f,%d",
        sample.cpuPeakPercent,sample.cpuPeakAvailable,sample.cpuPeakGroup,sample.cpuPeakLogicalProcessor,
        sample.cpuLogicalProcessorCount,sample.cpuPeakValidItems,sample.cpuPeakInvalidItems,
        static_cast<unsigned long long>(sample.cpuPeakSampleIntervalMs),static_cast<unsigned>(sample.cpuPeakStatus),
        sample.cpuFrequencyMinMHz,sample.cpuFrequencyMaxMHz,sample.cpuFrequencyMeanMHz,sample.cpuFrequencyAvailable,
        sample.cpuFrequencyValidItems,sample.cpuFrequencyInvalidItems,
        static_cast<unsigned long long>(sample.cpuFrequencySampleIntervalMs),static_cast<unsigned>(sample.cpuFrequencyStatus),
        sample.cpuReportedFrequencyMinMHz,sample.cpuReportedFrequencyMaxMHz,sample.cpuReportedFrequencyAvailable);
    fprintf(report,",%u,%llu,%llu,%d,%u,%u,%d,%d,%d",sample.dailyTraffic.date,
        static_cast<unsigned long long>(sample.dailyTraffic.downloadBytes),
        static_cast<unsigned long long>(sample.dailyTraffic.uploadBytes),sample.dailyTrafficAvailable,
        static_cast<unsigned>(sample.dailyTrafficStoreStatus),sample.dailyTrafficStoreError,
        sample.dailyTrafficStoreAttention,sample.dailyTrafficIncomplete,sample.dailyTrafficOverflowed);
    fprintf(report,",%d,%d",static_cast<int>(nativeMenuState),menuHoldValid);
    fputc('\n',report);
    fflush(report);
}
void BeginClose(const char* reason) {
    if(closing) return;
    lightmon::lifetime::LogEvent(reason);
    sample=sampler->Sample(); sampler->FlushDailyTraffic();
    closing=true; KillTimer(wnd,1);
    if(!gapThread.joinable()) { DestroyWindow(wnd); return; }
    { std::lock_guard<std::mutex> lock(gapMutex); gapStop=true; }
    gapWake.notify_one();
    // Keep pumping provider messages until the MTA worker has released UIA.
    // Its completion message performs the actual window teardown.
}
void BeginManualClose(const char* reason) {
    if(closing) return;
    if(!testing) automaticRecovery::PauseForCurrentLogon();
    BeginClose(reason);
}
const wchar_t* CpuCounterState(lightmon::CpuCounterStatus status) {
    switch(status) {
    case lightmon::CpuCounterStatus::WarmingUp: return L"等待首个完整采样";
    case lightmon::CpuCounterStatus::InvalidSample: return L"本次采样无效";
    case lightmon::CpuCounterStatus::IncompleteSample: return L"逻辑处理器读数不完整";
    case lightmon::CpuCounterStatus::BufferTooLarge: return L"计数器数据超出容量";
    case lightmon::CpuCounterStatus::Available: return L"可用";
    default: return L"系统计数器不可用";
    }
}
void CpuDetails(wchar_t (&text)[2048]) {
    wchar_t total[64],peak[160],frequency[160];
    if(sample.cpuAvailable) swprintf_s(total,L"%.1f%%",sample.cpuPercent);
    else wcscpy_s(total,L"—（等待有效采样）");
    if(sample.cpuPeakAvailable) swprintf_s(peak,L"%.1f%%（组 %u，逻辑核 %u；编号从 0 开始）",
        sample.cpuPeakPercent,sample.cpuPeakGroup,sample.cpuPeakLogicalProcessor);
    else swprintf_s(peak,L"—（%s）",CpuCounterState(sample.cpuPeakStatus));
    if(sample.cpuFrequencyAvailable) swprintf_s(frequency,L"%.2f–%.2f GHz；逐核均值 %.2f GHz",
        sample.cpuFrequencyMinMHz/1000.0,sample.cpuFrequencyMaxMHz/1000.0,sample.cpuFrequencyMeanMHz/1000.0);
    else swprintf_s(frequency,L"—（%s）",CpuCounterState(sample.cpuFrequencyStatus));
    swprintf_s(text,L"CPU 总占用：%s\n最忙逻辑核：%s\nWindows 测量频率：%s\n\n有效逻辑核：占用 %u/%u；频率 %u/%u\n采样间隔：占用 %.3f 秒；频率 %.3f 秒\n\n最忙核取所有逻辑处理器忙碌时间的最大值，含 SMT 逻辑核。\n频率来自 Windows Processor Information / Actual Frequency（MHz）。\n显示各逻辑核的最小值、最大值和算术平均值。\n详情是打开时的快照，频率只在详情显示。",
        total,peak,frequency,sample.cpuPeakValidItems,sample.cpuLogicalProcessorCount,
        sample.cpuFrequencyValidItems,sample.cpuLogicalProcessorCount,
        sample.cpuPeakSampleIntervalMs/1000.0,sample.cpuFrequencySampleIntervalMs/1000.0);
}
void Command(UINT id) {
    if(closing) return;
    if(id>=ID_ENGINE_FIRST&&id<ID_ENGINE_FIRST+lightmon::kGpuEngineCategoryCount) {
        cfg.gpuEngineMask^=1u<<(id-ID_ENGINE_FIRST); cfg.gpuSplit=true;
        PlaceTaskbar(true); UpdateTray(true); WriteConfig(); return;
    }
    switch(id) {
    case ID_SHOW: ToggleShow(); return;
    case ID_GPU: cfg.gpu=!cfg.gpu; sampler->Configure(cfg.gpu,cfg.disk); break;
    case ID_DISK: cfg.disk=!cfg.disk; sampler->Configure(cfg.gpu,cfg.disk); break;
    case ID_GPU_SPLIT: cfg.gpuSplit=!cfg.gpuSplit; break;
    case ID_INTEGRATED_GPU: cfg.integratedGpu=!cfg.integratedGpu; break;
    case ID_DAILY_TRAFFIC: cfg.dailyTraffic=!cfg.dailyTraffic; break;
    case ID_LITE: cfg.gpu=false; cfg.disk=false; sampler->Configure(false,false); break;
    
    
    case ID_SMOOTH_FONT: cfg.smoothFont=!cfg.smoothFont; Fonts(); break;
    case ID_THEME: cfg.theme=cfg.theme==1?0:1; UpdateTheme(); break;
    case ID_AUTO_THEME: cfg.theme=-1; UpdateTheme(); break;
    case ID_1SEC: case ID_2SEC: case ID_5SEC: cfg.interval=id==ID_1SEC?1000:id==ID_2SEC?2000:5000; KillTimer(wnd,1); SetTimer(wnd,1,cfg.interval,nullptr); break;
    case ID_RESET: PlaceTaskbar(true); return;
    case ID_CPU_TEMPERATURE:
        if(sampler->CpuTemperatureHelperActive()) { cfg.cpuTemperatureAuto=false; sampler->DisableCpuTemperature(); }
        else if(!sampler->EnableCpuTemperature(reinterpret_cast<std::uintptr_t>(wnd))) {
            MessageBoxW(wnd,L"CPU 温度需要管理员权限读取已安装的 PawnIO 驱动。\n授权取消或组件不可用时不会显示估算温度。\n可从右键菜单再次启用。",L"LightMon · CPU 温度",MB_OK|MB_ICONINFORMATION);
        }
        sample=sampler->Sample(); break;
    case ID_CPU_TEMPERATURE_AUTO:
        if(cfg.cpuTemperatureAuto) { cfg.cpuTemperatureAuto=false; sampler->DisableCpuTemperature(); }
        else if(sampler->EnableCpuTemperatureAutomatic()) cfg.cpuTemperatureAuto=true;
        else MessageBoxW(wnd,L"CPU 温度自动读取未能启动。\n请确认此电脑已配置温度专用启动任务，以及 PawnIO 读取组件可用。\n右键“全部温度详情”可查看错误状态。",L"LightMon · CPU 温度",MB_OK|MB_ICONINFORMATION);
        sample=sampler->Sample(); break;
    case ID_CPU_DETAILS: {
        wchar_t text[2048]; CpuDetails(text);
        MessageBoxW(wnd,text,L"LightMon · CPU 详细数值",MB_OK|MB_ICONINFORMATION); return;
    }
    case ID_DAILY_TRAFFIC_DETAILS: {
        wchar_t down[32],up[32],combined[64],text[2048];
        FormatTraffic(sample.dailyTraffic.downloadBytes,down,32);
        FormatTraffic(sample.dailyTraffic.uploadBytes,up,32);
        const auto maxBytes=UINT64_MAX;
        const bool combinedOverflow=sample.dailyTraffic.uploadBytes>maxBytes-sample.dailyTraffic.downloadBytes;
        if(combinedOverflow) wcscpy_s(combined,L"超出统计范围");
        else FormatTraffic(sample.dailyTraffic.downloadBytes+sample.dailyTraffic.uploadBytes,combined,64);
        const wchar_t* persistence=L"已启用本地保存（最多每 30 秒更新，正常退出时保存）";
        if(sample.dailyTrafficStoreStatus==lightmon::DailyTrafficStoreStatus::MemoryOnly) persistence=L"测试模式：仅在内存累计";
        if(sample.dailyTrafficStoreStatus==lightmon::DailyTrafficStoreStatus::WriteFailed) persistence=L"保存失败：当前仍在内存累计，退出后可能丢失未保存量";
        swprintf_s(text,L"日期：%04u-%02u-%02u（本地日期）\n\n今日下载：%s（%llu 字节）\n今日上传：%s（%llu 字节）\n今日合计：%s\n\n%s\n%s%s\n\n来源：Windows 物理以太网／Wi-Fi 网卡的原始收发字节计数，包含局域网传输。虚拟 VPN、回环及隧道接口不重复累计。\n单位按 1024 换算；详情是打开时的快照。\n\n首次启用从当时开始累计；重启恢复已经保存的值，软件关闭期间不补计。\n切换本地日期自动重新累计，跨日的首个间隔无法准确分配，因此不计入新一天。\n隐藏此列仍继续累计。",
            sample.dailyTraffic.date/10000,sample.dailyTraffic.date/100%100,sample.dailyTraffic.date%100,
            down,static_cast<unsigned long long>(sample.dailyTraffic.downloadBytes),up,
            static_cast<unsigned long long>(sample.dailyTraffic.uploadBytes),combined,persistence,
            sample.dailyTrafficStoreAttention?L"本次曾遇到记录损坏或读写失败，本日记录可能不完整。\n":L"",
            sample.dailyTrafficIncomplete||sample.dailyTrafficOverflowed?L"本日曾有采集缺口或超出容量，已记录值仍保留。":L"");
        MessageBoxW(wnd,text,L"LightMon · 今日网络流量",MB_OK|MB_ICONINFORMATION); return;
    }
    case ID_TEMPERATURE_DETAILS: {
        wchar_t text[16384]=L"温度单位：摄氏度（任务栏用 ° 简写）\n\n";
        auto add=[&](const wchar_t* label,bool available,int value) {
            wchar_t line[160];
            if(available) swprintf_s(line,L"%s：%d°C\n",label,value);
            else swprintf_s(line,L"%s：—（当前没有可用读数）\n",label);
            wcscat_s(text,line);
        };
        add(L"CPU",sample.cpuTemperatureAvailable,sample.cpuTemperatureC);
        if(!sample.cpuTemperatureAvailable) {
            const wchar_t* reason=L"读取组件未提供有效读数";
            switch(sample.cpuTemperatureStatus) {
            case lightmon::CpuTemperatureStatus::NeedsPermission: reason=L"尚未启用管理员读取组件"; break;
            case lightmon::CpuTemperatureStatus::Starting: reason=L"读取组件正在启动"; break;
            case lightmon::CpuTemperatureStatus::AccessDenied: reason=L"现有驱动拒绝访问，需要管理员权限"; break;
            case lightmon::CpuTemperatureStatus::LaunchCancelled: reason=L"Windows 权限提示已取消"; break;
            case lightmon::CpuTemperatureStatus::DriverMissing: reason=L"PawnIO 驱动未安装或未运行"; break;
            case lightmon::CpuTemperatureStatus::UnsupportedCpu: reason=L"当前 CPU 不在已验证的传感器型号范围"; break;
            case lightmon::CpuTemperatureStatus::Stale: reason=L"读数已过期，等待新的有效采样"; break;
            case lightmon::CpuTemperatureStatus::LaunchFailed: reason=L"温度读取组件启动失败，请检查专用任务或组件"; break;
            case lightmon::CpuTemperatureStatus::HelperExited: reason=L"温度读取组件已退出"; break;
            default: break;
            }
            wchar_t line[256]; swprintf_s(line,L"CPU 状态：%s（状态 %u，错误 0x%08X）\n",reason,
                static_cast<unsigned>(sample.cpuTemperatureStatus),static_cast<unsigned>(sample.cpuTemperatureError));
            wcscat_s(text,line);
        }
        add(L"核显",sample.integratedGpuTemperatureAvailable,sample.integratedGpuTemperatureC);
        add(L"独显核心",sample.gpuTemperatureAvailable,static_cast<int>(sample.gpuTemperatureC));
        add(L"独显热点",sample.gpuHotspotTemperatureAvailable,static_cast<int>(sample.gpuHotspotTemperatureC));
        add(L"独显显存",sample.gpuMemoryTemperatureAvailable,static_cast<int>(sample.gpuMemoryTemperatureC));
        add(L"独显供电",sample.gpuPowerTemperatureAvailable,static_cast<int>(sample.gpuPowerTemperatureC));
        add(L"独显板卡",sample.gpuBoardTemperatureAvailable,static_cast<int>(sample.gpuBoardTemperatureC));
        add(L"所有硬盘最高温",sample.diskTemperatureAvailable,sample.diskTemperatureC);
        if(sample.diskTemperatureAvailable) {
            wchar_t line[128]; swprintf_s(line,L"最高读数来源：物理盘 %u，传感器 %u\n",sample.hottestDiskIndex,sample.hottestDiskSensorIndex);
            wcscat_s(text,line);
        }
        for(std::size_t i=0;i<sample.diskTemperatureCount;++i) {
            const auto& sensor=sample.diskTemperatures[i]; wchar_t line[256];
            swprintf_s(line,L"盘 %u / 传感器 %u：%d°C（%s）\n",sensor.diskIndex,
                sensor.sensorIndex,sensor.temperatureC,
                sensor.model[0]?sensor.model:L"型号未提供");
            wcscat_s(text,line);
        }
        wcscat_s(text,L"\n独显：已绑定的 NVIDIA NVML 设备。\n核显：Windows WDDM 适配器温度，按同一设备 LUID 查询，每 5 秒更新。\n硬盘：Windows 原生存储温度查询，每 5 秒更新。\n额外硬盘传感器保持驱动编号，不猜测它是 NAND 或控制器。\nCPU：AMD 处理器 Tctl/Tdie 传感器，经已安装 PawnIO 驱动只读读取，每 2 秒更新。手动启用需要 Windows 管理员授权；配置专用任务后可勾选“随软件自动读取 CPU 温度”。读取组件随主程序退出而结束。\n不把 CPU 与核显温度相互代替；不估算未开放的热点和显存温度。");
        MessageBoxW(wnd,text,L"LightMon · 详细温度",MB_OK|MB_ICONINFORMATION); return;
    }
    case ID_ABOUT: {
        PROCESS_MEMORY_COUNTERS_EX pm{sizeof(pm)}; GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&pm,sizeof(pm));
        wchar_t gpuMemoryText[64];
        if(cfg.gpu&&sample.gpuMemoryAvailable) swprintf_s(gpuMemoryText,L"%.1f / %.1f GiB",sample.gpuUsedBytes/1073741824.0,sample.gpuTotalBytes/1073741824.0);
        else wcscpy_s(gpuMemoryText,cfg.gpu?L"—（无法读取）":L"未启用");
        wchar_t gpuName[96]{};
        MultiByteToWideChar(CP_UTF8,0,sample.gpuName,-1,gpuName,static_cast<int>(std::size(gpuName)));
        wchar_t engineValues[5][32]{};
        for(std::size_t i=0;i<lightmon::kGpuEngineCategoryCount;++i) {
            if(cfg.gpu&&sample.gpuEngines[i].available) swprintf_s(engineValues[i],L"%.0f%%",sample.gpuEngines[i].percent);
            else wcscpy_s(engineValues[i],sample.gpuEngines[i].supportKnown&&!sample.gpuEngines[i].supported?
                L"暂无独立统计":L"—");
        }
        wchar_t integratedValue[32]{};
        if(cfg.gpu&&sample.integratedGpu.utilizationAvailable) swprintf_s(integratedValue,L"%.0f%%",sample.integratedGpu.percent);
        else wcscpy_s(integratedValue,L"—");
        wchar_t text[2048]; swprintf_s(text,L"LightMon 轻量监控 1.1.28\n\n内存：%.1f / %.1f GiB（%.0f%%）\n独显显存：%s\n本程序物理内存：%.1f MiB\n私有提交量：%.1f MiB\n\nCPU：Windows GetSystemTimes 平均忙碌时间。最忙核逐逻辑处理器取最大值。\n频率请打开右键“CPU 详细数值（含频率）”。\nGPU：Windows GPU Engine，同一批数据按设备 LUID 分别统计。\n独显：%s\n3D %s · 计算 %s · 复制 %s\n视频编码 %s · 视频解码 %s\n每类取该类最忙引擎；同一引擎跨进程汇总。\n独显总体最忙引擎：%s；CPU / GPU 采样间隔：%.3f / %.3f 秒。\n核显：%s\n核显总体：%s；最忙引擎：%s；采样间隔：%.3f 秒。\n不把不同引擎或不同显卡的占用相加。\n独显按 NVML UUID / PCI 地址绑定；核显由 Windows DXCore IsIntegrated / IsHardware 确认。\nCPU 温度来自 AMD Tctl/Tdie 传感器，可配置随软件自动读取；独显温度和显存来自 NVIDIA NVML；核显温度来自 Windows WDDM；盘温来自 Windows 原生存储查询。任务栏盘温取全部有效硬盘传感器的最高值；右键可查看全部温度。未开放的温度显示 —。内核活跃率仅用于诊断。\n驱动未开放独立引擎统计显示“暂无”；启动或采样失败显示 —。计算没有独立计数器时不复用 3D 或总体占用。\n\n只在底部任务栏显示；右键数值或托盘图标设置。\n固定两行，10磅微软雅黑，指标分类配色；优先使用主屏任务栏空位。\nCPU与忙核上下同列；内存、GPU、磁盘和全部温度仍放左侧；即时网速与今日流量放右侧。也可使用副屏底部空位。\n所有任务栏均无足够空位时暂时隐藏数值，可减少显示项。\n网速汇总已连接的物理网卡，单位 B/s。今日流量请打开右键详情。任务栏温度 ° 表示摄氏度。\n磁盘为最忙物理磁盘的活动时间比例。\n\n原生 Win32 · 无新增服务或驱动 · 无联网回传；CPU 温度使用已有 PawnIO 驱动\n默认不添加开机启动。",
            sample.ramUsedBytes/1073741824.0,sample.ramTotalBytes/1073741824.0,sample.ramPercent,
            gpuMemoryText,pm.WorkingSetSize/1048576.0,pm.PrivateUsage/1048576.0,
            cfg.gpu&&gpuName[0]?gpuName:cfg.gpu?L"无法读取":L"未启用",
            engineValues[0],engineValues[1],engineValues[2],engineValues[3],engineValues[4],
            sample.gpuUtilizationAvailable?sample.gpuEngineType:L"—",
            sample.cpuSampleIntervalMs/1000.0,sample.gpuSampleIntervalMs/1000.0,
            cfg.gpu&&sample.integratedGpu.name[0]?sample.integratedGpu.name:cfg.gpu?L"无法读取":L"未启用",
            integratedValue,cfg.gpu&&sample.integratedGpu.utilizationAvailable?sample.integratedGpu.engineType:L"—",
            sample.integratedGpu.sampleIntervalMs/1000.0);
        MessageBoxW(wnd,text,L"轻量监控",MB_OK|MB_ICONINFORMATION); return;
    }
    case ID_EXIT: BeginManualClose("close_menu_exit"); return;
    default: return;
    }
    PlaceTaskbar(true); UpdateTray(true); WriteConfig();
}
LRESULT CALLBACK PanelProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_PAINT: Paint(h); return 0;
    case WM_PRINTCLIENT: Paint(h,reinterpret_cast<HDC>(wp)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CONTEXTMENU: Menu(); return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_DESTROY: RemovePropW(h,L"LightMon.Group"); if(panel==h) panel=nullptr; if(networkPanel==h) networkPanel=nullptr; return 0;
    }
    return DefWindowProcW(h,msg,wp,lp);
}
LRESULT CALLBACK InputProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    switch(msg) {
    case WM_PAINT: { PAINTSTRUCT ps{}; BeginPaint(h,&ps); EndPaint(h,&ps); return 0; }
    case WM_PRINTCLIENT: case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST: return HTCLIENT;
    case WM_CONTEXTMENU: Menu(); return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_DESTROY: if(inputPanel==h) inputPanel=nullptr; if(networkInputPanel==h) networkInputPanel=nullptr; return 0;
    }
    return DefWindowProcW(h,msg,wp,lp);
}
LRESULT CALLBACK WindowProc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
    if(taskbarCreated&&msg==taskbarCreated) { trayAdded=false; PlaceTaskbar(true); UpdateTray(true); return 0; }
    switch(msg) {
    case WM_SHOW_PANEL: hidden=false; PlaceTaskbar(true); UpdateTray(true); return 0;
    case WM_GAP_READY:
        if(menuRequiredGeneration&&static_cast<std::uint64_t>(wp)<menuRequiredGeneration) return 0;
        { std::lock_guard<std::mutex> lock(gapMutex);
          if(static_cast<std::uint64_t>(wp)!=gapRequestGeneration) return 0; }
        // A committed Unknown result cannot end the fixed post-menu grace.
        if(!closing) { PlaceTaskbar(); UpdateTray(true); if(panel) InvalidateRect(panel,nullptr,FALSE); } return 0;
    case WM_GAP_STOPPED: if(closing) DestroyWindow(h); return 0;
    case WM_DIAGNOSTICS_STOP: if(report) { fclose(report); report=nullptr; } return 0;
    // Read-only diagnostic discovery when the shell omits child HWNDs from
    // enumeration. Callers must independently validate identity and geometry.
    case WM_QUERY_TASKBAR_WINDOW: {
        const HWND windows[]={panel,inputPanel,networkPanel,networkInputPanel};
        return wp<std::size(windows)?reinterpret_cast<LRESULT>(windows[wp]):0;
    }
    case WM_TIMER:
        if(closing) return 0;
        if(wp==MENU_RESUME_TIMER) {
            if(menuGapDeadline&&GetTickCount64()<menuGapDeadline) return 0;
            CancelMenuGrace();
            PlaceTaskbar(true); UpdateTray(true); if(panel&&!hidden) InvalidateRect(panel,nullptr,FALSE);
            return 0;
        }
        sample=sampler->Sample(); PlaceTaskbar(); UpdateTray(); if(panel&&!hidden) InvalidateRect(panel,nullptr,FALSE);
        ++elapsedTicks;
        if(smoke) {
            if(elapsedTicks==3||elapsedTicks==5) Command(ID_SHOW);
            if(elapsedTicks==7||elapsedTicks==9) Command(ID_COMPACT);
            if(elapsedTicks==11||elapsedTicks==13) Command(ID_THEME);
        }
        if(networkPanel&&!hidden) InvalidateRect(networkPanel,nullptr,FALSE);
        Diagnostics();
        if(testing&&(GetTickCount64()-startTick)>=(ULONGLONG)runSeconds*1000) BeginClose("close_test_timeout");
        return 0;
    case WM_TRAY:
        switch(LOWORD(lp)) { case NIN_SELECT: case NIN_KEYSELECT: ToggleShow(); break; case WM_CONTEXTMENU: Menu(); break; } return 0;
    case WM_COMMAND: Command(LOWORD(wp)); return 0;
    case WM_DISPLAYCHANGE: PlaceTaskbar(true); return 0;
    case WM_SETTINGCHANGE: case WM_THEMECHANGED: UpdateTheme(); PlaceTaskbar(true); return 0;
    case WM_QUERYENDSESSION:
        lightmon::lifetime::LogEvent("session_query",static_cast<DWORD>(lp));
        // This is only a query and may be canceled. Do not wait on GPU/storage
        // providers here; BeginClose flushes traffic after a confirmed end.
        return TRUE;
    case WM_ENDSESSION:
        lightmon::lifetime::LogEvent("session_end",wp?1u:0u,static_cast<DWORD>(lp));
        if(wp) BeginClose("close_session_end"); return 0;
    case WM_CLOSE: BeginManualClose("close_wm_close"); return 0;
    case WM_DESTROY: {
        lightmon::lifetime::LogEvent("controller_destroy");
        WriteConfig(); KillTimer(h,1); KillTimer(h,MENU_RESUME_TIMER); if(panel) DestroyWindow(panel); if(inputPanel) DestroyWindow(inputPanel);
        if(networkPanel) DestroyWindow(networkPanel); if(networkInputPanel) DestroyWindow(networkInputPanel);
        NOTIFYICONDATAW ni{sizeof(ni)}; ni.hWnd=h; ni.uID=1; Shell_NotifyIconW(NIM_DELETE,&ni);
        if(trayIcon) DestroyIcon(trayIcon); if(valueFont) DeleteObject(valueFont); if(labelFont) DeleteObject(labelFont);
        PostQuitMessage(0); return 0;
    }
    }
    return DefWindowProcW(h,msg,wp,lp);
}
int WINAPI wWinMain(HINSTANCE inst,HINSTANCE,PWSTR,int) {
    instance=inst; SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc=0; wchar_t** args=CommandLineToArgvW(GetCommandLineW(),&argc);
    bool forceGpu=false,forceDisk=false,forceLite=false,quit=false,alreadyRelayed=false,taskStart=false,restarted=false;
    const wchar_t* reportPath=nullptr;
    for(int i=1;i<argc;++i) {
        if(lightmon::lifetime::IsRelayArgument(args[i])) alreadyRelayed=true;
        if(!wcscmp(args[i],L"--lightmon-task-start")) taskStart=true;
        if(!wcscmp(args[i],L"--lightmon-restarted")) restarted=true;
        if(!wcscmp(args[i],L"--gpu")) forceGpu=true; if(!wcscmp(args[i],L"--disk")) forceDisk=true;
        if(!wcscmp(args[i],L"--lite")) forceLite=true; if(!wcscmp(args[i],L"--quit")) quit=true;
        if(!wcscmp(args[i],L"--diagnostics")&&i+1<argc) reportPath=args[++i];
        if((!wcscmp(args[i],L"--benchmark")||!wcscmp(args[i],L"--smoke"))&&i+1<argc) {
            smoke=!wcscmp(args[i],L"--smoke"); benchmark=!smoke; testing=true; reportPath=args[++i];
        }
        if(!wcscmp(args[i],L"--seconds")&&i+1<argc) runSeconds=std::clamp(_wtoi(args[++i]),5,600);
    }
    // A tool launcher can belong to a job which kills all children when the
    // host exits. Leave that job once, before acquiring the singleton mutex.
    // Test/quit commands retain their original bounded process lifetime. An
    // explicit Task Scheduler launch stays in its task so repeated ensure-run
    // triggers track the monitor instead of an exited relay parent. This does
    // not assume Task Scheduler RestartOnFailure covers every forced exit.
    lightmon::lifetime::Init();
    const auto relay=lightmon::lifetime::MaybeRelay(quit||testing||taskStart,alreadyRelayed);
    if(relay.outcome==lightmon::lifetime::RelayOutcome::Launched) {
        LocalFree(args); return 0;
    }
    HANDLE mutex=CreateMutexW(nullptr,FALSE,L"Local\\LightMon.Native.Panel.v1.Single");
    const DWORD mutexError=GetLastError();
    if(!mutex) {
        lightmon::lifetime::LogEvent("singleton_create_failed",mutexError);
        LocalFree(args); return 1;
    }
    const bool existing=mutexError==ERROR_ALREADY_EXISTS;
    const bool automatic=taskStart||restarted;
    if(!testing) {
        if(quit) automaticRecovery::PauseForCurrentLogon();
        else if(!automatic) automaticRecovery::ClearPause();
    }
    if(quit||existing) {
        HWND other=FindWindowW(CLASS_NAME,nullptr);
        if(other) { if(quit) PostMessageW(other,WM_CLOSE,0,0); else if(!automatic) PostMessageW(other,WM_SHOW_PANEL,0,0); }
        CloseHandle(mutex); LocalFree(args); return existing&&testing?3:0;
    }
    if(automatic && !testing && automaticRecovery::PausedForCurrentLogon()) {
        CloseHandle(mutex); LocalFree(args); return 0;
    }
    lightmon::lifetime::LogStartup("1.1.28",alreadyRelayed);
    if(taskStart) lightmon::lifetime::LogEvent("task_managed_start");
    if(!testing) {
        // Register this singleton for update-driven restart without preserving
        // test arguments or a Task Scheduler/independent relay launch mode.
        constexpr DWORD restartFlags=RESTART_NO_CRASH|RESTART_NO_HANG;
        const HRESULT restart=RegisterApplicationRestart(L"--lightmon-restarted",restartFlags);
        lightmon::lifetime::LogEvent("restart_registration",static_cast<DWORD>(restart),restartFlags);
    }
    ReadConfig(); if(testing) cfg=Config{};
    if(forceLite) { cfg.gpu=false; cfg.disk=false; } else { if(forceGpu) cfg.gpu=true; if(forceDisk) cfg.disk=true; }
    if(reportPath) {
        _wfopen_s(&report,reportPath,L"wb"); if(!report) { lightmon::lifetime::LogEvent("report_open_failed",static_cast<DWORD>(errno)); LocalFree(args); CloseHandle(mutex); return 2; }
        fprintf(report,"seconds,working_set_bytes,private_commit_bytes,cpu_time_100ns,gdi_handles,user_handles,cpu_pct,ram_pct,download_Bps,upload_Bps,gpu_pct,gpu_temp_C,gpu_used_bytes,gpu_total_bytes,disk_pct,cpu_valid,net_valid,gpu_valid,temp_valid,disk_valid,sampled_at_tick_ms,cpu_window_ms,gpu_window_ms,gpu_luid_high,gpu_luid_low,gpu_phys,gpu_engine,gpu_engine_type,gpu_valid_items,gpu_invalid_items,gpu_engine_count,gpu_status,nvml_kernel_pct,nvml_kernel_valid,gpu_pci,gpu_uuid");
        for(const char* key:{"3d","compute","copy","video_encode","video_decode"})
            fprintf(report,",gpu_%s_pct,gpu_%s_valid,gpu_%s_engine_count,gpu_%s_phys,gpu_%s_engine,gpu_%s_support_known,gpu_%s_supported",key,key,key,key,key,key,key);
        fprintf(report,",igpu_pct,igpu_valid,igpu_device_valid,igpu_status,igpu_window_ms,igpu_luid_high,igpu_luid_low,igpu_phys,igpu_engine,igpu_engine_type,igpu_valid_items,igpu_invalid_items,igpu_engine_count,igpu_pci");
        fprintf(report,",cpu_temp_C,cpu_temp_valid,igpu_temp_C,igpu_temp_valid,disk_temp_C,disk_temp_valid,hottest_disk,hottest_disk_sensor,disk_temp_sensor_count,gpu_hotspot_temp_C,gpu_hotspot_temp_valid,gpu_memory_temp_C,gpu_memory_temp_valid");
        fprintf(report,",cpu_temp_status,cpu_temp_error,cpu_temp_raw,cpu_temp_tick_ms,cpu_temp_helper_active");
        fprintf(report,",cpu_peak_pct,cpu_peak_valid,cpu_peak_group,cpu_peak_logical_processor,cpu_logical_processor_count,cpu_peak_valid_items,cpu_peak_invalid_items,cpu_peak_window_ms,cpu_peak_status,cpu_frequency_min_MHz,cpu_frequency_max_MHz,cpu_frequency_mean_MHz,cpu_frequency_valid,cpu_frequency_valid_items,cpu_frequency_invalid_items,cpu_frequency_window_ms,cpu_frequency_status,cpu_reported_frequency_min_MHz,cpu_reported_frequency_max_MHz,cpu_reported_frequency_valid");
        fprintf(report,",daily_date,daily_download_bytes,daily_upload_bytes,daily_valid,daily_store_status,daily_store_error,daily_store_attention,daily_incomplete,daily_overflowed");
        fprintf(report,",native_menu,hold_valid");
        fputc('\n',report);
    }
    MetricsSampler metrics(cfg.gpu,cfg.disk); sampler=&metrics;
    wchar_t trafficFile[MAX_PATH]; wcscpy_s(trafficFile,configFile);
    wchar_t* trafficName=wcsrchr(trafficFile,L'\\');
    if(trafficName) wcscpy_s(trafficName+1,MAX_PATH-(trafficName+1-trafficFile),L"LightMon.traffic");
    sampler->ConfigureDailyTraffic(testing?nullptr:trafficFile); sample=sampler->Sample();
    WNDCLASSW wc{}; wc.lpfnWndProc=WindowProc; wc.hInstance=inst; wc.lpszClassName=CLASS_NAME; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.style=CS_DBLCLKS;
    wc.hIcon=LoadIconW(inst,MAKEINTRESOURCEW(101)); RegisterClassW(&wc);
    wc.lpfnWndProc=PanelProc; wc.lpszClassName=PANEL_CLASS; RegisterClassW(&wc);
    wc.lpfnWndProc=InputProc; wc.lpszClassName=INPUT_CLASS; RegisterClassW(&wc);
    wnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,CLASS_NAME,L"LightMon 轻量监控",WS_POPUP,
        0,0,0,0,nullptr,nullptr,inst,nullptr);
    if(!wnd) { lightmon::lifetime::LogEvent("controller_create_failed",GetLastError()); if(report) fclose(report); CloseHandle(mutex); LocalFree(args); return 1; }
    if(cfg.cpuTemperatureAuto && !testing) sampler->EnableCpuTemperatureAutomatic();
    dpi=GetDpiForWindow(wnd); taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
    UpdateTheme(); PlaceTaskbar(true); UpdateTray(true); WriteConfig();
    startTick=GetTickCount64(); SetTimer(wnd,1,cfg.interval,nullptr);
    lightmon::lifetime::LogEvent("controller_ready");
    MSG msg{}; BOOL messageResult=0;
    while((messageResult=GetMessageW(&msg,nullptr,0,0))>0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    const DWORD messageError=messageResult==-1?GetLastError():ERROR_SUCCESS;
    const int exitCode=messageResult==-1?1:0;
    lightmon::lifetime::LogEvent("message_loop_end",static_cast<DWORD>(messageResult),messageError,static_cast<ULONGLONG>(msg.wParam));
    { std::lock_guard<std::mutex> lock(gapMutex); gapStop=true; gapWake.notify_one(); }
    if(gapThread.joinable()) gapThread.join();
    if(report) fclose(report); CloseHandle(mutex); LocalFree(args);
    lightmon::lifetime::LogEvent("main_return",static_cast<DWORD>(exitCode),messageError); return exitCode;
}
