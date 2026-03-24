#ifndef TEST_LIMIT_UP_FLOW_DETECTOR_H
#define TEST_LIMIT_UP_FLOW_DETECTOR_H

#include "market_data_structs_aligned.h"
#include "FastOrderBook.h"

#include <cstdint>
#include <string>

// Test-only stub used to compile strategy unit tests without the production
// LimitUpFlowDetector implementation.
class LimitUpFlowDetector {
public:
    struct Config {
        uint32_t flow_threshold_pct = 0;
        int32_t flow_window_ms = 0;
        uint64_t min_bid_volume = 0;
        double cancel_weight = 0.0;
    };

    LimitUpFlowDetector() = default;
    explicit LimitUpFlowDetector(const Config& config) : config_(config) {}

    const Config& config() const { return config_; }
    bool is_synced() const { return synced_; }

    void sync_from_book(const FastOrderBook&, uint32_t, const std::string&) {
        synced_ = true;
    }

    void on_order(const MDOrderStruct&, uint32_t) {}
    void on_transaction(const MDTransactionStruct&, uint32_t) {}

    bool check(int32_t, const FastOrderBook&, uint32_t) const { return false; }
    uint64_t get_flow_sum(int32_t) const { return 0; }
    uint64_t flow_event_count() const { return 0; }

private:
    Config config_{};
    bool synced_ = false;
};

#endif // TEST_LIMIT_UP_FLOW_DETECTOR_H
