#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winternl.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <d3dkmthk.h>
#include <dxcore_interface.h>

#include "metrics.h"
#include "cpu_temperature.h"
#include "disk_temperature.h"
#include "gpu_engine_aggregation.h"
#include "cpu_counter_aggregation.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <limits>
#include <cstdio>
#include <cstddef>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")

namespace lightmon {
namespace {

std::uint64_t FileTimeValue(const FILETIME& value) noexcept {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) |
           value.dwLowDateTime;
}

double Percent(double value) noexcept {
    return std::isfinite(value) ? (std::max)(0.0, (std::min)(100.0, value)) : 0.0;
}

template <std::size_t N>
void CopyName(wchar_t (&destination)[N], const wchar_t* source) noexcept {
    std::size_t i = 0;
    for (; i + 1 < N && source[i] != L'\0'; ++i) destination[i] = source[i];
    destination[i] = L'\0';
}

// Absolute system-directory loading excludes the working directory and PATH.
HMODULE LoadSystemDll(const wchar_t* name) noexcept {
    wchar_t path[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return nullptr;
    const std::size_t nameLength = std::wcslen(name);
    if (length + nameLength + 2 > MAX_PATH) return nullptr;
    path[length] = L'\\';
    std::memcpy(path + length + 1, name, (nameLength + 1) * sizeof(wchar_t));
    return LoadLibraryExW(path, nullptr,
                         LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

template <typename T>
T FindProc(HMODULE module, const char* name) noexcept {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}

bool IsPhysicalAdapter(const MIB_IF_ROW2& row) noexcept {
    // Count traffic at physical NICs once. VPN/tunnel/loopback/virtual-switch
    // interfaces are deliberately excluded, even when they report Ethernet.
    return row.InterfaceAndOperStatusFlags.HardwareInterface &&
           !row.InterfaceAndOperStatusFlags.FilterInterface &&
           !row.InterfaceAndOperStatusFlags.EndPointInterface &&
           (row.Type == IF_TYPE_ETHERNET_CSMACD || row.Type == IF_TYPE_IEEE80211);
}

bool IsConnected(const MIB_IF_ROW2& row) noexcept {
    return row.OperStatus == IfOperStatusUp &&
           row.MediaConnectState != MediaConnectStateDisconnected &&
           !row.InterfaceAndOperStatusFlags.NotMediaConnected &&
           !row.InterfaceAndOperStatusFlags.NotAuthenticated;
}

struct IfTableOwner {
    MIB_IF_TABLE2* value = nullptr;
    ~IfTableOwner() { if (value) FreeMibTable(value); }
};

/*
Copyright 1993-2026 NVIDIA Corporation.  All rights reserved.

NOTICE TO USER:

This source code is subject to NVIDIA ownership rights under U.S. and
international Copyright laws.  Users and possessors of this source code
are hereby granted a nonexclusive, royalty-free license to use this code
in individual and commercial software.

NVIDIA MAKES NO REPRESENTATION ABOUT THE SUITABILITY OF THIS SOURCE
CODE FOR ANY PURPOSE.  IT IS PROVIDED "AS IS" WITHOUT EXPRESS OR
IMPLIED WARRANTY OF ANY KIND.  NVIDIA DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOURCE CODE, INCLUDING ALL IMPLIED WARRANTIES OF
MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE.
IN NO EVENT SHALL NVIDIA BE LIABLE FOR ANY SPECIAL, INDIRECT, INCIDENTAL,
OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
OF USE, DATA OR PROFITS,  WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
OR OTHER TORTIOUS ACTION,  ARISING OUT OF OR IN CONNECTION WITH THE USE
OR PERFORMANCE OF THIS SOURCE CODE.

U.S. Government End Users.   This source code is a "commercial item" as
that term is defined at  48 C.F.R. 2.101 (OCT 1995), consisting  of
"commercial computer  software"  and "commercial computer software
documentation" as such terms are  used in 48 C.F.R. 12.212 (SEPT 1995)
and is provided to the U.S. Government only as a commercial end item.
Consistent with 48 C.F.R.12.212 and 48 C.F.R. 227.7202-1 through
227.7202-4 (JUNE 1995), all U.S. Government End Users acquire the
source code with only those rights set forth herein.

Any use of this source code in individual and commercial software must
include, in the user documentation and internal comments to the code,
the above Disclaimer and U.S. Government End Users Notice.

*/
// Minimal, ABI-compatible NVML v1 declarations; no NVIDIA SDK dependency.
// NVML's exported C functions use the platform's C calling convention.
struct NvmlDeviceOpaque;
using NvmlDevice = NvmlDeviceOpaque*;
using NvmlResult = int;
struct NvmlUtilization { unsigned int gpu; unsigned int memory; };
struct NvmlMemory { unsigned long long total; unsigned long long free; unsigned long long used; };
// Exact nvmlPciInfo_t layout used by nvmlDeviceGetPciInfo_v3, from NVIDIA's
// public nvml.h. Do not substitute the differently versioned extended struct.
struct NvmlPciInfo {
    char busIdLegacy[16];
    unsigned int domain, bus, device, pciDeviceId, pciSubSystemId;
    char busId[32];
};
// Public NVIDIA NVML field/thermal/temperature-v1 ABI. These definitions are
// limited to the dynamic read APIs used below; no vendor SDK/runtime is added.
// https://github.com/NVIDIA/go-nvml/blob/main/pkg/nvml/nvml.h
union NvmlValue {
    double doubleValue;
    int signedIntValue;
    unsigned int unsignedIntValue;
    unsigned long unsignedLongValue;
    unsigned long long unsignedLongLongValue;
    signed long long signedLongLongValue;
    unsigned short unsignedShortValue;
};
struct NvmlFieldValue {
    unsigned int fieldId, scopeId;
    long long timestamp, latencyUsec;
    int valueType;
    NvmlResult result;
    NvmlValue value;
};
struct NvmlThermalSensor {
    int controller, defaultMinTemp, defaultMaxTemp, currentTemp, target;
};
struct NvmlThermalSettings {
    unsigned int count;
    NvmlThermalSensor sensors[3];
};
struct NvmlTemperatureV1 {
    unsigned int version;
    int sensorType, temperature;
};
static_assert(sizeof(NvmlPciInfo) == 68 && offsetof(NvmlPciInfo, busId) == 36,
              "NVML PCI v3 ABI");
static_assert(sizeof(NvmlFieldValue) == 40 && offsetof(NvmlFieldValue, value) == 32,
              "NVML field-value ABI");
static_assert(sizeof(NvmlThermalSettings) == 64 &&
              offsetof(NvmlThermalSettings, sensors) == 4, "NVML thermal ABI");
static_assert(sizeof(NvmlTemperatureV1) == 12, "NVML temperature v1 ABI");
using NvmlInit = NvmlResult (__cdecl*)();
using NvmlShutdown = NvmlResult (__cdecl*)();
using NvmlGetCount = NvmlResult (__cdecl*)(unsigned int*);
using NvmlGetDevice = NvmlResult (__cdecl*)(unsigned int, NvmlDevice*);
using NvmlGetUtilization = NvmlResult (__cdecl*)(NvmlDevice, NvmlUtilization*);
using NvmlGetTemperature = NvmlResult (__cdecl*)(NvmlDevice, unsigned int, unsigned int*);
using NvmlGetMemory = NvmlResult (__cdecl*)(NvmlDevice, NvmlMemory*);
using NvmlGetName = NvmlResult (__cdecl*)(NvmlDevice, char*, unsigned int);
using NvmlGetPci = NvmlResult (__cdecl*)(NvmlDevice, NvmlPciInfo*);
using NvmlGetByUuid = NvmlResult (__cdecl*)(const char*, NvmlDevice*);
using NvmlGetFieldValues = NvmlResult (__cdecl*)(NvmlDevice, int, NvmlFieldValue*);
using NvmlGetThermalSettings = NvmlResult (__cdecl*)(NvmlDevice, unsigned int, NvmlThermalSettings*);
using NvmlGetTemperatureV = NvmlResult (__cdecl*)(NvmlDevice, NvmlTemperatureV1*);

bool ReadFieldTemperature(const NvmlFieldValue& field, unsigned int& result) noexcept {
    // nvmlDeviceGetFieldValues may succeed while an individual field failed.
    if (field.fieldId != 82 || field.result != 0) return false;
    double degrees = -1.0;
    switch (field.valueType) {
    case 0: degrees = field.value.doubleValue; break;
    case 1: degrees = field.value.unsignedIntValue; break;
    case 2: degrees = field.value.unsignedLongValue; break;
    case 3: degrees = static_cast<double>(field.value.unsignedLongLongValue); break;
    case 4: degrees = static_cast<double>(field.value.signedLongLongValue); break;
    case 5: degrees = field.value.signedIntValue; break;
    case 6: degrees = field.value.unsignedShortValue; break;
    default: return false;
    }
    if (!std::isfinite(degrees) || degrees < 0.0 || degrees > 200.0) return false;
    result = static_cast<unsigned int>(std::round(degrees));
    return true;
}

// Match an NVML board's PCI location to a Windows adapter LUID. Virtual PCI
// sentinels, nonzero PCI domains, ambiguous matches, and linked multi-board
// adapters cannot be safely bound by this API; leave the percentage unavailable.
GpuSampleStatus ResolveAdapter(const NvmlPciInfo& pci, LUID& luid) noexcept {
    unsigned int domain = 0, bus = 0, device = 0, function = 0;
    int end = 0;
    if (pci.busId[sizeof(pci.busId) - 1] != '\0' ||
        sscanf_s(pci.busId, "%x:%x:%x.%x%n", &domain, &bus, &device, &function, &end) != 4 ||
        end <= 0 || pci.busId[end] != '\0' || domain != 0 || domain != pci.domain ||
        bus != pci.bus || device != pci.device || bus > 255 || device > 31 || function > 7)
        return GpuSampleStatus::AdapterUnmapped;
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) return GpuSampleStatus::AdapterUnmapped;
    auto enumerate = FindProc<decltype(&D3DKMTEnumAdapters)>(gdi, "D3DKMTEnumAdapters");
    auto query = FindProc<decltype(&D3DKMTQueryAdapterInfo)>(gdi, "D3DKMTQueryAdapterInfo");
    auto close = FindProc<decltype(&D3DKMTCloseAdapter)>(gdi, "D3DKMTCloseAdapter");
    if (!enumerate || !query || !close) return GpuSampleStatus::AdapterUnmapped;
    D3DKMT_ENUMADAPTERS list{};
    if (enumerate(&list) < 0) return GpuSampleStatus::AdapterUnmapped;
    unsigned int matches = 0;
    bool singlePhysical = false;
    const ULONG count = (std::min)(list.NumAdapters, static_cast<ULONG>(std::size(list.Adapters)));
    for (ULONG i = 0; i < count; ++i) {
        D3DKMT_ADAPTERADDRESS address{};
        D3DKMT_QUERYADAPTERINFO request{};
        request.hAdapter = list.Adapters[i].hAdapter;
        request.Type = KMTQAITYPE_ADAPTERADDRESS;
        request.pPrivateDriverData = &address;
        request.PrivateDriverDataSize = sizeof(address);
        if (query(&request) >= 0 && address.BusNumber <= 255 && address.DeviceNumber <= 31 &&
            address.FunctionNumber <= 7 && address.BusNumber == bus &&
            address.DeviceNumber == device && address.FunctionNumber == function) {
            ++matches;
            luid = list.Adapters[i].AdapterLuid;
            D3DKMT_PHYSICAL_ADAPTER_COUNT physical{};
            request.Type = KMTQAITYPE_PHYSICALADAPTERCOUNT;
            request.pPrivateDriverData = &physical;
            request.PrivateDriverDataSize = sizeof(physical);
            singlePhysical = query(&request) >= 0 && physical.Count == 1;
        }
        D3DKMT_CLOSEADAPTER closer{};
        closer.hAdapter = list.Adapters[i].hAdapter;
        close(&closer); // Every successful enum handle is closed, including nonmatches.
    }
    if (list.NumAdapters > count || matches != 1) return GpuSampleStatus::AdapterUnmapped;
    return singlePhysical ? GpuSampleStatus::Available : GpuSampleStatus::MultiPhysicalUnsupported;
}

// D3DKMT HybridIntegrated only describes hybrid configurations. DXCore's
// IsIntegrated and IsHardware properties explicitly identify a hardware iGPU
// by the exact Windows LUID, including ordinary desktop configurations.
// Do not guess from vendor/name, shared memory, or adapter enumeration order.
IntegratedGpuSample ResolveIntegratedAdapter(IDXCoreAdapterFactory* factory) noexcept {
    IntegratedGpuSample result{};
    result.status = GpuSampleStatus::AdapterUnmapped;
    if (!factory) return result;
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) return result;
    auto enumerate = FindProc<decltype(&D3DKMTEnumAdapters)>(gdi, "D3DKMTEnumAdapters");
    auto query = FindProc<decltype(&D3DKMTQueryAdapterInfo)>(gdi, "D3DKMTQueryAdapterInfo");
    auto close = FindProc<decltype(&D3DKMTCloseAdapter)>(gdi, "D3DKMTCloseAdapter");
    if (!enumerate || !query || !close) return result;
    D3DKMT_ENUMADAPTERS list{};
    if (enumerate(&list) < 0) return result;
    const ULONG count = (std::min)(list.NumAdapters, static_cast<ULONG>(std::size(list.Adapters)));
    unsigned int matches = 0;
    bool incompleteType = list.NumAdapters > count;
    for (ULONG i = 0; i < count; ++i) {
        const auto& entry = list.Adapters[i];
        D3DKMT_QUERYADAPTERINFO request{};
        request.hAdapter = entry.hAdapter;
        D3DKMT_ADAPTERTYPE type{};
        request.Type = KMTQAITYPE_ADAPTERTYPE;
        request.pPrivateDriverData = &type;
        request.PrivateDriverDataSize = sizeof(type);
        const bool typeRead = query(&request) >= 0;
        if (!typeRead) incompleteType = true;
        if (typeRead && type.RenderSupported && !type.SoftwareDevice &&
            !type.IndirectDisplayDevice && !type.Paravirtualized) {
            IDXCoreAdapter* core = nullptr;
            const HRESULT found = factory->GetAdapterByLuid(entry.AdapterLuid,
                __uuidof(IDXCoreAdapter), reinterpret_cast<void**>(&core));
            bool hardware = false, integrated = false;
            const bool propertiesRead = SUCCEEDED(found) && core && core->IsValid() &&
                core->IsPropertySupported(DXCoreAdapterProperty::IsHardware) &&
                core->IsPropertySupported(DXCoreAdapterProperty::IsIntegrated) &&
                SUCCEEDED(core->GetProperty(DXCoreAdapterProperty::IsHardware, &hardware)) &&
                SUCCEEDED(core->GetProperty(DXCoreAdapterProperty::IsIntegrated, &integrated));
            if (!propertiesRead) incompleteType = true;
            if (propertiesRead && hardware && integrated) {
                ++matches;
                result.deviceAvailable = true;
                result.luidHigh = static_cast<std::uint32_t>(entry.AdapterLuid.HighPart);
                result.luidLow = entry.AdapterLuid.LowPart;
                D3DKMT_PHYSICAL_ADAPTER_COUNT physical{};
                request.Type = KMTQAITYPE_PHYSICALADAPTERCOUNT;
                request.pPrivateDriverData = &physical;
                request.PrivateDriverDataSize = sizeof(physical);
                result.status = query(&request) >= 0 && physical.Count == 1 ?
                    GpuSampleStatus::Available : GpuSampleStatus::MultiPhysicalUnsupported;
                D3DKMT_ADAPTERREGISTRYINFO registry{};
                request.Type = KMTQAITYPE_ADAPTERREGISTRYINFO;
                request.pPrivateDriverData = &registry;
                request.PrivateDriverDataSize = sizeof(registry);
                if (query(&request) >= 0 && registry.AdapterString[MAX_PATH - 1] == L'\0')
                    CopyName(result.name, registry.AdapterString);
                D3DKMT_ADAPTERADDRESS address{};
                request.Type = KMTQAITYPE_ADAPTERADDRESS;
                request.pPrivateDriverData = &address;
                request.PrivateDriverDataSize = sizeof(address);
                if (query(&request) >= 0 && address.BusNumber <= 255 &&
                    address.DeviceNumber <= 31 && address.FunctionNumber <= 7)
                    sprintf_s(result.pciBusId, "00000000:%02x:%02x.%x",
                        address.BusNumber, address.DeviceNumber, address.FunctionNumber);
            }
            if (core) core->Release();
        }
        D3DKMT_CLOSEADAPTER closer{};
        closer.hAdapter = entry.hAdapter;
        close(&closer); // Every successful enum handle is closed, including nonmatches.
    }
    // If a hardware adapter's type is unknown, uniqueness cannot be proven.
    if (incompleteType || matches > 1) {
        result = {};
        result.status = GpuSampleStatus::AdapterUnmapped;
    } else if (matches == 0) {
        result.status = GpuSampleStatus::NoDevice;
    }
    return result;
}

// Windows' public WDDM main GPU sensor is in deci-Celsius. Use the same
// hardware iGPU LUID as its engine counters, never the CPU package or a vendor
// name/adapter-index guess. A short-lived adapter handle cannot survive a
// stale binding; it is closed on both successful and unsuccessful queries.
// https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_adapter_perfdata
bool ReadIntegratedTemperature(const IntegratedGpuSample& identity, int& degrees) noexcept {
    static_assert(sizeof(D3DKMT_ADAPTER_PERFDATA) == 64 &&
        offsetof(D3DKMT_ADAPTER_PERFDATA, Temperature) == 56, "WDDM perf-data x64 ABI");
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) return false;
    auto open = FindProc<decltype(&D3DKMTOpenAdapterFromLuid)>(gdi, "D3DKMTOpenAdapterFromLuid");
    auto query = FindProc<decltype(&D3DKMTQueryAdapterInfo)>(gdi, "D3DKMTQueryAdapterInfo");
    auto close = FindProc<decltype(&D3DKMTCloseAdapter)>(gdi, "D3DKMTCloseAdapter");
    if (!open || !query || !close) return false;
    D3DKMT_OPENADAPTERFROMLUID adapter{};
    adapter.AdapterLuid.HighPart = static_cast<LONG>(identity.luidHigh);
    adapter.AdapterLuid.LowPart = identity.luidLow;
    if (open(&adapter) < 0) return false;
    D3DKMT_ADAPTER_PERFDATA data{};
    data.PhysicalAdapterIndex = 0; // Binding has already verified one physical adapter.
    D3DKMT_QUERYADAPTERINFO request{};
    request.hAdapter = adapter.hAdapter;
    request.Type = KMTQAITYPE_ADAPTERPERFDATA;
    request.pPrivateDriverData = &data;
    request.PrivateDriverDataSize = sizeof(data);
    const NTSTATUS status = query(&request);
    D3DKMT_CLOSEADAPTER closer{};
    closer.hAdapter = adapter.hAdapter;
    close(&closer);
    // Unsupported sensors commonly return zero. Reject sentinels and values
    // outside 1..200 C; never turn a missing sensor into a valid zero reading.
    if (status < 0 || data.Temperature < 10 || data.Temperature > 2000) return false;
    degrees = static_cast<int>((data.Temperature + 5) / 10);
    return true;
}

struct GpuEngineSupport {
    bool known = false;
    std::array<bool, kGpuEngineCategoryCount> supported{};
};

// A complete driver node inventory can distinguish an unsupported category
// from a temporarily absent PDH sample. The public DXGK_ENGINE_TYPE enum has
// no Compute value: driver-defined Compute/Compute_N nodes use OTHER names.
// https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ne-d3dkmdt-dxgk_engine_type
GpuEngineSupport ReadGpuEngineSupport(const LUID& luid) noexcept {
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) return {};
    auto open = FindProc<decltype(&D3DKMTOpenAdapterFromLuid)>(gdi, "D3DKMTOpenAdapterFromLuid");
    auto query = FindProc<decltype(&D3DKMTQueryAdapterInfo)>(gdi, "D3DKMTQueryAdapterInfo");
    auto statistics = FindProc<decltype(&D3DKMTQueryStatistics)>(gdi, "D3DKMTQueryStatistics");
    auto close = FindProc<decltype(&D3DKMTCloseAdapter)>(gdi, "D3DKMTCloseAdapter");
    if (!open || !query || !statistics || !close) return {};
    D3DKMT_OPENADAPTERFROMLUID adapter{};
    adapter.AdapterLuid = luid;
    if (open(&adapter) < 0) return {};
    struct AdapterOwner {
        D3DKMT_HANDLE handle;
        decltype(&D3DKMTCloseAdapter) close;
        ~AdapterOwner() {
            D3DKMT_CLOSEADAPTER request{};
            request.hAdapter = handle;
            close(&request);
        }
    } owner{adapter.hAdapter, close};

