#include "stream/engine.hpp"
#include "core/instance.hpp"
#include "core/validator.hpp"
#include "gridlock/fifo_bypass.hpp"
#include "gridlock/drop_violators.hpp"
#include "gridlock/branch_bound.hpp"
#include "gridlock/maxflow_relax.hpp"
#include "gridlock/lp_bound.hpp"
#include "core/rng.hpp"

using namespace std;

// Contract used by this file (includes/stream/engine.hpp, src/stream/engine.cpp). Everything is deterministic.
//
//   SettlementEngine(balance, credit_limit, GridlockSolver& solver, EngineConfig cfg = {})
//       throws invalid_argument: n < 1, sizes differ, a negative balance or credit_limit, batch_interval < 0,
//       value_trigger < 0. A bank may pay while  position + credit_limit >= amount.  Clock starts at 0.
//
//   int submit(from, to, amt, t, deadline = NO_DEADLINE)          returns the payment id 0, 1, 2, ...
//       throws invalid_argument (state unchanged): endpoints out of range or equal, amt <= 0, t < now(), deadline < t
//       throws logic_error after close()
//       1. advance_to(t)
//       2. if the payer can afford it: settle now (How::Arrival), then RECYCLE starting at the payee
//          else: queue it; then if (queue_trigger > 0 and queue_size() >= queue_trigger) or
//                (value_trigger > 0 and queued_value() >= value_trigger): run a BATCH now
//
//   RECYCLE(start): a FIFO worklist of banks, initially [start]. Pop bank b; go through b's queued payments in id
//       order and settle (How::Recycle) every one b can afford at that moment, skipping those it cannot (bypass);
//       each settled payment pushes its payee onto the worklist unless that bank is already waiting in it.
//
//   BATCH at time now: if the queue is empty nothing happens (and it is not counted). Otherwise build an Instance with
//       the same n, balance_i = position_i + credit_limit_i, credit_limit_i = 0, obs = queued payments in id order;
//       call solver.solve(); if validate_gridlock rejects the answer throw logic_error; settle every selected payment
//       (How::Batch). batches_run() counts these batches. No recycling after a batch.
//
//   advance_to(t): throws invalid_argument if t < now(), logic_error after close(). Processes timer events in time
//       order, every event time tau <= t (events at time tau come BEFORE an arrival at tau):
//         tick times      batch_interval, 2*batch_interval, ... (only if batch_interval > 0)
//         deadline times  the deadline of every queued payment
//       at each such tau: one BATCH if a tick is at tau, or if deadline_batch and a queued payment has deadline tau;
//       then every queued payment with deadline <= tau becomes Status::Expired (done_at = tau). Then now() = t.
//
//   close(t): advance_to(t), one BATCH at t, every payment still queued becomes Status::Unsettled (done_at = t).
//       After close every mutating call throws logic_error; queries still work.
//
//   PaymentRecord: from, to, amt, arrival, deadline, status, how (None unless settled), done_at (-1 while queued).
//   stats(): counts and values per status / how, batches, delay = done_at - arrival over SETTLED payments,
//       percentiles by nearest rank: sorted delays d, p-th = d[ceil(p * N / 100) - 1]; all 0 when nothing settled.

static int failures = 0;

