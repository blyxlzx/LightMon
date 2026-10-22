#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace lightmon {
enum class CpuTemperatureStatus : std::uint32_t {
    NeedsPermission, Starting, Available, UnsupportedCpu, DriverMissing,
    AccessDenied, DllMissing, DllInvalid, ModuleRejected, MutexUnavailable,
    ReadFailed, InvalidSensor, Stale, HelperExited, LaunchCancelled,
    LaunchFailed, ParentMismatch, Stopped
};

constexpr std::uint32_t kCpuTemperatureProtocolMagic = 0x4d43544c;
constexpr std::uint32_t kCpuTemperatureProtocolVersion = 1;
constexpr unsigned kCpuTemperaturePollMs = 2000;
constexpr unsigned kCpuTemperatureStaleMs = 5000;

struct CpuTemperatureSnapshot {
    std::uint64_t sampledAtTickMs = 0;
    std::uint32_t raw = 0;
    std::int32_t temperatureMilliC = 0;
    CpuTemperatureStatus status = CpuTemperatureStatus::Starting;
    std::int32_t lastError = 0;
};

// One private page shared with one authorized elevated helper. No input accepts
// hardware addresses or operations. The only parent command is the stop flag.
struct alignas(8) CpuTemperatureSharedPage {
    std::uint32_t magic = kCpuTemperatureProtocolMagic;
    std::uint32_t version = kCpuTemperatureProtocolVersion;
    std::uint32_t parentPid = 0;
    // Protocol-v1 reserved word now carries the helper PID handshake. Existing
    // offsets and the 56-byte page layout remain unchanged.
    volatile LONG reserved = 0;
    std::uint64_t parentCreationTime = 0;
    volatile LONG stop = 0;
    volatile LONG sequence = 0;
    CpuTemperatureSnapshot snapshot{};
};
static_assert(sizeof(CpuTemperatureSharedPage) == 56, "CPU IPC v1 layout remains stable");

inline std::uint64_t CpuFileTimeValue(const FILETIME& value) noexcept {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}

// AMD Family19h Model61h: Tctl/Tdie in 0.125 C units, including the range/TjSel
// adjustment shared by LHM and Linux k10temp. The supported 7800X3D has no old
// Ryzen/TR marketing offset. Reject absent and physically implausible readings.
inline bool DecodeCpuTemperatureRaw(std::uint32_t raw, std::int32_t& milliC) noexcept {
    if (raw == 0 || raw == 0xffffffffu) return false;
    const auto count = raw >> 21;
    std::int32_t value = static_cast<std::int32_t>(count * 125u);
    if ((raw & 0x80000u) != 0 || (raw & 0x30000u) == 0x30000u) value -= 49000;
    if (value < 0 || value > 125000) return false;
    milliC = value;
    return true;
}

inline bool ReadCpuTemperatureSnapshot(const CpuTemperatureSharedPage* page,
                                      CpuTemperatureSnapshot& out) noexcept {
    if (!page) return false;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        auto* sequence = const_cast<volatile LONG*>(&page->sequence);
        const LONG before = InterlockedCompareExchange(sequence, 0, 0);
        if ((before & 1) != 0) continue;
        CpuTemperatureSnapshot candidate{};
        std::memcpy(&candidate, &page->snapshot, sizeof(candidate));
        MemoryBarrier();
        const LONG after = InterlockedCompareExchange(sequence, 0, 0);
        if (before == after && (after & 1) == 0) { out = candidate; return true; }
    }
    return false;
}

inline void PublishCpuTemperatureSnapshot(CpuTemperatureSharedPage* page,
                                         const CpuTemperatureSnapshot& value) noexcept {
    InterlockedIncrement(&page->sequence);
    std::memcpy(&page->snapshot, &value, sizeof(value));
    InterlockedIncrement(&page->sequence);
}

inline bool IsFreshCpuTemperature(const CpuTemperatureSnapshot& value,
                                 std::uint64_t now) noexcept {
    std::int32_t decoded = 0;
    return value.status == CpuTemperatureStatus::Available &&
           value.sampledAtTickMs != 0 && value.sampledAtTickMs <= now &&
           now - value.sampledAtTickMs <= kCpuTemperatureStaleMs &&
           DecodeCpuTemperatureRaw(value.raw, decoded) && decoded == value.temperatureMilliC;
}
} // namespace lightmon
