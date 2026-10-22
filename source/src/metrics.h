#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include "cpu_temperature_protocol.h"
#include "daily_traffic_counter.h"
#include "daily_traffic_store.h"

namespace lightmon {

enum class CpuCounterStatus : std::uint32_t {
    CounterUnavailable, WarmingUp, InvalidSample, IncompleteSample,
    Available, BufferTooLarge
};

enum class GpuSampleStatus : std::uint32_t {
    Disabled, NoDevice, AdapterUnmapped, CounterUnavailable, WarmingUp,
    InvalidSample, Available, MultiPhysicalUnsupported, BufferTooLarge
};

enum class GpuEngineCategory : std::uint32_t {
    ThreeD, Compute, Copy, VideoEncode, VideoDecode, Count
};
constexpr std::size_t kGpuEngineCategoryCount =
    static_cast<std::size_t>(GpuEngineCategory::Count);

// Peak physical engine within this category, after summing its PID counters.
// A present idle engine is available with zero percent. Absence of a PDH
// instance alone does not establish whether a category is supported. The
// separate support flags come from a complete, exact-LUID node inventory.
// All categories share gpuSampleIntervalMs and the adapter LUID.
struct GpuEngineCategorySample {
    bool available = false;
    bool supportKnown = false;
    bool supported = false;
    double percent = 0.0;
    std::uint32_t engineCount = 0;
    std::uint32_t physicalIndex = 0;
    std::uint32_t engineIndex = 0;
};

// An explicitly identified hardware iGPU, separate from the NVIDIA board.
// Both adapters use the same Windows GPU Engine collection and interval.
// No temperature or memory sensor is inferred for the integrated adapter.
struct IntegratedGpuSample {
    double percent = 0.0;
    std::array<GpuEngineCategorySample, kGpuEngineCategoryCount> engines{};
    wchar_t name[128]{};
    char pciBusId[32]{};
    wchar_t engineType[48]{};
    std::uint32_t luidHigh = 0;
    std::uint32_t luidLow = 0;
    std::uint32_t physicalIndex = 0;
    std::uint32_t engineIndex = 0;
    std::uint32_t validItems = 0;
    std::uint32_t invalidItems = 0;
    std::uint32_t engineCount = 0;
    std::uint64_t sampleIntervalMs = 0;
    GpuSampleStatus status = GpuSampleStatus::Disabled;
    bool deviceAvailable = false;
    bool utilizationAvailable = false;
};

// Values are a snapshot. Always check the corresponding availability flag:
// a missing sensor or an initial rate baseline must not be displayed as zero.
struct DiskTemperatureSample {
    std::uint32_t diskIndex = 0;
    std::uint32_t sensorIndex = 0; // 0 may be composite; others remain unnamed.
    int temperatureC = 0;
    wchar_t model[96]{};
};
constexpr std::size_t kDiskTemperatureCapacity = 64;
struct MetricsSample {
    double cpuPercent = 0.0;
    // Highest busy-time percentage among every active logical processor.
    // These are logical processors (SMT threads), not physical cores. The
    // group/index identify the Windows Processor Information counter instance.
    double cpuPeakPercent = 0.0;
    std::uint32_t cpuPeakGroup = 0;
    std::uint32_t cpuPeakLogicalProcessor = 0;
    std::uint32_t cpuLogicalProcessorCount = 0;
    std::uint32_t cpuPeakValidItems = 0;
    std::uint32_t cpuPeakInvalidItems = 0;
    std::uint64_t cpuPeakSampleIntervalMs = 0;
    CpuCounterStatus cpuPeakStatus = CpuCounterStatus::CounterUnavailable;
    bool cpuPeakAvailable = false;
    // Actual Frequency is the current MHz measured by Windows. Per-counter
    // provider semantics and the timer window differ from instantaneous clocks
    // and Task Manager's aggregate speed; no identical-value promise is made.
    // The mean gives each logical processor equal weight, not load weighting.
    // A complete valid logical-instance inventory is required for the range.
    double cpuFrequencyMinMHz = 0.0;
    double cpuFrequencyMaxMHz = 0.0;
    double cpuFrequencyMeanMHz = 0.0;
    // Processor Frequency is the separate Windows-requested/reported MHz value.
    // It may remain nominal; it is diagnostic only and never substitutes for
    // Actual Frequency when that counter is missing or invalid.
    double cpuReportedFrequencyMinMHz = 0.0;
    double cpuReportedFrequencyMaxMHz = 0.0;
    bool cpuReportedFrequencyAvailable = false;
    std::uint32_t cpuFrequencyValidItems = 0;
    std::uint32_t cpuFrequencyInvalidItems = 0;
    std::uint64_t cpuFrequencySampleIntervalMs = 0;
    CpuCounterStatus cpuFrequencyStatus = CpuCounterStatus::CounterUnavailable;
    bool cpuFrequencyAvailable = false;
    double ramPercent = 0.0;
    double downloadBytesPerSec = 0.0;
    double uploadBytesPerSec = 0.0;
    DailyTrafficTotals dailyTraffic{};
    DailyTrafficStoreStatus dailyTrafficStoreStatus = DailyTrafficStoreStatus::MemoryOnly;
    std::uint32_t dailyTrafficStoreError = 0;
    bool dailyTrafficAvailable = false;
    bool dailyTrafficOverflowed = false;
    bool dailyTrafficIncomplete = false;
    bool dailyTrafficStoreAttention = false;
    double gpuPercent = 0.0;
    std::array<GpuEngineCategorySample, kGpuEngineCategoryCount> gpuEngines{};
    IntegratedGpuSample integratedGpu{};
    // NVML kernel-active time is diagnostic only; never replaces gpuPercent.
    double nvmlKernelPercent = 0.0;
    double diskBusyPercent = 0.0; // Busiest physical disk, from 100 - % Idle Time.
    std::uint64_t ramUsedBytes = 0;
    std::uint64_t ramTotalBytes = 0;
    std::uint64_t gpuUsedBytes = 0;
    std::uint64_t gpuTotalBytes = 0;
    std::uint32_t gpuTemperatureC = 0;
    // gpuTemperatureC is the NVIDIA GPU die sensor. These are distinct optional
    // sensors on that same UUID-pinned board, never estimates from the die value.
    std::uint32_t gpuHotspotTemperatureC = 0;
    std::uint32_t gpuMemoryTemperatureC = 0;
    std::uint32_t gpuPowerTemperatureC = 0;
    std::uint32_t gpuBoardTemperatureC = 0;
    int cpuTemperatureC = 0;
    CpuTemperatureStatus cpuTemperatureStatus = CpuTemperatureStatus::NeedsPermission;
    std::int32_t cpuTemperatureError = 0;
    std::uint32_t cpuTemperatureRaw = 0;
    std::uint64_t cpuTemperatureSampledAtTickMs = 0;
    bool cpuTemperatureHelperActive = false;
    int integratedGpuTemperatureC = 0;
    int diskTemperatureC = 0; // Hottest valid sensor of any physical disk.
    std::uint32_t hottestDiskIndex = 0;
    std::uint32_t hottestDiskSensorIndex = 0;
    std::array<DiskTemperatureSample, kDiskTemperatureCapacity> diskTemperatures{};
    std::size_t diskTemperatureCount = 0;
    std::uint32_t activeAdapterCount = 0;
    wchar_t firstAdapterName[128]{};
    char gpuName[96]{}; // NVML UTF-8 device name; first NVIDIA device only.
    char gpuUuid[96]{};
    char gpuPciBusId[32]{};
    wchar_t gpuEngineType[48]{};
    std::uint32_t gpuLuidHigh = 0;
    std::uint32_t gpuLuidLow = 0;
    std::uint32_t gpuPhysicalIndex = 0;
    std::uint32_t gpuEngineIndex = 0;
    std::uint32_t gpuEngineValidItems = 0;
    std::uint32_t gpuEngineInvalidItems = 0;
    std::uint32_t gpuEngineCount = 0;
    std::uint64_t sampledAtTickMs = 0;
    std::uint64_t cpuSampleIntervalMs = 0;
    std::uint64_t gpuSampleIntervalMs = 0;
    std::uint64_t gpuAuxTemperatureSampledAtTickMs = 0;
    GpuSampleStatus gpuStatus = GpuSampleStatus::Disabled;
    bool cpuAvailable = false;
    bool ramAvailable = false;
    bool networkAvailable = false;
    bool gpuAvailable = false;
    bool gpuUtilizationAvailable = false;
    bool nvmlKernelAvailable = false;
    bool gpuTemperatureAvailable = false;
    bool gpuHotspotTemperatureAvailable = false;
    bool gpuMemoryTemperatureAvailable = false;
    bool gpuPowerTemperatureAvailable = false;
    bool gpuBoardTemperatureAvailable = false;
    bool cpuTemperatureAvailable = false;
    bool integratedGpuTemperatureAvailable = false;
    bool diskTemperatureAvailable = false;
    bool gpuMemoryAvailable = false;
    bool diskAvailable = false;
};

// Use on one thread. Construct once, then Sample() from a roughly 1-second
// timer. CPU/network rates need a baseline. Normal sampling needs no WMI or
// elevation. CPU temperature is an explicitly enabled, elevated read-only helper.
class MetricsSampler {
public:
    explicit MetricsSampler(bool enableGpu = false, bool enableDisk = false);
    ~MetricsSampler();
    MetricsSampler(const MetricsSampler&) = delete;
    MetricsSampler& operator=(const MetricsSampler&) = delete;

    MetricsSample Sample() noexcept;
    // Restore recorded totals only. Hardware baselines always start fresh.
    // A null path keeps test sampling entirely in memory.
    void ConfigureDailyTraffic(const wchar_t* path) noexcept;
    void FlushDailyTraffic() noexcept;
    // Enabling can allocate/initialize an optional provider once. Disabling
    // closes its query/session and unloads the optional DLL immediately.
    void Configure(bool enableGpu, bool enableDisk);
    bool EnableCpuTemperature(std::uintptr_t owner) noexcept;
    bool EnableCpuTemperatureAutomatic() noexcept;
    void DisableCpuTemperature() noexcept;
    bool CpuTemperatureHelperActive() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lightmon