    D3DKMT_PHYSICAL_ADAPTER_COUNT physical{};
    D3DKMT_QUERYADAPTERINFO request{};
    request.hAdapter = adapter.hAdapter;
    request.Type = KMTQAITYPE_PHYSICALADAPTERCOUNT;
    request.pPrivateDriverData = &physical;
    request.PrivateDriverDataSize = sizeof(physical);
    if (query(&request) < 0 || physical.Count != 1) return {};
    D3DKMT_QUERYSTATISTICS adapterStatistics{};
    adapterStatistics.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
    adapterStatistics.AdapterLuid = luid;
    if (statistics(&adapterStatistics) < 0) return {};
    const ULONG nodeCount = adapterStatistics.QueryResult.AdapterInformation.NodeCount;
    if (nodeCount == 0 || nodeCount > 256) return {};

    GpuEngineSupport result{};
    for (ULONG node = 0; node < nodeCount; ++node) {
        D3DKMT_NODEMETADATA metadata{};
        // Low word is the node ordinal; high word is physical adapter zero.
        metadata.NodeOrdinalAndAdapterIndex = node;
        request.Type = KMTQAITYPE_NODEMETADATA;
        request.pPrivateDriverData = &metadata;
        request.PrivateDriverDataSize = sizeof(metadata);
        if (query(&request) < 0) return {}; // Never publish a partial inventory.
        GpuEngineCategory category = GpuEngineCategory::Count;
        switch (metadata.NodeData.EngineType) {
        case DXGK_ENGINE_TYPE_3D: category = GpuEngineCategory::ThreeD; break;
        case DXGK_ENGINE_TYPE_COPY: category = GpuEngineCategory::Copy; break;
        case DXGK_ENGINE_TYPE_VIDEO_ENCODE: category = GpuEngineCategory::VideoEncode; break;
        case DXGK_ENGINE_TYPE_VIDEO_DECODE: category = GpuEngineCategory::VideoDecode; break;
        case DXGK_ENGINE_TYPE_OTHER:
            if (!std::wmemchr(metadata.NodeData.FriendlyName, L'\0',
                             std::size(metadata.NodeData.FriendlyName))) return {};
            category = detail::ClassifyGpuEngineType(metadata.NodeData.FriendlyName);
            break;
        case DXGK_ENGINE_TYPE_VIDEO_PROCESSING:
        case DXGK_ENGINE_TYPE_SCENE_ASSEMBLY:
        case DXGK_ENGINE_TYPE_OVERLAY:
        case DXGK_ENGINE_TYPE_CRYPTO:
        case DXGK_ENGINE_TYPE_VIDEO_CODEC: break;
        default: return {}; // A new/invalid enum is not proof of absence.
        }
        if (category != GpuEngineCategory::Count)
            result.supported[static_cast<std::size_t>(category)] = true;
    }
    result.known = true; // All bounded node metadata queries have succeeded.
    return result;
}