#define CHECK(cond) \
    do { if(!(cond)) { cerr << __FILE__ << ":" << __LINE__ << "  CHECK failed: " #cond "\n"; failures++; } } while(0)

// ------------------------------------------------------------------ helpers
template <class E, class F>
static bool throws(F f){
    try{
        f();
    }catch(const E&){
        return true;
    }catch(...){
        return false;
    }
    return false;
}

static bool same_record(const PaymentRecord& a, const PaymentRecord& b){
    return a.from == b.from && a.to == b.to && a.amt == b.amt && a.arrival == b.arrival && a.deadline == b.deadline &&
           a.status == b.status && a.how == b.how && a.done_at == b.done_at;
}

static void expect_payment(const char* label, const SettlementEngine& e, int id, Status st, How how, Time done){
    const PaymentRecord& r = e.payments()[id];
    bool ok = r.status == st && r.how == how && r.done_at == done;
    CHECK(ok);
    if(!ok) cerr << "  [" << label << "] payment " << id << ": status " << (int)r.status << " how " << (int)r.how
                 << " done_at " << r.done_at << ", expected " << (int)st << " " << (int)how << " " << done << "\n";
}

// returns every payment it is given: infeasible whenever some bank cannot pay all of its queue
class AllSolver : public GridlockSolver {
public:
    string name() const override { return "all"; }
    vector<int> solve(const Instance& inst) override {
        vector<int> v(inst.obs.size());
        iota(v.begin(), v.end(), 0);
        return v;
    }
};

// ------------------------------------------------------------------ independent reference (straight from the contract)
struct Ref {
    int n;
    vector<Amount> pos, cr;
    GridlockSolver& solver;
    EngineConfig c;
    Time now = 0, next_tick;
    bool closed = false;
    int batches = 0;
    vector<PaymentRecord> rec;

    Ref(vector<Amount> b, vector<Amount> l, GridlockSolver& s, EngineConfig cfg) : n((int)b.size()), pos(b), cr(l), solver(s), c(cfg){
        next_tick = c.batch_interval;
    }
    vector<int> queued(){
        vector<int> q;
        for(int i = 0; i < (int)rec.size(); i++) if(rec[i].status == Status::Queued) q.push_back(i);
        return q;
    }
    Amount qvalue(){
        Amount v = 0;
        for(int i : queued()) v += rec[i].amt;
        return v;
    }
    void settle(int id, How h){
        pos[rec[id].from] -= rec[id].amt;
        pos[rec[id].to]   += rec[id].amt;
        rec[id].status = Status::Settled;
        rec[id].how = h;
        rec[id].done_at = now;
    }
    void batch(){
        vector<int> q = queued();
        if(q.empty()) return;
        Instance inst;
        inst.n = n;
        for(int i = 0; i < n; i++){
            inst.balance.push_back(pos[i] + cr[i]);
            inst.credit_limit.push_back(0);
        }
        for(int id : q){
            Obligation o;
            o.from = rec[id].from;
            o.to = rec[id].to;
            o.amt = rec[id].amt;
            inst.obs.push_back(o);
        }
        vector<int> sel = solver.solve(inst);
        if(!validate_gridlock(inst, sel).ok) throw logic_error("bad batch");
        for(int k : sel) settle(q[k], How::Batch);
        batches++;
    }
    void recycle(int start){
        deque<int> wl = {start};
        vector<char> waiting(n, 0);
        waiting[start] = 1;
        while(!wl.empty()){
            int b = wl.front();
            wl.pop_front();
            waiting[b] = 0;
            for(int id = 0; id < (int)rec.size(); id++){
                if(rec[id].status != Status::Queued || rec[id].from != b) continue;
                if(pos[b] + cr[b] >= rec[id].amt){
                    settle(id, How::Recycle);
                    int v = rec[id].to;
                    if(!waiting[v]){ waiting[v] = 1; wl.push_back(v); }
                }
            }
        }
    }
    void advance(Time t){
        while(true){
            Time tk = c.batch_interval > 0 ? next_tick : NO_DEADLINE;
            Time dl = NO_DEADLINE;
            for(int id : queued()) dl = min(dl, rec[id].deadline);
            Time tau = min(tk, dl);
            if(tau > t || tau == NO_DEADLINE) break;
            bool b = (tk == tau) || (c.deadline_batch && dl == tau);
            if(tk == tau) next_tick += c.batch_interval;
            now = tau;
            if(b) batch();
            for(int id : queued()) if(rec[id].deadline <= tau){ rec[id].status = Status::Expired; rec[id].done_at = tau; }
        }
        now = t;
    }
    int submit(int from, int to, Amount amt, Time t, Time dl){
        advance(t);
        PaymentRecord r;
        r.from = from; r.to = to; r.amt = amt; r.arrival = t; r.deadline = dl;
        rec.push_back(r);
        int id = (int)rec.size() - 1;
        if(pos[from] + cr[from] >= amt){
            settle(id, How::Arrival);
            recycle(to);
        }else{
            vector<int> q = queued();
            if((c.queue_trigger > 0 && q.size() >= c.queue_trigger) || (c.value_trigger > 0 && qvalue() >= c.value_trigger)) batch();
        }
        return id;
    }
    void close(Time t){
        advance(t);
        batch();
        for(int id : queued()){ rec[id].status = Status::Unsettled; rec[id].done_at = t; }
        closed = true;
    }
};

static bool same_state(const SettlementEngine& e, Ref& r, string& why){
    if(e.payments().size() != r.rec.size()){ why = "different number of payments"; return false; }
    for(size_t i = 0; i < r.rec.size(); i++){
        if(!same_record(e.payments()[i], r.rec[i])){
            const PaymentRecord& a = e.payments()[i];
            const PaymentRecord& b = r.rec[i];
            why = "payment " + to_string(i) + ": status/how/done_at " + to_string((int)a.status) + "/" + to_string((int)a.how) + "/" +
                  to_string(a.done_at) + ", expected " + to_string((int)b.status) + "/" + to_string((int)b.how) + "/" + to_string(b.done_at);
            return false;
        }
    }
    if(e.positions() != r.pos){ why = "positions differ"; return false; }
    if(e.batches_run() != r.batches){ why = "batches_run " + to_string(e.batches_run()) + ", expected " + to_string(r.batches); return false; }
    if(e.queue_size() != r.queued().size()){ why = "queue_size differs"; return false; }
    if(e.queued_value() != r.qvalue()){ why = "queued_value differs"; return false; }
    if(e.now() != r.now){ why = "now() " + to_string(e.now()) + ", expected " + to_string(r.now); return false; }
    if(e.closed() != r.closed){ why = "closed() differs"; return false; }
    return true;
}

// ------------------------------------------------------------------ A. hand scenarios
static void test_arrival_and_recycle(){
    DropViolatorsSolver dv;
    {   // pay on arrival when affordable; otherwise queue; an incoming payment releases the queue
        SettlementEngine e({100, 0}, {0, 0}, dv);
        CHECK(e.submit(0, 1, 60, 1) == 0);
        expect_payment("arrival", e, 0, Status::Settled, How::Arrival, 1);
        CHECK(e.submit(0, 1, 50, 2) == 1);
        expect_payment("queued", e, 1, Status::Queued, How::None, -1);
        CHECK(e.queue_size() == 1 && e.queued_value() == 50);
        e.submit(1, 0, 20, 3);
        expect_payment("incoming", e, 2, Status::Settled, How::Arrival, 3);
        expect_payment("recycled", e, 1, Status::Settled, How::Recycle, 3);
        CHECK(e.positions() == vector<Amount>({10, 90}));
        CHECK(e.queue_size() == 0 && e.queued_value() == 0);
        CHECK(e.now() == 3);
        CHECK(e.batches_run() == 0);
    }
    {   // recycling cascades down a chain
        SettlementEngine e({0, 0, 0, 100}, {0, 0, 0, 0}, dv);
        e.submit(0, 1, 50, 1);
        e.submit(1, 2, 30, 2);
        e.submit(3, 0, 50, 3);
        expect_payment("cascade 0", e, 0, Status::Settled, How::Recycle, 3);
        expect_payment("cascade 1", e, 1, Status::Settled, How::Recycle, 3);
        expect_payment("cascade 2", e, 2, Status::Settled, How::Arrival, 3);
        CHECK(e.positions() == vector<Amount>({0, 20, 30, 50}));
    }
    {   // bypass: a big queued payment does not block smaller ones, on arrival or in recycling
        SettlementEngine e({30, 30}, {0, 0}, dv);
        e.submit(0, 1, 100, 1);                                   // id 0 queued
        e.submit(0, 1, 20, 2);                                    // id 1 pays past it
        e.submit(0, 1, 10, 3);                                    // id 2
        e.submit(0, 1, 12, 4);                                    // id 3 queued (bank 0 has 0)
        e.submit(1, 0, 15, 5);                                    // id 4: bank 0 gets 15 -> id 3 fits, id 0 does not
        expect_payment("bypass big", e, 0, Status::Queued, How::None, -1);
        expect_payment("bypass 20", e, 1, Status::Settled, How::Arrival, 2);
        expect_payment("bypass 10", e, 2, Status::Settled, How::Arrival, 3);
        expect_payment("bypass 12", e, 3, Status::Settled, How::Recycle, 5);
        CHECK(e.positions() == vector<Amount>({3, 57}));
    }
    {   // credit counts, exactly enough is enough
        SettlementEngine e({10, 0}, {40, 0}, dv);
        e.submit(0, 1, 50, 0);
        expect_payment("credit", e, 0, Status::Settled, How::Arrival, 0);
        CHECK(e.positions() == vector<Amount>({-40, 50}));
    }
}

static void test_batches(){
    DropViolatorsSolver dv;
    {   // periodic batch resolves a gridlock at the next tick, and only then
        EngineConfig c;
        c.batch_interval = 10;
        SettlementEngine e({0, 0, 0}, {0, 0, 0}, dv, c);
        e.submit(0, 1, 100, 1);
        e.submit(1, 2, 100, 2);
        e.submit(2, 0, 100, 3);
        e.advance_to(9);
        CHECK(e.queue_size() == 3 && e.batches_run() == 0);
        e.advance_to(10);
        for(int i = 0; i < 3; i++) expect_payment("tick", e, i, Status::Settled, How::Batch, 10);
        CHECK(e.batches_run() == 1);
        e.advance_to(1000000);                                    // empty queue: ticks do nothing and are not counted
        CHECK(e.batches_run() == 1 && e.now() == 1000000);
    }
    {   // a tick at time tau runs BEFORE a payment arriving at tau
        EngineConfig c;
        c.batch_interval = 10;
        SettlementEngine e({0, 0}, {0, 0}, dv, c);
        e.submit(0, 1, 10, 5);
        e.submit(1, 0, 10, 10);                                   // tick at 10 sees only payment 0: nothing settles
        CHECK(e.batches_run() == 1);
        CHECK(e.queue_size() == 2);
        e.advance_to(20);
        expect_payment("tie 0", e, 0, Status::Settled, How::Batch, 20);
        expect_payment("tie 1", e, 1, Status::Settled, How::Batch, 20);
        CHECK(e.batches_run() == 2);
    }
    {   // queue-size trigger fires right after the payment that fills the queue
        EngineConfig c;
        c.queue_trigger = 3;
        SettlementEngine e({0, 0, 0}, {0, 0, 0}, dv, c);
        e.submit(0, 1, 100, 1);
        e.submit(1, 2, 100, 2);
        CHECK(e.batches_run() == 0);
        e.submit(2, 0, 100, 3);
        for(int i = 0; i < 3; i++) expect_payment("queue trigger", e, i, Status::Settled, How::Batch, 3);
        CHECK(e.batches_run() == 1);
    }
    {   // value trigger
        EngineConfig c;
        c.value_trigger = 250;
        SettlementEngine e({0, 0, 0}, {0, 0, 0}, dv, c);
        e.submit(0, 1, 100, 1);
        e.submit(1, 2, 100, 2);
        CHECK(e.batches_run() == 0);
        e.submit(2, 0, 100, 3);
        CHECK(e.batches_run() == 1 && e.queue_size() == 0);
    }
    {   // a trigger only fires when a payment is QUEUED, and an empty-handed batch still counts
        EngineConfig c;
        c.queue_trigger = 1;
        SettlementEngine e({50, 0}, {0, 0}, dv, c);
        e.submit(0, 1, 10, 1);                                    // settles on arrival: no batch
        CHECK(e.batches_run() == 0);
        e.submit(1, 0, 999, 2);                                   // queued: batch runs, cannot settle it
        CHECK(e.batches_run() == 1 && e.queue_size() == 1);
    }
    {   // the solver's answer is checked
        AllSolver bad;
        EngineConfig c;
        c.queue_trigger = 1;
        SettlementEngine e({0, 0}, {0, 0}, bad, c);
        CHECK(throws<logic_error>([&]{ e.submit(0, 1, 5, 1); }));
    }
}

static void test_deadlines_and_close(){
    DropViolatorsSolver dv;
    {   // expiry at the deadline
        SettlementEngine e({0, 0}, {0, 0}, dv);
        e.submit(0, 1, 10, 1, 5);
        e.advance_to(4);
        expect_payment("not yet", e, 0, Status::Queued, How::None, -1);
        e.advance_to(5);
        expect_payment("expired", e, 0, Status::Expired, How::None, 5);
        CHECK(e.queue_size() == 0 && e.queued_value() == 0);
        e.submit(0, 1, 10, 7, 7);                                 // deadline == arrival: expires at the next advance
        expect_payment("same time", e, 1, Status::Queued, How::None, -1);
        e.advance_to(7);
        expect_payment("same time, expired", e, 1, Status::Expired, How::None, 7);
    }
    {   // an expired payment can no longer be released by money arriving later
        SettlementEngine e({0, 0}, {0, 0}, dv);
        e.submit(0, 1, 10, 1, 3);
        e.submit(1, 0, 10, 4);                                    // bank 1 has nothing either: queued
        expect_payment("expired before funding", e, 0, Status::Expired, How::None, 3);
    }
    {   // deadline batch: last chance before expiry
        EngineConfig c;
        c.deadline_batch = true;
        SettlementEngine e({0, 0}, {0, 0}, dv, c);
        e.submit(0, 1, 10, 1, 5);
        e.submit(1, 0, 10, 2, 5);
        e.advance_to(5);
        expect_payment("deadline batch 0", e, 0, Status::Settled, How::Batch, 5);
        expect_payment("deadline batch 1", e, 1, Status::Settled, How::Batch, 5);
        CHECK(e.batches_run() == 1);
    }
    {   // without it, both just expire
        SettlementEngine e({0, 0}, {0, 0}, dv);
        e.submit(0, 1, 10, 1, 5);
        e.submit(1, 0, 10, 2, 5);
        e.advance_to(5);
        expect_payment("no deadline batch", e, 0, Status::Expired, How::None, 5);
        CHECK(e.batches_run() == 0);
    }
    {   // tick and deadline at the same time: ONE batch
        EngineConfig c;
        c.batch_interval = 5;
        c.deadline_batch = true;
        SettlementEngine e({0, 0}, {0, 0}, dv, c);
        e.submit(0, 1, 10, 1, 5);
        e.advance_to(5);
        CHECK(e.batches_run() == 1);
        expect_payment("tick + deadline", e, 0, Status::Expired, How::None, 5);
    }
    {   // close: final batch, leftovers unsettled, then frozen
        SettlementEngine e({0, 0, 0}, {0, 0, 0}, dv);
        e.submit(0, 1, 10, 1);
        e.submit(1, 0, 10, 2);
        e.submit(2, 0, 5, 3);
        e.close(20);
        expect_payment("close 0", e, 0, Status::Settled, How::Batch, 20);
        expect_payment("close 1", e, 1, Status::Settled, How::Batch, 20);
        expect_payment("close 2", e, 2, Status::Unsettled, How::None, 20);
        CHECK(e.closed() && e.now() == 20 && e.batches_run() == 1 && e.queue_size() == 0);
        CHECK(throws<logic_error>([&]{ e.submit(0, 1, 1, 30); }));
        CHECK(throws<logic_error>([&]{ e.advance_to(30); }));
        CHECK(throws<logic_error>([&]{ e.close(30); }));
        CHECK(e.payments().size() == 3);
    }
}

static void test_stats(){
    DropViolatorsSolver dv;
    EngineConfig c;
    c.batch_interval = 10;
    SettlementEngine e({100, 0, 0, 0}, {0, 0, 0, 0}, dv, c);
    e.submit(0, 3, 30, 0);                                        // arrival, delay 0
    e.submit(0, 3, 30, 1);                                        // arrival, delay 0
    e.submit(1, 2, 70, 1);                                        // cycle 1 -> 2 -> 3 -> 1, settles at 10: delays 9, 8, 7
    e.submit(2, 3, 70, 2);
    e.submit(3, 1, 70, 3);
    e.submit(2, 0, 500, 4, 6);                                    // expires at 6
    e.submit(1, 0, 900, 5);                                       // left at close
    e.close(15);
    EngineStats s = e.stats();
    CHECK(s.submitted == 7 && s.settled == 5 && s.by_arrival == 2 && s.by_recycle == 0 && s.by_batch == 3);
    CHECK(s.expired == 1 && s.expired_value == 500);
    CHECK(s.unsettled == 1 && s.unsettled_value == 900);
    CHECK(s.queued == 0 && s.queued_value == 0);
    CHECK(s.settled_value == 30 + 30 + 210);
    CHECK(s.batches == 2);                                        // tick at 10, close at 15
    CHECK(s.delay_p50 == 7 && s.delay_p95 == 9 && s.delay_p99 == 9 && s.delay_max == 9);   // delays 0 0 7 8 9

    // 10 delays 0..9: nearest rank p50 = d[ceil(5) - 1] = d[4] = 4 (not d[5])
    SettlementEngine r({0, 0, 100}, {0, 0, 0}, dv);
    for(Time t = 1; t <= 9; t++) r.submit(0, 1, 1, t);
    r.submit(2, 0, 9, 10);                                        // releases all 9 by recycling at 10
    EngineStats q = r.stats();
    CHECK(q.settled == 10 && q.by_arrival == 1 && q.by_recycle == 9);
    CHECK(q.delay_p50 == 4 && q.delay_p95 == 9 && q.delay_p99 == 9 && q.delay_max == 9);

    SettlementEngine empty({1}, {0}, dv);
    EngineStats z = empty.stats();
    CHECK(z.submitted == 0 && z.settled == 0 && z.delay_p50 == 0 && z.delay_max == 0 && z.batches == 0);
}

static void test_bad_input(){
    DropViolatorsSolver dv;
    CHECK(throws<invalid_argument>([&]{ SettlementEngine e({}, {}, dv); }));
    CHECK(throws<invalid_argument>([&]{ SettlementEngine e({1, 2}, {0}, dv); }));
    CHECK(throws<invalid_argument>([&]{ SettlementEngine e({-1}, {0}, dv); }));
    CHECK(throws<invalid_argument>([&]{ SettlementEngine e({1}, {-1}, dv); }));
    CHECK(throws<invalid_argument>([&]{ EngineConfig c; c.batch_interval = -1; SettlementEngine e({1}, {0}, dv, c); }));
    CHECK(throws<invalid_argument>([&]{ EngineConfig c; c.value_trigger = -1; SettlementEngine e({1}, {0}, dv, c); }));

    SettlementEngine e({10, 10}, {0, 0}, dv);
    e.advance_to(10);
    CHECK(throws<invalid_argument>([&]{ e.submit(0, 0, 1, 10); }));
    CHECK(throws<invalid_argument>([&]{ e.submit(0, 2, 1, 10); }));
    CHECK(throws<invalid_argument>([&]{ e.submit(-1, 1, 1, 10); }));
    CHECK(throws<invalid_argument>([&]{ e.submit(0, 1, 0, 10); }));
    CHECK(throws<invalid_argument>([&]{ e.submit(0, 1, -5, 10); }));
    CHECK(throws<invalid_argument>([&]{ e.submit(0, 1, 1, 9); }));
    CHECK(throws<invalid_argument>([&]{ e.submit(0, 1, 1, 12, 11); }));
    CHECK(throws<invalid_argument>([&]{ e.advance_to(9); }));
    CHECK(e.payments().empty() && e.now() == 10);                 // nothing changed
    CHECK(e.positions() == vector<Amount>({10, 10}));
    CHECK(e.submit(0, 1, 1, 10) == 0);                            // same time is fine
}

// ------------------------------------------------------------------ B. random streams vs the reference
static void test_vs_reference(bool fast){
    FifoBypassSolver fifo; DropViolatorsSolver dv; MaxflowRelaxSolver rx; BranchBoundSolver bb;
    vector<GridlockSolver*> solvers = {&fifo, &dv, &rx, &bb};

    int seeds = fast ? 800 : 3000;
    int bad = 0;
    long long ops = 0;
    for(int seed = 0; seed < seeds; seed++){
        Rng rng(31000 + seed);
        int n = (int)rng.uniform(2, 6);
        vector<Amount> bal(n), cr(n);
        for(int i = 0; i < n; i++){
            bal[i] = rng.uniform(0, 2) == 0 ? 0 : rng.uniform(0, 60);
            cr[i] = rng.uniform(0, 3) == 0 ? rng.uniform(0, 20) : 0;
        }
        EngineConfig c;
        if(rng.uniform(0, 1)) c.batch_interval = rng.uniform(1, 15);
        if(rng.uniform(0, 2) == 0) c.queue_trigger = (size_t)rng.uniform(1, 6);
        if(rng.uniform(0, 2) == 0) c.value_trigger = rng.uniform(30, 300);
        c.deadline_batch = rng.uniform(0, 1);
        GridlockSolver& s = *solvers[rng.uniform(0, 3)];

        SettlementEngine e(bal, cr, s, c);
        Ref r(bal, cr, s, c);
        string why;
        bool ok = true;
        Time t = 0;
        int steps = (int)rng.uniform(0, 40);
        for(int k = 0; k < steps && ok; k++){
            t += rng.uniform(0, 4);
            if(rng.uniform(0, 5) == 0){
                e.advance_to(t);
                r.advance(t);
            }else{
                int from = (int)rng.uniform(0, n - 1);
                int to = (int)rng.uniform(0, n - 2);
                if(to >= from) to++;
                Amount amt = rng.uniform(1, 60);
                Time dl = rng.uniform(0, 2) == 0 ? t + rng.uniform(0, 20) : NO_DEADLINE;
                int id = e.submit(from, to, amt, t, dl);
                int rid = r.submit(from, to, amt, t, dl);
                if(id != rid){ ok = false; why = "submit returned the wrong id"; }
            }
            ops++;
            if(ok) ok = same_state(e, r, why);
        }
        if(ok){
            t += rng.uniform(0, 30);
            e.close(t);
            r.close(t);
            ok = same_state(e, r, why);
        }
        if(!ok && bad++ < 5) cerr << "  seed " << seed << " (solver " << s.name() << ", interval " << c.batch_interval << ", queue_trigger "
                                  << c.queue_trigger << ", value_trigger " << c.value_trigger << ", deadline_batch " << c.deadline_batch
                                  << "): " << why << "\n";
    }
    CHECK(bad == 0);
    if(bad) cout << "engine vs reference: " << bad << " of " << seeds << " streams differ\n";
    else cout << "engine on " << seeds << " random streams (" << ops << " operations): identical to the reference after every step\n";
}

// ------------------------------------------------------------------ C. invariants on longer streams
static void test_invariants(bool fast){
    DropViolatorsSolver dv; MaxflowRelaxSolver rx;
    int streams = fast ? 6 : 20;
    for(int seed = 0; seed < streams; seed++){
        Rng rng(32000 + seed);
        int n = 40;
        vector<Amount> bal(n), cr(n);
        for(int i = 0; i < n; i++){
            bal[i] = rng.uniform(0, 5000);
            cr[i] = rng.uniform(0, 1) ? rng.uniform(0, 2000) : 0;
        }
        EngineConfig c;
        c.batch_interval = rng.uniform(20, 200);
        c.queue_trigger = seed % 2 ? 50 : 0;
        c.deadline_batch = seed % 3 == 0;
        SettlementEngine e(bal, cr, seed % 2 ? (GridlockSolver&)dv : (GridlockSolver&)rx, c);

        Amount total = 0;
        for(Amount b : bal) total += b;
        Instance day;
        day.n = n;
        day.balance = bal;
        day.credit_limit = cr;
        Time t = 0;
        bool ok = true;
        for(int k = 0; k < 3000; k++){
            t += rng.uniform(0, 3);
            int from = (int)rng.uniform(0, n - 1);
            int to = (int)rng.uniform(0, n - 2);
            if(to >= from) to++;
            Amount amt = rng.uniform(1, 3000);
            Time dl = rng.uniform(0, 3) == 0 ? t + rng.uniform(0, 300) : NO_DEADLINE;
            e.submit(from, to, amt, t, dl);
            Obligation o;
            o.from = from; o.to = to; o.amt = amt;
            day.obs.push_back(o);

            Amount sum = 0;
            for(int i = 0; i < n; i++){
                if(e.positions()[i] + cr[i] < 0) ok = false;
                sum += e.positions()[i];
            }
            if(sum != total) ok = false;
        }
        e.close(t + 1000);
        CHECK(ok);                                               // nobody below its limit, money conserved, at every step

        vector<int> settled;
        bool times_ok = true;
        for(int id = 0; id < (int)e.payments().size(); id++){
            const PaymentRecord& r = e.payments()[id];
            if(r.status == Status::Queued) times_ok = false;
            if(r.status == Status::Settled){
                settled.push_back(id);
                if(r.done_at < r.arrival || r.done_at > r.deadline) times_ok = false;
            }
            if(r.status == Status::Expired && r.done_at != r.deadline) times_ok = false;
            if(r.status != Status::Settled && r.how != How::None) times_ok = false;
        }
        CHECK(times_ok);

        // the whole day's settled set is feasible as one offline batch, so it can never beat the offline bound
        GridlockCheck g = validate_gridlock(day, settled);
        CHECK(g.ok);
        EngineStats s = e.stats();
        CHECK(g.settled_value == s.settled_value);
        CHECK(s.settled_value <= gridlock_lp_bound(day).value);
        CHECK(s.settled + s.expired + s.unsettled == s.submitted);
        CHECK(s.by_arrival + s.by_recycle + s.by_batch == s.settled);
    }
    cout << "engine on " << streams << " streams of 3000 payments: limits respected and money conserved at every step, "
         << "online value <= offline LP bound\n";
}

// ------------------------------------------------------------------ D. throughput
static void test_scale(bool fast){
    using clk = chrono::steady_clock;
    int n = 1000, m = fast ? 50000 : 200000;
    Rng rng(4242);
    vector<Amount> bal(n), cr(n, 0);
    for(int i = 0; i < n; i++) bal[i] = rng.uniform(10000, 200000);
    DropViolatorsSolver dv;
    EngineConfig c;
    c.batch_interval = 500;
    c.deadline_batch = true;
    SettlementEngine e(bal, cr, dv, c);

    auto t0 = clk::now();
    Time t = 0;
    for(int k = 0; k < m; k++){
        t += rng.uniform(0, 2);
        int from = (int)rng.uniform(0, n - 1);
        int to = (int)rng.uniform(0, n - 2);
        if(to >= from) to++;
        Amount amt = rng.uniform(0, 9) == 0 ? rng.uniform(50000, 400000) : rng.uniform(100, 20000);
        Time dl = rng.uniform(0, 4) == 0 ? t + rng.uniform(100, 3000) : NO_DEADLINE;
        e.submit(from, to, amt, t, dl);
    }
    e.close(t + 1);
    double ms = chrono::duration<double, milli>(clk::now() - t0).count();
    EngineStats s = e.stats();
    CHECK(s.submitted == (size_t)m);
#ifdef NDEBUG
    CHECK(ms < 20000);                                            // optimised builds only: sanitizers are ~15x slower
#endif
    cout << fixed << setprecision(1);
    cout << "engine: " << m << " payments, " << n << " banks in " << ms << " ms (" << (double)m / ms * 1000.0 << " payments/s), "
         << s.batches << " batches\n"
         << "  settled " << 100.0 * s.settled / m << "% (arrival " << s.by_arrival << ", recycle " << s.by_recycle << ", batch "
         << s.by_batch << "), expired " << s.expired << ", unsettled " << s.unsettled << "; delay p50 " << s.delay_p50
         << ", p95 " << s.delay_p95 << ", p99 " << s.delay_p99 << ", max " << s.delay_max << "\n";
    cout.unsetf(ios::fixed);
}

int main(int argc, char** argv){
    bool fast = (argc > 1 && string(argv[1]) == "--fast");

    test_arrival_and_recycle();
    test_batches();
    test_deadlines_and_close();
    test_stats();
    test_bad_input();
    test_vs_reference(fast);
    test_invariants(fast);
    test_scale(fast);

    if(failures){
        cerr << failures << " check(s) failed\n";
        return 1;
    }
    cout << "all tests passed\n";
    return 0;
}