#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwchar>

namespace lightmon {
namespace detail {

// Windows supports up to 64 processor groups, each with at most 64 logical
// processors. Keep the validation scratch space bounded and allocation-free.
constexpr std::size_t kCpuGroupCapacity = 64;
struct CpuTopology {
    std::array<std::uint32_t, kCpuGroupCapacity> logicalCounts{};
    std::uint32_t groupCount = 0;
    std::uint32_t logicalCount = 0;
    bool available = false;
};

struct CpuCounterSummary {
    bool available = false;
    double minimum = 0.0;
    double maximum = 0.0;
    double mean = 0.0;
    std::uint32_t peakGroup = 0;
    std::uint32_t peakLogicalProcessor = 0;
    std::uint32_t validItems = 0;
    std::uint32_t invalidItems = 0;
};

inline bool CpuReadDecimal(const wchar_t*& cursor, std::uint32_t& value) noexcept {
    value = 0;
    unsigned digits = 0;
    while (*cursor >= L'0' && *cursor <= L'9') {
        const auto digit = static_cast<std::uint32_t>(*cursor - L'0');
        if (++digits > 10 || value > (0xffffffffu - digit) / 10u) return false;
        value = value * 10u + digit;
        ++cursor;
    }
    return digits != 0;
}

// Processor Information includes _Total and per-group N,_Total instances.
// They cannot compete with individual logical processors for the maximum.
inline bool IsCpuTotalInstance(const wchar_t* name) noexcept {
    if (!name) return false;
    if (std::wcscmp(name, L"_Total") == 0) return true;
    const wchar_t* cursor = name;
    std::uint32_t group = 0;
    return CpuReadDecimal(cursor, group) && *cursor++ == L',' &&
        std::wcscmp(cursor, L"_Total") == 0;
}

inline bool ParseCpuLogicalInstance(const wchar_t* name, std::uint32_t& group,
                                    std::uint32_t& logical) noexcept {
    if (!name) return false;
    const wchar_t* cursor = name;
    return CpuReadDecimal(cursor, group) && *cursor++ == L',' &&
        CpuReadDecimal(cursor, logical) && *cursor == L'\0';
}

// Validate completeness by group and processor number, not by item count:
// a duplicate and a missing instance must not cancel each other out. Reject
// partial/invalid data instead of falsely calling a lower visible value the
// maximum. Ties use the lowest group and logical index, independent of order.
inline CpuCounterSummary AggregateCpuCounters(
    const PDH_FMT_COUNTERVALUE_ITEM_W* items, std::size_t count,
    const CpuTopology& topology, bool frequency) noexcept {
    CpuCounterSummary result{};
    if (!items || !topology.available || topology.groupCount == 0 ||
        topology.groupCount > kCpuGroupCapacity || topology.logicalCount == 0) return result;
    std::uint32_t expected = 0;
    for (std::uint32_t group = 0; group < topology.groupCount; ++group) {
        if (topology.logicalCounts[group] == 0 || topology.logicalCounts[group] > 64) return result;
        expected += topology.logicalCounts[group];
    }
    if (expected != topology.logicalCount) return result;
    std::array<std::uint64_t, kCpuGroupCapacity> seen{};
    for (std::size_t i = 0; i < count; ++i) {
        const auto& item = items[i];
        if (IsCpuTotalInstance(item.szName)) continue;
        std::uint32_t group = 0, logical = 0;
        if (!ParseCpuLogicalInstance(item.szName, group, logical) ||
            group >= topology.groupCount || logical >= 64 ||
            logical >= topology.logicalCounts[group]) {
            ++result.invalidItems;
            continue;
        }
        const std::uint64_t bit = std::uint64_t{1} << logical;
        if ((seen[group] & bit) != 0) {
            ++result.invalidItems;
            continue;
        }
        seen[group] |= bit;
        const double value = item.FmtValue.doubleValue;
        const bool statusValid = item.FmtValue.CStatus == PDH_CSTATUS_VALID_DATA ||
            item.FmtValue.CStatus == PDH_CSTATUS_NEW_DATA;
        const bool rangeValid = frequency ? value > 0.0 && value <= 100000.0 :
            value >= 0.0 && value <= 100.0;
        if (!statusValid || !std::isfinite(value) || !rangeValid) {
            ++result.invalidItems;
            continue;
        }
        const bool first = result.validItems++ == 0;
        result.mean += value;
        if (first || value < result.minimum) result.minimum = value;
        if (first || value > result.maximum ||
            (value == result.maximum && (group < result.peakGroup ||
                (group == result.peakGroup && logical < result.peakLogicalProcessor)))) {
            result.maximum = value;
            result.peakGroup = group;
            result.peakLogicalProcessor = logical;
        }
    }
    result.available = result.invalidItems == 0 && result.validItems == topology.logicalCount;
    if (result.validItems != 0) result.mean /= result.validItems;
    return result;
}

} // namespace detail
} // namespace lightmon