void ApplyGpuEngineSupport(
    std::array<GpuEngineCategorySample, kGpuEngineCategoryCount>& samples,
    const GpuEngineSupport& support) noexcept {
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i].supportKnown = support.known;
        samples[i].supported = support.known && support.supported[i];
    }
}

struct GpuProvider {
    HMODULE module = nullptr;
    NvmlShutdown shutdown = nullptr;
    NvmlGetUtilization utilization = nullptr;
    NvmlGetTemperature temperature = nullptr;
    NvmlGetTemperatureV temperatureV = nullptr;
    NvmlGetFieldValues fieldValues = nullptr;
    NvmlGetThermalSettings thermalSettings = nullptr;
    NvmlGetMemory memory = nullptr;
    NvmlGetName name = nullptr;
    NvmlInit init = nullptr;
    NvmlGetCount count = nullptr;
    NvmlGetDevice getDevice = nullptr;
    NvmlGetName getUuid = nullptr;
    NvmlGetByUuid byUuid = nullptr;
    NvmlGetPci getPci = nullptr;
    NvmlDevice device = nullptr;
    bool initialized = false;
    char deviceName[96]{};
    char deviceUuid[96]{};
    NvmlPciInfo pci{};
    LUID adapterLuid{};
    bool mapped = false;
    GpuSampleStatus bindingStatus = GpuSampleStatus::NoDevice;
    HMODULE dxcoreModule = nullptr;
    IDXCoreAdapterFactory* coreFactory = nullptr;
    IntegratedGpuSample integratedIdentity{};
    bool integratedMapped = false;
    GpuEngineSupport discreteEngineSupport{};
    GpuEngineSupport integratedEngineSupport{};
    ULONGLONG lastBindingTick = 0;
    HMODULE pdhModule = nullptr;
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER engines = nullptr;
    decltype(&PdhOpenQueryW) openQuery = nullptr;
    decltype(&PdhAddEnglishCounterW) addCounter = nullptr;
    decltype(&PdhCollectQueryData) collect = nullptr;
    decltype(&PdhGetFormattedCounterArrayW) getArray = nullptr;
    decltype(&PdhCloseQuery) closeQuery = nullptr;
    std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> buffer;
    ULONGLONG lastCollectTick = 0;
    // Auxiliary sensors change slowly; reuse one bounded four-value snapshot.
    // Order: hottest GPU die, memory, GPU power supply, GPU board ambient.
    std::array<unsigned int, 4> auxiliaryTemperatureC{};
    std::array<bool, 4> auxiliaryTemperatureAvailable{};
    ULONGLONG lastAuxiliaryTemperatureTick = 0;
    int integratedTemperatureC = 0;
    bool integratedTemperatureAvailable = false;
    ULONGLONG lastIntegratedTemperatureTick = 0;

