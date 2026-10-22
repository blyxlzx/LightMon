#pragma once

#include <cstdint>
#include <optional>

namespace lightmon {

enum class TaskbarGapState { Unknown, Available, NoSpace };

// The caller owns synchronization and defines complete request identity in same.
// A failed query never renews or destroys the last verified available placement.
template<class Request>
class TaskbarGapCache {
public:
    static constexpr std::uint64_t kLifetimeMs = 10000;

    template<class Same>
    void Commit(const Request& request, TaskbarGapState state, int x,
                std::uint64_t tick, Same same) {
        latest_ = Record{request, state, x, tick};
        if(state == TaskbarGapState::Available) {
            lastAvailable_ = latest_;
        } else if(state == TaskbarGapState::NoSpace && lastAvailable_ &&
                  same(lastAvailable_->request, request)) {
            // A complete query has disproved this placement. Later Unknown
            // results must not resurrect it from the earlier success.
            lastAvailable_.reset();
        }
    }

    template<class Same>
    bool Resolve(const Request& request, std::uint64_t now, int& x, Same same) const {
        if(latest_ && latest_->state == TaskbarGapState::NoSpace &&
           same(latest_->request, request)) return false;
        if(!lastAvailable_ || !same(lastAvailable_->request, request)) return false;
        const auto tick = lastAvailable_->tick;
        // Check the ordering before subtraction: a future tick must not wrap.
        if(tick > now || now - tick > kLifetimeMs) return false;
        x = lastAvailable_->x;
        return true;
    }

private:
    struct Record {
        Request request;
        TaskbarGapState state;
        int x;
        std::uint64_t tick;
    };
    std::optional<Record> latest_;
    std::optional<Record> lastAvailable_;
};

} // namespace lightmon
