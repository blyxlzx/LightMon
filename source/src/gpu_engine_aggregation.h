#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include "metrics.h"

namespace lightmon {
namespace detail {

struct GpuEngineInstance {
    std::uint32_t pid = 0;
    std::uint32_t luidHigh = 0;
    std::uint32_t luidLow = 0;
    std::uint32_t phys = 0;
    std::uint32_t engine = 0;
    wchar_t type[48]{};
};

struct GpuEngineSummary {
    bool available = false;
    bool overflow = false;
    double percent = 0.0;
    std::uint32_t busiestPhys = 0;
    std::uint32_t busiestEngine = 0;
    wchar_t type[48]{};
    DWORD validItems = 0;
    DWORD invalidItems = 0;
    DWORD engineCount = 0;
    std::array<GpuEngineCategorySample, kGpuEngineCategoryCount> gpuEngines{};
};

inline bool GpuConsumeLiteral(const wchar_t*& cursor, const wchar_t* literal) noexcept {
    while (*literal) {
        if (*cursor != *literal) return false;
        ++cursor;
        ++literal;
    }
    return true;
}

inline bool GpuReadUnsigned(const wchar_t*& cursor, unsigned base,
                            std::uint32_t& value) noexcept {
    constexpr std::uint32_t limit = 0xffffffffu;
    const unsigned digitLimit = base == 16 ? 8u : 10u;
    unsigned digits = 0;
    value = 0;
    while (*cursor) {
        unsigned digit = base;
        if (*cursor >= L'0' && *cursor <= L'9') digit = static_cast<unsigned>(*cursor - L'0');
        else if (base == 16 && *cursor >= L'a' && *cursor <= L'f')
            digit = static_cast<unsigned>(*cursor - L'a') + 10u;
        else if (base == 16 && *cursor >= L'A' && *cursor <= L'F')
            digit = static_cast<unsigned>(*cursor - L'A') + 10u;
        if (digit >= base) break;
        if (++digits > digitLimit || value > (limit - digit) / base) return false;
        value = value * base + digit;
        ++cursor;
    }
    return digits != 0;
}

// Parse the entire Windows GPU Engine instance identity. The adapter LUID and
// physical engine number remain distinct; type classifies rather than keys an
// engine, so Compute_0/Compute_1 never merge different physical engine IDs.
inline bool ParseGpuEngineInstance(const wchar_t* name, GpuEngineInstance& result) noexcept {
    result = {};
    if (!name) return false;
    const wchar_t* cursor = name;
    if (!GpuConsumeLiteral(cursor, L"pid_") ||
        !GpuReadUnsigned(cursor, 10, result.pid) ||
        !GpuConsumeLiteral(cursor, L"_luid_0x") ||
        !GpuReadUnsigned(cursor, 16, result.luidHigh) ||
        !GpuConsumeLiteral(cursor, L"_0x") ||
        !GpuReadUnsigned(cursor, 16, result.luidLow) ||
        !GpuConsumeLiteral(cursor, L"_phys_") ||
        !GpuReadUnsigned(cursor, 10, result.phys) ||
        !GpuConsumeLiteral(cursor, L"_eng_") ||
        !GpuReadUnsigned(cursor, 10, result.engine) ||
        !GpuConsumeLiteral(cursor, L"_engtype_")) return false;

    std::size_t length = 0;
    while (*cursor) {
        const wchar_t c = *cursor++;
        // Driver-provided names can contain spaces (for example, AMD's
        // "High Priority 3D" and "Video Codec 0"). Preserve them for the
        // overall peak; an unknown category must not drop a valid engine.
        const bool valid = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
                           (c >= L'0' && c <= L'9') || c == L'_' || c == L' ';
        if (!valid || length + 1 >= std::size(result.type)) return false;
        result.type[length++] = c;
    }
    result.type[length] = L'\0';
    return length != 0 && result.type[0] != L' ' && result.type[length - 1] != L' ';
}

// Windows exposes driver engine names. Only these recognized labels enter a
// display category; unknown labels still contribute to the overall GPU peak.
inline GpuEngineCategory ClassifyGpuEngineType(const wchar_t* type) noexcept {
    if (!type) return GpuEngineCategory::Count;
    if (std::wcscmp(type, L"3D") == 0) return GpuEngineCategory::ThreeD;
    if (std::wcscmp(type, L"Copy") == 0) return GpuEngineCategory::Copy;
    if (std::wcscmp(type, L"VideoEncode") == 0) return GpuEngineCategory::VideoEncode;
    if (std::wcscmp(type, L"VideoDecode") == 0) return GpuEngineCategory::VideoDecode;
    if (std::wcscmp(type, L"Compute") == 0) return GpuEngineCategory::Compute;
    // Exact OTHER names exposed by the current AMD adapter's node metadata
    // and GPU Engine counters. They remain bound to that adapter's LUID.
    if (std::wcscmp(type, L"High Priority Compute") == 0) return GpuEngineCategory::Compute;
    const wchar_t* cursor = type;
    std::uint32_t suffix = 0;
    if (GpuConsumeLiteral(cursor, L"Compute_") &&
        GpuReadUnsigned(cursor, 10, suffix) && *cursor == L'\0')
        return GpuEngineCategory::Compute;
    cursor = type;
    if (GpuConsumeLiteral(cursor, L"Compute ") &&
        GpuReadUnsigned(cursor, 10, suffix) && *cursor == L'\0')
        return GpuEngineCategory::Compute;
    return GpuEngineCategory::Count;
}

// A valid zero sample is available. Empty/baseline/invalid samples are not.
// Any invalid target-adapter item rejects this batch: dropping one PID's
// contribution could otherwise understate an engine sum or select wrong peak.
// Reject a capacity overflow as a whole, so no partial maximum is displayed.
inline GpuEngineSummary AggregateGpuEngineUsage(
    const PDH_FMT_COUNTERVALUE_ITEM_W* items, DWORD count,
    std::uint32_t targetLuidHigh, std::uint32_t targetLuidLow) noexcept {
    GpuEngineSummary result{};
    if (!items || count == 0) return result;
    struct Engine {
        std::uint32_t phys = 0;
        std::uint32_t number = 0;
        double sum = 0.0;
        GpuEngineCategory category = GpuEngineCategory::Count;
        wchar_t type[48]{};
    };
    std::array<Engine, 256> engines{};
    std::size_t engineCount = 0;
    for (DWORD i = 0; i < count; ++i) {
        GpuEngineInstance parsed{};
        if (!ParseGpuEngineInstance(items[i].szName, parsed) ||
            parsed.luidHigh != targetLuidHigh || parsed.luidLow != targetLuidLow) continue;
        // PDH_CSTATUS_VALID_DATA == 0; PDH_CSTATUS_NEW_DATA == 1.
        // Avoid interpreting the union for any failed per-instance sample.
        const DWORD status = items[i].FmtValue.CStatus;
        if (status != 0u && status != 1u) { ++result.invalidItems; continue; }
        const double value = items[i].FmtValue.doubleValue;
        if (!std::isfinite(value) || value < 0.0) { ++result.invalidItems; continue; }

        std::size_t index = 0;
        while (index < engineCount &&
               (engines[index].phys != parsed.phys || engines[index].number != parsed.engine)) ++index;
        if (index == engineCount) {
            if (engineCount == engines.size()) {
                result.overflow = true;
                result.engineCount = static_cast<DWORD>(engineCount);
                return result;
            }
            Engine& entry = engines[engineCount++];
            entry.phys = parsed.phys;
            entry.number = parsed.engine;
            entry.category = ClassifyGpuEngineType(parsed.type);
            std::wmemcpy(entry.type, parsed.type, std::size(entry.type));
        }
        // Inconsistent categories for the same physical engine cannot yield a
        // trustworthy category peak. Equivalent Compute aliases are allowed.
        if (engines[index].category != ClassifyGpuEngineType(parsed.type)) {
            ++result.invalidItems;
            continue;
        }
        // Clamping while summing is equivalent to capping the final engine
        // sum for nonnegative inputs, and cannot overflow a finite double.
        Engine& entry = engines[index];
        const double boundedValue = value > 100.0 ? 100.0 : value;
        entry.sum += boundedValue;
        if (entry.sum > 100.0) entry.sum = 100.0;
        ++result.validItems;
    }
    result.engineCount = static_cast<DWORD>(engineCount);
    if (engineCount == 0 || result.invalidItems != 0) return result;
    std::size_t busiest = 0;
    for (std::size_t i = 1; i < engineCount; ++i) {
        const Engine& a = engines[i];
        const Engine& b = engines[busiest];
        if (a.sum > b.sum || (a.sum == b.sum &&
            (a.phys < b.phys || (a.phys == b.phys && a.number < b.number)))) busiest = i;
    }
    result.available = true;
    result.percent = engines[busiest].sum;
    result.busiestPhys = engines[busiest].phys;
    result.busiestEngine = engines[busiest].number;
    std::wmemcpy(result.type, engines[busiest].type, std::size(result.type));
    for (std::size_t i = 0; i < engineCount; ++i) {
        const Engine& engine = engines[i];
        const std::size_t category = static_cast<std::size_t>(engine.category);
        if (category >= result.gpuEngines.size()) continue;
        GpuEngineCategorySample& peak = result.gpuEngines[category];
        ++peak.engineCount;
        if (!peak.available || engine.sum > peak.percent ||
            (engine.sum == peak.percent && (engine.phys < peak.physicalIndex ||
             (engine.phys == peak.physicalIndex && engine.number < peak.engineIndex)))) {
            peak.percent = engine.sum;
            peak.physicalIndex = engine.phys;
            peak.engineIndex = engine.number;
        }
        peak.available = true;
    }
    return result;
}

} // namespace detail
} // namespace lightmon