    GpuProvider() noexcept {
        module = LoadSystemDll(L"nvml.dll");
        if (!module) {
            // NVIDIA's documented location for standard (non-DCH) drivers.
            wchar_t path[MAX_PATH]{};
            DWORD length = GetEnvironmentVariableW(L"ProgramW6432", path, MAX_PATH);
            if (length == 0) length = GetEnvironmentVariableW(L"ProgramFiles", path, MAX_PATH);
            constexpr wchar_t suffix[] = L"\\NVIDIA Corporation\\NVSMI\\nvml.dll";
            if (length > 2 && length < MAX_PATH && path[1] == L':' &&
                (path[2] == L'\\' || path[2] == L'/') &&
                length + (sizeof(suffix) / sizeof(wchar_t)) <= MAX_PATH) {
                std::memcpy(path + length, suffix, sizeof(suffix));
                module = LoadLibraryExW(path, nullptr,
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            }
        }
        if (module) {
            init = FindProc<NvmlInit>(module, "nvmlInit_v2");
            if (!init) init = FindProc<NvmlInit>(module, "nvmlInit");
            shutdown = FindProc<NvmlShutdown>(module, "nvmlShutdown");
            count = FindProc<NvmlGetCount>(module, "nvmlDeviceGetCount_v2");
            if (!count) count = FindProc<NvmlGetCount>(module, "nvmlDeviceGetCount");
            getDevice = FindProc<NvmlGetDevice>(module, "nvmlDeviceGetHandleByIndex_v2");
            if (!getDevice) getDevice = FindProc<NvmlGetDevice>(module, "nvmlDeviceGetHandleByIndex");
            utilization = FindProc<NvmlGetUtilization>(module, "nvmlDeviceGetUtilizationRates");
            temperature = FindProc<NvmlGetTemperature>(module, "nvmlDeviceGetTemperature");
            temperatureV = FindProc<NvmlGetTemperatureV>(module, "nvmlDeviceGetTemperatureV");
            fieldValues = FindProc<NvmlGetFieldValues>(module, "nvmlDeviceGetFieldValues");
            thermalSettings = FindProc<NvmlGetThermalSettings>(module, "nvmlDeviceGetThermalSettings");
            memory = FindProc<NvmlGetMemory>(module, "nvmlDeviceGetMemoryInfo");
            name = FindProc<NvmlGetName>(module, "nvmlDeviceGetName");
            getUuid = FindProc<NvmlGetName>(module, "nvmlDeviceGetUUID");
            byUuid = FindProc<NvmlGetByUuid>(module, "nvmlDeviceGetHandleByUUID");
            getPci = FindProc<NvmlGetPci>(module, "nvmlDeviceGetPciInfo_v3");
        }
        RefreshBinding(GetTickCount64());
    }

    ~GpuProvider() {
        CloseCounter();
        if (pdhModule) FreeLibrary(pdhModule);
        if (coreFactory) coreFactory->Release();
        if (dxcoreModule) FreeLibrary(dxcoreModule);
        if (initialized && shutdown) shutdown();
        if (module) FreeLibrary(module);
    }

    void CloseCounter() noexcept {
        if (query && closeQuery) closeQuery(query);
        query = nullptr;
        engines = nullptr;
        lastCollectTick = 0;
    }

    void OpenCounter() noexcept {
        CloseCounter();
        if (!mapped && !integratedMapped) return;
        if (!pdhModule) pdhModule = LoadSystemDll(L"pdh.dll");
        if (!pdhModule) return;
        openQuery = FindProc<decltype(openQuery)>(pdhModule, "PdhOpenQueryW");
        addCounter = FindProc<decltype(addCounter)>(pdhModule, "PdhAddEnglishCounterW");
        collect = FindProc<decltype(collect)>(pdhModule, "PdhCollectQueryData");
        getArray = FindProc<decltype(getArray)>(pdhModule, "PdhGetFormattedCounterArrayW");
        closeQuery = FindProc<decltype(closeQuery)>(pdhModule, "PdhCloseQuery");
        if (!openQuery || !addCounter || !collect || !getArray || !closeQuery) return;
        if (openQuery(nullptr, 0, &query) != ERROR_SUCCESS) return;
        if (addCounter(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engines) != ERROR_SUCCESS) {
            CloseCounter();
            return;
        }
        const ULONGLONG now = GetTickCount64();
        if (collect(query) == ERROR_SUCCESS) lastCollectTick = now;
    }

    void RefreshNvidiaBinding() noexcept {
        mapped = false;
        bindingStatus = GpuSampleStatus::NoDevice;
        if (!init || !shutdown || !count || !getDevice || !getUuid || !byUuid || !getPci) {
            device = nullptr; return;
        }
        if (!initialized) initialized = init() == 0;
        if (!initialized) { device = nullptr; return; }
        NvmlDevice next = nullptr;
        unsigned int deviceCount = 0;
        const bool found = deviceUuid[0] ? byUuid(deviceUuid, &next) == 0 :
            count(&deviceCount) == 0 && deviceCount > 0 && getDevice(0, &next) == 0;
        char uuid[96]{};
        if (!found || !next || getUuid(next, uuid, static_cast<unsigned int>(sizeof(uuid))) != 0 ||
            uuid[0] == '\0' || uuid[sizeof(uuid) - 1] != '\0' ||
            (deviceUuid[0] && std::strcmp(uuid, deviceUuid) != 0)) {
            device = nullptr; return;
        }
        if (device != next) {
            lastAuxiliaryTemperatureTick = 0;
            auxiliaryTemperatureC = {};
            auxiliaryTemperatureAvailable = {};
        }
        device = next;
        std::memcpy(deviceUuid, uuid, sizeof(uuid)); // Pin the board across index reorderings.
        if (name) name(device, deviceName, static_cast<unsigned int>(sizeof(deviceName)));
        deviceName[sizeof(deviceName) - 1] = '\0';
        NvmlPciInfo currentPci{};
        bindingStatus = GpuSampleStatus::AdapterUnmapped;
        if (getPci(device, &currentPci) != 0) return;
        pci = currentPci;
        bindingStatus = ResolveAdapter(pci, adapterLuid);
        mapped = bindingStatus == GpuSampleStatus::Available;
    }

    void RefreshBinding(ULONGLONG now) noexcept {
        lastBindingTick = now;
        const LUID oldLuid = adapterLuid;
        const bool wasMapped = mapped;
        const auto oldIntegrated = integratedIdentity;
        const bool wasIntegratedMapped = integratedMapped;
        if (!dxcoreModule) dxcoreModule = LoadSystemDll(L"dxcore.dll");
        if (dxcoreModule && !coreFactory) {
            using CreateFactory = HRESULT (WINAPI*)(REFIID, void**);
            auto create = FindProc<CreateFactory>(dxcoreModule, "DXCoreCreateAdapterFactory");
            if (create) {
                IDXCoreAdapterFactory* next = nullptr;
                const HRESULT status = create(__uuidof(IDXCoreAdapterFactory),
                    reinterpret_cast<void**>(&next));
                if (SUCCEEDED(status)) coreFactory = next;
                else if (next) next->Release();
            }
        }
        integratedIdentity = ResolveIntegratedAdapter(coreFactory);
        integratedMapped = integratedIdentity.status == GpuSampleStatus::Available;
        const bool integratedChanged = wasIntegratedMapped != integratedMapped || (integratedMapped &&
            (oldIntegrated.luidHigh != integratedIdentity.luidHigh ||
             oldIntegrated.luidLow != integratedIdentity.luidLow));
        if (integratedChanged || !integratedMapped) {
            integratedTemperatureC = 0;
            integratedTemperatureAvailable = false;
            lastIntegratedTemperatureTick = 0;
        }
        RefreshNvidiaBinding();
        // Re-read every binding refresh, even when the LUID is unchanged.
        // Failed/remapped bindings and partial topology reads clear old flags.
        discreteEngineSupport = mapped ? ReadGpuEngineSupport(adapterLuid) : GpuEngineSupport{};
        LUID integratedLuid{};
        integratedLuid.HighPart = static_cast<LONG>(integratedIdentity.luidHigh);
        integratedLuid.LowPart = integratedIdentity.luidLow;
        integratedEngineSupport = integratedMapped ?
            ReadGpuEngineSupport(integratedLuid) : GpuEngineSupport{};
        if (!mapped && !integratedMapped) { CloseCounter(); return; }
        const bool nvidiaChanged = wasMapped != mapped || (mapped &&
            (oldLuid.HighPart != adapterLuid.HighPart || oldLuid.LowPart != adapterLuid.LowPart));
        if (nvidiaChanged || integratedChanged || !query || !engines) OpenCounter();
    }

    void SetEngineStatus(MetricsSample& sample, GpuSampleStatus status) noexcept {
        if (mapped) sample.gpuStatus = status;
        if (integratedMapped) sample.integratedGpu.status = status;
    }

    void SampleEngines(MetricsSample& sample, ULONGLONG now) noexcept {
        sample.gpuStatus = bindingStatus;
        ApplyGpuEngineSupport(sample.gpuEngines, discreteEngineSupport);
        ApplyGpuEngineSupport(sample.integratedGpu.engines, integratedEngineSupport);
        if (!mapped && !integratedMapped) return;
        if (mapped) {
            sample.gpuLuidHigh = static_cast<std::uint32_t>(adapterLuid.HighPart);
            sample.gpuLuidLow = adapterLuid.LowPart;
        }
        SetEngineStatus(sample, GpuSampleStatus::CounterUnavailable);
        if (!query || !engines || !collect || !getArray) return;
        SetEngineStatus(sample, GpuSampleStatus::WarmingUp);
        if (lastCollectTick && now - lastCollectTick < 100) return;
        if (lastCollectTick && now - lastCollectTick > 30000) { OpenCounter(); return; }
        const ULONGLONG previous = lastCollectTick;
        if (collect(query) != ERROR_SUCCESS) { CloseCounter(); return; }
        lastCollectTick = now;
        if (!previous) return;
        // Each mapped adapter receives the identical collection-start interval.
        if (mapped) sample.gpuSampleIntervalMs = now - previous;
        if (integratedMapped) sample.integratedGpu.sampleIntervalMs = now - previous;
        SetEngineStatus(sample, GpuSampleStatus::InvalidSample);
        DWORD bytes = 0, itemCount = 0;
        PDH_STATUS status = getArray(engines, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                                    &bytes, &itemCount, nullptr);
        constexpr DWORD maximumBytes = 4 * 1024 * 1024;
        for (unsigned int attempt = 0; attempt < 2 && status == PDH_MORE_DATA; ++attempt) {
            if (bytes == 0 || bytes > maximumBytes) {
                SetEngineStatus(sample, GpuSampleStatus::BufferTooLarge);
                return;
            }
            const std::size_t needed = (bytes + sizeof(buffer[0]) - 1) / sizeof(buffer[0]);
            try { if (buffer.size() < needed) buffer.resize(needed); }
            catch (...) { return; } // Sampling is noexcept; a failed allocation is unavailable.
            bytes = static_cast<DWORD>(buffer.size() * sizeof(buffer[0]));
            status = getArray(engines, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                              &bytes, &itemCount, buffer.data());
            if (status == PDH_MORE_DATA && attempt + 1 < 2) {
                // A nonzero undersized call does not provide a reliable size.
                // Query it again before the next bounded allocation attempt.
                bytes = 0;
                itemCount = 0;
                status = getArray(engines, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                                  &bytes, &itemCount, nullptr);
            }
        }
        if (status != ERROR_SUCCESS || itemCount == 0 || itemCount > buffer.size()) return;
        if (mapped) {
            const auto summary = detail::AggregateGpuEngineUsage(buffer.data(), itemCount,
                sample.gpuLuidHigh, sample.gpuLuidLow);
            sample.gpuEngineValidItems = summary.validItems;
            sample.gpuEngineInvalidItems = summary.invalidItems;
            sample.gpuEngineCount = summary.engineCount;
            if (summary.available) {
                sample.gpuPercent = summary.percent;
                sample.gpuEngines = summary.gpuEngines;
                ApplyGpuEngineSupport(sample.gpuEngines, discreteEngineSupport);
                sample.gpuPhysicalIndex = summary.busiestPhys;
                sample.gpuEngineIndex = summary.busiestEngine;
                std::wmemcpy(sample.gpuEngineType, summary.type, std::size(sample.gpuEngineType));
                sample.gpuUtilizationAvailable = true;
                sample.gpuStatus = GpuSampleStatus::Available;
            }
        }
        if (integratedMapped) {
            auto& integrated = sample.integratedGpu;
            const auto summary = detail::AggregateGpuEngineUsage(buffer.data(), itemCount,
                integrated.luidHigh, integrated.luidLow);
            integrated.validItems = summary.validItems;
            integrated.invalidItems = summary.invalidItems;
            integrated.engineCount = summary.engineCount;
            if (summary.available) {
                integrated.percent = summary.percent;
                integrated.engines = summary.gpuEngines;
                ApplyGpuEngineSupport(integrated.engines, integratedEngineSupport);
                integrated.physicalIndex = summary.busiestPhys;
                integrated.engineIndex = summary.busiestEngine;
                std::wmemcpy(integrated.engineType, summary.type, std::size(integrated.engineType));
                integrated.utilizationAvailable = true;
                integrated.status = GpuSampleStatus::Available;
            }
        }
    }

    void SampleAuxiliaryTemperatures(MetricsSample& sample, ULONGLONG now) noexcept {
        if (!lastAuxiliaryTemperatureTick || now - lastAuxiliaryTemperatureTick >= 5000) {
            lastAuxiliaryTemperatureTick = now;
            auxiliaryTemperatureC = {};
            auxiliaryTemperatureAvailable = {};
            // NVML_TEMPERATURE_GPU_MAX = 1 names the hottest part of the die.
            // It is not a shutdown threshold or an estimate from GPU core temp.
            NvmlTemperatureV1 hottest{};
            hottest.version = sizeof(hottest) | (1u << 24);
            hottest.sensorType = 1;
            if (temperatureV && temperatureV(device, &hottest) == 0 &&
                hottest.temperature >= 0 && hottest.temperature <= 200) {
                auxiliaryTemperatureC[0] = static_cast<unsigned int>(hottest.temperature);
                auxiliaryTemperatureAvailable[0] = true;
            }
            // Target ALL=15 asks the driver for named thermal sensors. NONE=0
            // and UNKNOWN=-1 placeholders do not prove a sensor exists.
            NvmlThermalSettings settings{};
            if (thermalSettings && thermalSettings(device, 15, &settings) == 0 &&
                settings.count <= std::size(settings.sensors)) {
                for (unsigned int i = 0; i < settings.count; ++i) {
                    const auto& sensor = settings.sensors[i];
                    if (sensor.controller <= 0 || sensor.currentTemp < 0 || sensor.currentTemp > 200)
                        continue;
                    std::size_t target = auxiliaryTemperatureC.size();
                    if (sensor.target == 2) target = 1; // GPU memory.
                    else if (sensor.target == 4) target = 2; // GPU power supply.
                    else if (sensor.target == 8) target = 3; // GPU board ambient.
                    if (target < auxiliaryTemperatureC.size()) {
                        auxiliaryTemperatureC[target] = static_cast<unsigned int>(sensor.currentTemp);
                        auxiliaryTemperatureAvailable[target] = true;
                    }
                }
            }
            NvmlFieldValue memoryTemperature{};
            memoryTemperature.fieldId = 82; // NVML_FI_DEV_MEMORY_TEMP, degrees C.
            unsigned int memoryDegrees = 0;
            if (fieldValues && fieldValues(device, 1, &memoryTemperature) == 0 &&
                ReadFieldTemperature(memoryTemperature, memoryDegrees)) {
                auxiliaryTemperatureC[1] = memoryDegrees;
                auxiliaryTemperatureAvailable[1] = true;
            }
        }
        sample.gpuHotspotTemperatureC = auxiliaryTemperatureC[0];
        sample.gpuMemoryTemperatureC = auxiliaryTemperatureC[1];
        sample.gpuPowerTemperatureC = auxiliaryTemperatureC[2];
        sample.gpuBoardTemperatureC = auxiliaryTemperatureC[3];
        sample.gpuHotspotTemperatureAvailable = auxiliaryTemperatureAvailable[0];
        sample.gpuMemoryTemperatureAvailable = auxiliaryTemperatureAvailable[1];
        sample.gpuPowerTemperatureAvailable = auxiliaryTemperatureAvailable[2];
        sample.gpuBoardTemperatureAvailable = auxiliaryTemperatureAvailable[3];
        sample.gpuAuxTemperatureSampledAtTickMs = lastAuxiliaryTemperatureTick;
    }

    void Sample(MetricsSample& sample) noexcept {
        ULONGLONG now = GetTickCount64();
        const ULONGLONG retryMs = (mapped || integratedMapped) && query && engines ? 30000 : 10000;
        if (now - lastBindingTick >= retryMs) {
            RefreshBinding(now);
            // RefreshBinding may prime a new counter after the original tick.
            now = GetTickCount64();
        }
        sample.gpuStatus = bindingStatus;
        sample.integratedGpu = integratedIdentity;
        if (integratedMapped) {
            if (!lastIntegratedTemperatureTick || now - lastIntegratedTemperatureTick >= 5000) {
                lastIntegratedTemperatureTick = now;
                integratedTemperatureC = 0;
                integratedTemperatureAvailable =
                    ReadIntegratedTemperature(integratedIdentity, integratedTemperatureC);
            }
            sample.integratedGpuTemperatureC = integratedTemperatureC;
            sample.integratedGpuTemperatureAvailable = integratedTemperatureAvailable;
        }
        SampleEngines(sample, now);
        if (!device) return;
        sample.gpuAvailable = true;
        std::memcpy(sample.gpuName, deviceName, sizeof(deviceName));
        std::memcpy(sample.gpuUuid, deviceUuid, sizeof(deviceUuid));
        std::memcpy(sample.gpuPciBusId, pci.busId, sizeof(pci.busId));
        sample.gpuPciBusId[sizeof(sample.gpuPciBusId) - 1] = '\0';
        NvmlUtilization value{};
        if (utilization && utilization(device, &value) == 0 && value.gpu <= 100) {
            sample.nvmlKernelPercent = value.gpu;
            sample.nvmlKernelAvailable = true;
        }
        unsigned int degrees = 0;
        // NVML_TEMPERATURE_GPU = 0 (GPU die sensor).
        if (temperature && temperature(device, 0, &degrees) == 0 && degrees <= 200) {
            sample.gpuTemperatureC = degrees;
            sample.gpuTemperatureAvailable = true;
        }
        SampleAuxiliaryTemperatures(sample, now);
        NvmlMemory bytes{};
        if (memory && memory(device, &bytes) == 0 && bytes.total != 0 && bytes.used <= bytes.total) {
            sample.gpuTotalBytes = bytes.total;
            sample.gpuUsedBytes = bytes.used;
            sample.gpuMemoryAvailable = true;
        }
    }
};

// CPU counters are always available to the sampler, including CPU-only mode.
// One native PDH query owns all bounded per-logical-processor snapshots; no
// WMI process, managed runtime, registry frequency, or hardware library.
struct CpuCounterProvider {
    HMODULE module = nullptr;
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER busy = nullptr;
    PDH_HCOUNTER frequency = nullptr;
    PDH_HCOUNTER reportedFrequency = nullptr;
    decltype(&PdhOpenQueryW) openQuery = nullptr;
    decltype(&PdhAddEnglishCounterW) addCounter = nullptr;
    decltype(&PdhCollectQueryData) collect = nullptr;
    decltype(&PdhGetFormattedCounterArrayW) getArray = nullptr;
    decltype(&PdhCloseQuery) closeQuery = nullptr;
    std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> buffer;
    detail::CpuTopology topology{};
    ULONGLONG lastCollectTick = 0;
    ULONGLONG lastOpenTick = 0;

