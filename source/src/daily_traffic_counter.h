#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace lightmon {

struct DailyTrafficTotals {
    std::uint32_t date = 0; // Local calendar date in YYYYMMDD form.
    std::uint64_t downloadBytes = 0;
    std::uint64_t uploadBytes = 0;
};

// Accumulates differences of Windows' 64-bit per-interface byte counters.
// No allocation, elapsed-time assumption, or integration of rounded rates.
// A caller must BeginSample, Observe every successfully read eligible interface,
// then EndSample before reading the newly committed Totals. Missing interfaces
// are forgotten; their next observation establishes a fresh baseline.
class DailyTrafficCounter {
public:
    static constexpr std::size_t kCapacity = 64;

    void Restore(DailyTrafficTotals totals) noexcept {
        totals_ = totals.date != 0 ? totals : DailyTrafficTotals{};
        pending_ = totals_;
        ClearBaselines();
        sampleOpen_ = false;
        overflowed_ = false;
        pendingOverflowed_ = false;
        capacityExceeded_ = false;
    }

    // The caller supplies a valid local date, or zero if the date is unavailable.
    // An unavailable date preserves committed totals but breaks every baseline,
    // so a later valid read cannot silently count the unobserved interval.
    void BeginSample(std::uint32_t date) noexcept {
        sampleOpen_ = date != 0;
        capacityExceeded_ = false;
        if (date == 0) {
            ClearBaselines();
            pending_ = totals_;
            pendingOverflowed_ = overflowed_;
            return;
        }
        if (totals_.date != date) {
            totals_ = DailyTrafficTotals{date, 0, 0};
            ClearBaselines();
            overflowed_ = false;
        }
        pending_ = totals_;
        pendingOverflowed_ = overflowed_;
        for (Slot& slot : slots_) slot.seen = false;
    }

    // Returns false if this sample cannot be accepted. A new LUID never adds
    // the interface's pre-existing lifetime bytes to today's total. Duplicate
    // LUIDs in one snapshot are ignored after the first successfully read row.
    bool Observe(std::uint64_t luid, std::uint64_t inBytes,
                 std::uint64_t outBytes) noexcept {
        if (!sampleOpen_ || capacityExceeded_) return false;

        Slot* empty = nullptr;
        for (Slot& slot : slots_) {
            if (!slot.occupied) {
                if (!empty) empty = &slot;
                continue;
            }
            if (slot.luid != luid) continue;
            if (slot.seen) return true;
            slot.seen = true;

            // One direction resetting invalidates the pair, not just that
            // direction. This prevents a device reset becoming a false spike.
            if (inBytes >= slot.inBytes && outBytes >= slot.outBytes) {
                AddSaturating(pending_.downloadBytes, inBytes - slot.inBytes);
                AddSaturating(pending_.uploadBytes, outBytes - slot.outBytes);
            }
            slot.inBytes = inBytes;
            slot.outBytes = outBytes;
            return true;
        }

        if (!empty) {
            // An unseen old slot may occur later in this snapshot; evicting it
            // would silently lose a valid delta. Reject the whole sample instead.
            capacityExceeded_ = true;
            return false;
        }
        *empty = Slot{luid, inBytes, outBytes, true, true};
        return true;
    }

    void EndSample() noexcept {
        if (!sampleOpen_) return;
        sampleOpen_ = false;
        if (capacityExceeded_) {
            // Do not publish a partial total as a complete sample. Fresh
            // baselines next time also prevent recovery from back-filling it.
            ClearBaselines();
            pending_ = totals_;
            pendingOverflowed_ = overflowed_;
            return;
        }
        totals_ = pending_;
        overflowed_ = pendingOverflowed_;
        for (Slot& slot : slots_) {
            if (!slot.seen) slot = Slot{};
        }
    }

    const DailyTrafficTotals& Totals() const noexcept { return totals_; }
    bool Overflowed() const noexcept { return overflowed_; }
    bool CapacityExceeded() const noexcept { return capacityExceeded_; }

private:
    struct Slot {
        std::uint64_t luid = 0;
        std::uint64_t inBytes = 0;
        std::uint64_t outBytes = 0;
        bool occupied = false;
        bool seen = false;
    };

    void ClearBaselines() noexcept {
        for (Slot& slot : slots_) slot = Slot{};
    }

    void AddSaturating(std::uint64_t& total, std::uint64_t delta) noexcept {
        constexpr std::uint64_t maximum =
            (std::numeric_limits<std::uint64_t>::max)();
        if (delta > maximum - total) {
            total = maximum;
            pendingOverflowed_ = true;
        } else {
            total += delta;
        }
    }

    std::array<Slot, kCapacity> slots_{};
    DailyTrafficTotals totals_{};
    DailyTrafficTotals pending_{};
    bool sampleOpen_ = false;
    bool overflowed_ = false;
    bool pendingOverflowed_ = false;
    bool capacityExceeded_ = false;
};

} // namespace lightmon
