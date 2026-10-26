#pragma once
#include "core/types.hpp"
#include "gridlock/gridlock.hpp"

using namespace std;

const Time NO_DEADLINE = numeric_limits<Time>::max();

struct EngineConfig {
    Time batch_interval = 0;
    size_t queue_trigger = 0;
    Amount value_trigger = 0;
    bool deadline_batch = false;
};

enum class Status { Queued, Settled, Expired, Unsettled };
enum class How { None, Arrival, Recycle, Batch };

struct PaymentRecord {
    int from = 0;
    int to = 0;
    Amount amt = 0;
    Time arrival = 0;
    Time deadline = NO_DEADLINE;
    Status status = Status::Queued;
    How how = How::None;
    Time done_at = -1;
};

struct EngineStats {
    size_t submitted = 0, settled = 0, by_arrival = 0, by_recycle = 0, by_batch = 0, expired = 0, unsettled = 0, queued = 0;
    Amount settled_value = 0, expired_value = 0, unsettled_value = 0, queued_value = 0;
    int batches = 0;
    Time delay_p50 = 0, delay_p95 = 0, delay_p99 = 0, delay_max = 0;
};

class SettlementEngine {
public:
    SettlementEngine(const vector<Amount>& balance, const vector<Amount>& credit_limit,
                     GridlockSolver& solver, EngineConfig cfg = {});
    int submit(int from, int to, Amount amt, Time t, Time deadline = NO_DEADLINE);
    void advance_to(Time t);
    void close(Time t);

    Time now() const;
    bool closed() const;
    const vector<Amount>& positions() const;
    const vector<PaymentRecord>& payments() const;
    size_t queue_size() const;
    Amount queued_value() const;
    int batches_run() const;
    EngineStats stats() const;
private:
    int n_;
    vector<Amount> pos_, credit_;
    GridlockSolver& solver_;
    EngineConfig cfg_;
    Time now_ = 0;
    Time next_tick_ = 0;
    bool closed_ = false;
    int batches_ = 0;
    vector<PaymentRecord> rec_;
    set<int> queue_;
    vector<set<int>> by_payer_;
    set<pair<Time,int>> deadlines_;
    Amount queued_value_ = 0;

    void settle(int id, How how);
    void dequeue(int id);
    void recycle(int start);
    void run_batch();
};