    CpuCounterProvider() noexcept {
        module = LoadSystemDll(L"pdh.dll");
        if (!module) return;
        openQuery = FindProc<decltype(openQuery)>(module, "PdhOpenQueryW");
        addCounter = FindProc<decltype(addCounter)>(module, "PdhAddEnglishCounterW");
        collect = FindProc<decltype(collect)>(module, "PdhCollectQueryData");
        getArray = FindProc<decltype(getArray)>(module, "PdhGetFormattedCounterArrayW");
        closeQuery = FindProc<decltype(closeQuery)>(module, "PdhCloseQuery");
        if (!openQuery || !addCounter || !collect || !getArray || !closeQuery) return;
        Open(GetTickCount64());
    }

    ~CpuCounterProvider() {
        Close();
        if (module) FreeLibrary(module);
    }

    void Close() noexcept {
        if (query && closeQuery) closeQuery(query);
        query = nullptr;
        busy = nullptr;
        frequency = nullptr;
        reportedFrequency = nullptr;
        lastCollectTick = 0;
    }

    static detail::CpuTopology ReadTopology() noexcept {
        detail::CpuTopology result{};
        result.groupCount = GetActiveProcessorGroupCount();
        if (result.groupCount == 0 || result.groupCount > result.logicalCounts.size()) return result;
        for (WORD group = 0; group < result.groupCount; ++group) {
            const DWORD count = GetActiveProcessorCount(group);
            if (count == 0 || count > 64) return result;
            result.logicalCounts[group] = count;
            result.logicalCount += count;
        }
        result.available = result.logicalCount != 0;
        return result;
    }

    void Open(ULONGLONG now) noexcept {
        Close();
        lastOpenTick = now;
        topology = ReadTopology();
        if (!topology.available || !openQuery || !addCounter || !collect || !getArray ||
            !closeQuery || openQuery(nullptr, 0, &query) != ERROR_SUCCESS) return;
        if (addCounter(query, L"\\Processor Information(*)\\% Processor Time", 0, &busy) !=
            ERROR_SUCCESS) busy = nullptr;
        if (addCounter(query, L"\\Processor Information(*)\\Actual Frequency", 0, &frequency) !=
            ERROR_SUCCESS) frequency = nullptr;
        if (addCounter(query, L"\\Processor Information(*)\\Processor Frequency", 0, &reportedFrequency) !=
            ERROR_SUCCESS) reportedFrequency = nullptr;
        if (!busy && !frequency && !reportedFrequency) { Close(); return; }
        if (collect(query) == ERROR_SUCCESS) lastCollectTick = now;
        else Close();
    }

    CpuCounterStatus ReadArray(PDH_HCOUNTER counter, DWORD& count) noexcept {
        count = 0;
        DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(buffer[0]));
        PDH_STATUS status = getArray(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
            &bytes, &count, buffer.empty() ? nullptr : buffer.data());
        constexpr DWORD maximumBytes = 512 * 1024;
        for (unsigned attempt = 0; status == PDH_MORE_DATA && attempt < 2; ++attempt) {
            // For a nonzero undersized call the returned bytes is unreliable.
            // Probe again; storage is only enlarged when the provider grows.
            bytes = 0;
            count = 0;
            status = getArray(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                &bytes, &count, nullptr);
            if (status != PDH_MORE_DATA) return CpuCounterStatus::InvalidSample;
            if (bytes == 0 || bytes > maximumBytes) return CpuCounterStatus::BufferTooLarge;
            const std::size_t needed = (bytes + sizeof(buffer[0]) - 1) / sizeof(buffer[0]);
            try { if (buffer.size() < needed) buffer.resize(needed); }
            catch (...) { return CpuCounterStatus::CounterUnavailable; }
            bytes = static_cast<DWORD>(buffer.size() * sizeof(buffer[0]));
            status = getArray(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
                &bytes, &count, buffer.data());
        }
        if (status != ERROR_SUCCESS || count == 0 || count > buffer.size() ||
            static_cast<std::size_t>(count) * sizeof(buffer[0]) > bytes)
            return CpuCounterStatus::InvalidSample;
        return CpuCounterStatus::Available;
    }

    void Sample(MetricsSample& sample) noexcept {
        const ULONGLONG now = GetTickCount64();
        if (!query && (!lastOpenTick || now - lastOpenTick >= 10000)) Open(now);
        sample.cpuLogicalProcessorCount = topology.available ? topology.logicalCount : 0;
        if (!query || (!busy && !frequency && !reportedFrequency) || !collect || !getArray) return;
        if (busy) sample.cpuPeakStatus = CpuCounterStatus::WarmingUp;
        if (frequency) sample.cpuFrequencyStatus = CpuCounterStatus::WarmingUp;
        if (lastCollectTick && now - lastCollectTick < 100) return;
        const auto currentTopology = ReadTopology();
        if (!currentTopology.available || currentTopology.groupCount != topology.groupCount ||
            currentTopology.logicalCounts != topology.logicalCounts ||
            (lastCollectTick && now - lastCollectTick > 30000)) {
            // A changed active-processor inventory or a sleep-sized gap needs
            // a fresh baseline; do not publish a stale/partial maximum.
            Open(now);
            sample.cpuLogicalProcessorCount = topology.available ? topology.logicalCount : 0;
            if (!busy) sample.cpuPeakStatus = CpuCounterStatus::CounterUnavailable;
            if (!frequency) sample.cpuFrequencyStatus = CpuCounterStatus::CounterUnavailable;
            return;
        }
        const ULONGLONG previous = lastCollectTick;
        if (collect(query) != ERROR_SUCCESS) {
            sample.cpuPeakStatus = CpuCounterStatus::CounterUnavailable;
            sample.cpuFrequencyStatus = CpuCounterStatus::CounterUnavailable;
            Close();
            lastOpenTick = now;
            return;
        }
        lastCollectTick = now;
        if (!previous) return;
        if (busy) {
            sample.cpuPeakSampleIntervalMs = now - previous;
            DWORD count = 0;
            sample.cpuPeakStatus = ReadArray(busy, count);
            if (sample.cpuPeakStatus == CpuCounterStatus::Available) {
                const auto summary = detail::AggregateCpuCounters(buffer.data(), count, topology, false);
                sample.cpuPeakValidItems = summary.validItems;
                sample.cpuPeakInvalidItems = summary.invalidItems;
                sample.cpuPeakStatus = summary.available ? CpuCounterStatus::Available :
                    summary.invalidItems ? CpuCounterStatus::InvalidSample : CpuCounterStatus::IncompleteSample;
                if (summary.available) {
                    sample.cpuPeakPercent = summary.maximum;
                    sample.cpuPeakGroup = summary.peakGroup;
                    sample.cpuPeakLogicalProcessor = summary.peakLogicalProcessor;
                    sample.cpuPeakAvailable = true;
                }
            }
        }
        if (frequency) {
            sample.cpuFrequencySampleIntervalMs = now - previous;
            DWORD count = 0;
            sample.cpuFrequencyStatus = ReadArray(frequency, count);
            if (sample.cpuFrequencyStatus == CpuCounterStatus::Available) {
                const auto summary = detail::AggregateCpuCounters(buffer.data(), count, topology, true);
                sample.cpuFrequencyValidItems = summary.validItems;
                sample.cpuFrequencyInvalidItems = summary.invalidItems;
                sample.cpuFrequencyStatus = summary.available ? CpuCounterStatus::Available :
                    summary.invalidItems ? CpuCounterStatus::InvalidSample : CpuCounterStatus::IncompleteSample;
                if (summary.available) {
                    sample.cpuFrequencyMinMHz = summary.minimum;
                    sample.cpuFrequencyMaxMHz = summary.maximum;
                    sample.cpuFrequencyMeanMHz = summary.mean;
                    sample.cpuFrequencyAvailable = true;
                }
            }
        }
        if (reportedFrequency) {
            DWORD count = 0;
            if (ReadArray(reportedFrequency, count) == CpuCounterStatus::Available) {
                const auto summary = detail::AggregateCpuCounters(buffer.data(), count, topology, true);
                if (summary.available) {
                    sample.cpuReportedFrequencyMinMHz = summary.minimum;
                    sample.cpuReportedFrequencyMaxMHz = summary.maximum;
                    sample.cpuReportedFrequencyAvailable = true;
                }
            }
        }
    }
};

struct DiskProvider {
    HMODULE module = nullptr;
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER idle = nullptr;
    decltype(&PdhOpenQueryW) openQuery = nullptr;
    decltype(&PdhAddEnglishCounterW) addCounter = nullptr;
    decltype(&PdhCollectQueryData) collect = nullptr;
    decltype(&PdhGetFormattedCounterArrayW) getArray = nullptr;
    decltype(&PdhCloseQuery) closeQuery = nullptr;
    // Reused and aligned for the item array; optional provider owns this memory.
    alignas(PDH_FMT_COUNTERVALUE_ITEM_W) std::array<unsigned char, 32768> buffer{};

    DiskProvider() noexcept {
        module = LoadSystemDll(L"pdh.dll");
        if (!module) return;
        openQuery = FindProc<decltype(openQuery)>(module, "PdhOpenQueryW");
        addCounter = FindProc<decltype(addCounter)>(module, "PdhAddEnglishCounterW");
        collect = FindProc<decltype(collect)>(module, "PdhCollectQueryData");
        getArray = FindProc<decltype(getArray)>(module, "PdhGetFormattedCounterArrayW");
        closeQuery = FindProc<decltype(closeQuery)>(module, "PdhCloseQuery");
        if (!openQuery || !addCounter || !collect || !getArray || !closeQuery) return;
        if (openQuery(nullptr, 0, &query) != ERROR_SUCCESS) return;
        if (addCounter(query, L"\\PhysicalDisk(*)\\% Idle Time", 0, &idle) != ERROR_SUCCESS) {
            idle = nullptr;
            return;
        }
        collect(query); // First measurement primes two-sample counters.
    }

    ~DiskProvider() {
        if (query && closeQuery) closeQuery(query); // Also closes the counter.
        if (module) FreeLibrary(module);
    }

    void Sample(MetricsSample& sample) noexcept {
        if (!query || !idle || collect(query) != ERROR_SUCCESS) return;
        DWORD bytes = static_cast<DWORD>(buffer.size());
        DWORD count = 0;
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (getArray(idle, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items) != ERROR_SUCCESS) return;
        // Use the busiest individual disk. % Disk Time can exceed 100 with
        // concurrent requests, whereas 100 - % Idle Time describes active time.
        for (DWORD i = 0; i < count; ++i) {
            const auto& item = items[i];
            if (!item.szName || std::wcscmp(item.szName, L"_Total") == 0) continue;
            if (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
                item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
            const double value = item.FmtValue.doubleValue;
            if (!std::isfinite(value)) continue;
            sample.diskBusyPercent = (std::max)(sample.diskBusyPercent, Percent(100.0 - value));
            sample.diskAvailable = true;
        }
    }
};

} // namespace

struct MetricsSampler::Impl {
    struct Adapter {
        NET_LUID luid{};
        GUID guid{};
        std::uint64_t inBytes = 0;
        std::uint64_t outBytes = 0;
        LONGLONG timestamp = 0;
        bool baseline = false;
    };

    std::array<Adapter, 64> adapters{};
    std::size_t adapterCount = 0;
    bool interfaceCapacityExceeded = false;
    DailyTrafficCounter dailyTraffic;
    DailyTrafficStore dailyStore;
    bool dailyIncomplete = false;
    LONGLONG qpcFrequency = 0;
    LONGLONG lastInterfaceRefresh = 0;
    std::uint64_t lastIdle = 0;
    std::uint64_t lastKernel = 0;
    std::uint64_t lastUser = 0;
    ULONGLONG lastCpuTick = 0;
    bool cpuBaseline = false;
    CpuCounterProvider cpuCounters;
    std::unique_ptr<GpuProvider> gpu;
    std::unique_ptr<DiskProvider> disk;
    std::unique_ptr<DiskTemperatureProvider> diskTemperature;
    CpuTemperatureProvider cpuTemperature;

    Impl() noexcept {
        LARGE_INTEGER frequency{};
        if (QueryPerformanceFrequency(&frequency)) qpcFrequency = frequency.QuadPart;
        FILETIME idle{}, kernel{}, user{};
        if (GetSystemTimes(&idle, &kernel, &user)) {
            lastIdle = FileTimeValue(idle);
            lastKernel = FileTimeValue(kernel);
            lastUser = FileTimeValue(user);
            cpuBaseline = true;
            lastCpuTick = GetTickCount64();
        }
        LARGE_INTEGER now{};
        if (qpcFrequency > 0 && QueryPerformanceCounter(&now)) RefreshInterfaces(now.QuadPart);
    }

    ~Impl() { dailyStore.Save(dailyTraffic.Totals(), true); }

    void RefreshInterfaces(LONGLONG now) noexcept {
        lastInterfaceRefresh = now;
        IfTableOwner table;
        if (GetIfTable2(&table.value) != NO_ERROR || !table.value) return;
        std::array<Adapter, 64> refreshed{};
        std::size_t refreshedCount = 0;
        interfaceCapacityExceeded = false;
        for (ULONG i = 0; i < table.value->NumEntries; ++i) {
            const MIB_IF_ROW2& row = table.value->Table[i];
            if (!IsPhysicalAdapter(row)) continue;
            if(refreshedCount == refreshed.size()) { interfaceCapacityExceeded = true; break; }
            Adapter next{};
            next.luid = row.InterfaceLuid;
            next.guid = row.InterfaceGuid;
            for (std::size_t old = 0; old < adapterCount; ++old) {
                if (adapters[old].luid.Value == next.luid.Value) {
                    next = adapters[old];
                    if(memcmp(&next.guid,&row.InterfaceGuid,sizeof(GUID))) {
                        // A replaced device must not import its lifetime bytes.
                        dailyTraffic.Restore(dailyTraffic.Totals());
                        dailyIncomplete = true;
                        next.baseline = false;
                        next.guid = row.InterfaceGuid;
                    }
                    break;
                }
            }
            if (!next.baseline && IsConnected(row)) {
                next.inBytes = row.InOctets;
                next.outBytes = row.OutOctets;
                next.timestamp = now;
                next.baseline = true;
            }
            refreshed[refreshedCount++] = next;
        }
        adapters = refreshed;
        adapterCount = refreshedCount;
    }

    void SampleCpu(MetricsSample& sample) noexcept {
        const ULONGLONG now = GetTickCount64();
        // Keep the constructor baseline through immediate startup samples.
        if (cpuBaseline && now - lastCpuTick < 100) return;
        FILETIME idle{}, kernel{}, user{};
        if (!GetSystemTimes(&idle, &kernel, &user)) {
            cpuBaseline = false;
            return;
        }
        const auto currentIdle = FileTimeValue(idle);
        const auto currentKernel = FileTimeValue(kernel);
        const auto currentUser = FileTimeValue(user);
        if (cpuBaseline && now - lastCpuTick <= 30000 &&
            currentIdle >= lastIdle && currentKernel >= lastKernel && currentUser >= lastUser) {
            const auto idleDelta = currentIdle - lastIdle;
            const auto kernelDelta = currentKernel - lastKernel;
            const auto userDelta = currentUser - lastUser;
            // Kernel includes idle. Convert to double before adding to avoid
            // integer overflow; the common 100 ns unit cancels in the ratio.
            const double total = static_cast<double>(kernelDelta) + static_cast<double>(userDelta);
            if (total > 0.0 && static_cast<double>(idleDelta) <= total) {
                sample.cpuPercent = Percent((total - static_cast<double>(idleDelta)) * 100.0 / total);
                sample.cpuAvailable = true;
                sample.cpuSampleIntervalMs = now - lastCpuTick;
            }
        }
        lastIdle = currentIdle;
        lastKernel = currentKernel;
        lastUser = currentUser;
        lastCpuTick = now;
        cpuBaseline = true;
    }

    void SampleNetwork(MetricsSample& sample) noexcept {
        SYSTEMTIME local{}; GetLocalTime(&local);
        const std::uint32_t date = local.wYear * 10000u + local.wMonth * 100u + local.wDay;
        if(date != dailyTraffic.Totals().date) dailyIncomplete = false;
        LARGE_INTEGER now{};
        const bool clockAvailable = qpcFrequency > 0 && QueryPerformanceCounter(&now);
        if (!clockAvailable || now.QuadPart < lastInterfaceRefresh ||
            static_cast<double>(now.QuadPart - lastInterfaceRefresh) / qpcFrequency >= 10.0) {
            RefreshInterfaces(now.QuadPart);
        }
        dailyTraffic.BeginSample(date);
        // A truncated NIC inventory cannot establish a complete byte delta.
        // Preserve earlier totals, and rebaseline before the next full sample.
        if(interfaceCapacityExceeded) dailyTraffic.BeginSample(0);
        for (std::size_t i = 0; i < adapterCount; ++i) {
            auto& adapter = adapters[i];
            MIB_IF_ROW2 row{};
            row.InterfaceLuid = adapter.luid;
            if (GetIfEntry2(&row) != NO_ERROR || !IsPhysicalAdapter(row)) {
                adapter.baseline = false;
                dailyIncomplete = true;
                continue;
            }
            if(memcmp(&adapter.guid,&row.InterfaceGuid,sizeof(GUID))) {
                dailyTraffic.Restore(dailyTraffic.Totals());
                dailyTraffic.BeginSample(date);
                dailyIncomplete = true;
                adapter.guid = row.InterfaceGuid;
                adapter.baseline = false;
            }
            // Daily bytes do not depend on connection status, the speed clock,
            // or the 30-second maximum used for an instantaneous rate.
            dailyTraffic.Observe(row.InterfaceLuid.Value,row.InOctets,row.OutOctets);
            if(!clockAvailable || !IsConnected(row)) { adapter.baseline = false; continue; }
            if (sample.activeAdapterCount++ == 0) CopyName(sample.firstAdapterName, row.Alias);
            const double seconds = static_cast<double>(now.QuadPart - adapter.timestamp) / qpcFrequency;
            if (adapter.baseline && seconds > 0.0 && seconds <= 30.0 &&
                row.InOctets >= adapter.inBytes && row.OutOctets >= adapter.outBytes) {
                sample.downloadBytesPerSec += static_cast<double>(row.InOctets - adapter.inBytes) / seconds;
                sample.uploadBytesPerSec += static_cast<double>(row.OutOctets - adapter.outBytes) / seconds;
                sample.networkAvailable = true;
            }
            // Reset/reconnect/sleep creates a fresh baseline instead of an
            // unsigned subtraction spike or an average spanning suspension.
            adapter.inBytes = row.InOctets;
            adapter.outBytes = row.OutOctets;
            adapter.timestamp = now.QuadPart;
            adapter.baseline = true;
        }
        dailyTraffic.EndSample();
        dailyIncomplete = dailyIncomplete || interfaceCapacityExceeded || dailyTraffic.CapacityExceeded();
        dailyStore.Save(dailyTraffic.Totals());
        sample.dailyTraffic = dailyTraffic.Totals();
        sample.dailyTrafficAvailable = date != 0;
        sample.dailyTrafficOverflowed = dailyTraffic.Overflowed();
        sample.dailyTrafficIncomplete = dailyIncomplete;
        sample.dailyTrafficStoreStatus = dailyStore.Status();
        sample.dailyTrafficStoreError = dailyStore.Error();
        sample.dailyTrafficStoreAttention = dailyStore.NeedsAttention();
    }
};

MetricsSampler::MetricsSampler(bool enableGpu, bool enableDisk)
    : impl_(std::make_unique<Impl>()) {
    Configure(enableGpu, enableDisk);
}

MetricsSampler::~MetricsSampler() = default;

void MetricsSampler::ConfigureDailyTraffic(const wchar_t* path) noexcept {
    DailyTrafficTotals loaded{};
    impl_->dailyStore.Configure(path, loaded);
    impl_->dailyTraffic.Restore(loaded);
}

void MetricsSampler::FlushDailyTraffic() noexcept {
    impl_->dailyStore.Save(impl_->dailyTraffic.Totals(), true);
}

bool MetricsSampler::EnableCpuTemperature(std::uintptr_t owner) noexcept {
    return impl_->cpuTemperature.Start(reinterpret_cast<HWND>(owner));
}
bool MetricsSampler::EnableCpuTemperatureAutomatic() noexcept {
    return impl_->cpuTemperature.StartAutomatic(nullptr);
}
void MetricsSampler::DisableCpuTemperature() noexcept { impl_->cpuTemperature.Stop(); }
bool MetricsSampler::CpuTemperatureHelperActive() const noexcept { return impl_->cpuTemperature.Active(); }

void MetricsSampler::Configure(bool enableGpu, bool enableDisk) {
    if (!enableGpu) impl_->gpu.reset();
    else if (!impl_->gpu) impl_->gpu = std::make_unique<GpuProvider>();
    if (!enableDisk) { impl_->disk.reset(); impl_->diskTemperature.reset(); }
    else {
        if (!impl_->disk) impl_->disk = std::make_unique<DiskProvider>();
        if (!impl_->diskTemperature) impl_->diskTemperature = std::make_unique<DiskTemperatureProvider>();
    }
}

MetricsSample MetricsSampler::Sample() noexcept {
    MetricsSample sample{};
    sample.sampledAtTickMs = GetTickCount64();
    impl_->SampleCpu(sample);
    impl_->cpuCounters.Sample(sample);
    impl_->cpuTemperature.Sample(sample);
    sample.cpuTemperatureStatus = impl_->cpuTemperature.Status();
    sample.cpuTemperatureError = static_cast<std::int32_t>(impl_->cpuTemperature.LastError());
    sample.cpuTemperatureRaw = impl_->cpuTemperature.Raw();
    sample.cpuTemperatureSampledAtTickMs = impl_->cpuTemperature.LastTick();
    sample.cpuTemperatureHelperActive = impl_->cpuTemperature.Active();
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory) && memory.ullTotalPhys > 0 && memory.ullAvailPhys <= memory.ullTotalPhys) {
        sample.ramTotalBytes = memory.ullTotalPhys;
        sample.ramUsedBytes = memory.ullTotalPhys - memory.ullAvailPhys;
        sample.ramPercent = Percent(static_cast<double>(sample.ramUsedBytes) * 100.0 /
                                    static_cast<double>(sample.ramTotalBytes));
        sample.ramAvailable = true;
    }
    impl_->SampleNetwork(sample);
    if (impl_->gpu) impl_->gpu->Sample(sample);
    if (impl_->disk) impl_->disk->Sample(sample);
    if (impl_->diskTemperature) impl_->diskTemperature->Sample(sample);
    return sample;
}

} // namespace lightmon
