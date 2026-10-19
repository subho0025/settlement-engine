#include "core/io.hpp"
#include "core/validator.hpp"
#include "gridlock/gridlock.hpp"
#include "gridlock/fifo_bypass.hpp"
#include "gridlock/drop_violators.hpp"
#include "brute/brute_gridlock.hpp"
#include "core/rng.hpp"

using namespace std;

// Contract used by this file:
//   class DropViolatorsSolver : public GridlockSolver   (name "drop_violators", solve() -> ascending indices)
// The algorithm is pinned down exactly, so hand cases check exact index sets:
//   phase 1  start with ALL payments; worklist = every bank in id order; pop bank v; while v would end below its limit,
//            drop v's LAST (highest index) remaining outgoing payment, and queue the payee (if not queued already)
//   phase 2  repeated passes in queue order: put a dropped payment back when its payer has enough slack left

static int failures = 0;

#define CHECK(cond) \
    do { if(!(cond)) { cerr << __FILE__ << ":" << __LINE__ << "  CHECK failed: " #cond "\n"; failures++; } } while(0)

// ------------------------------------------------------------------ helpers
static Instance make(const vector<array<long long,2>>& bank, const vector<array<long long,3>>& pay){
    Instance inst;
    inst.n = (int)bank.size();
    for(const auto& b : bank){
        inst.balance.push_back(b[0]);
        inst.credit_limit.push_back(b[1]);
    }
    for(const auto& p : pay){
        Obligation o;
        o.from = p[0];
        o.to = p[1];
        o.amt = p[2];
        inst.obs.push_back(o);
    }
    return inst;
}

static Instance random_gridlock(Rng& rng, int n, int m, Amount A){
    Instance inst;
    inst.n = n;
    for(int i = 0; i < n; i++){
        inst.balance.push_back(rng.uniform(0, 2) == 0 ? 0 : rng.uniform(0, A));
        inst.credit_limit.push_back(rng.uniform(0, 3) == 0 ? rng.uniform(0, A / 2) : 0);
    }
    for(int p = 0; p < m; p++){
        Obligation o;
        o.from = rng.uniform(0, n - 1);
        o.to = rng.uniform(0, n - 2);
        if(o.to >= o.from) o.to++;
        o.amt = rng.uniform(1, A);
        inst.obs.push_back(o);
    }
    return inst;
}

// slack of every bank after settling `sel`: balance + credit_limit + incoming - outgoing
static vector<__int128> slack_after(const Instance& inst, const vector<int>& sel){
    vector<__int128> s(inst.n);
    for(int i = 0; i < inst.n; i++) s[i] = (__int128)inst.balance[i] + inst.credit_limit[i];
    for(int p : sel){
        s[inst.obs[p].from] -= inst.obs[p].amt;
        s[inst.obs[p].to]   += inst.obs[p].amt;
    }
    return s;
}

// every payment left out must be unaffordable for its payer: nothing more can be added
static bool is_maximal(const Instance& inst, const vector<int>& sel){
    vector<__int128> s = slack_after(inst, sel);
    vector<char> in_sel(inst.obs.size(), 0);
    for(int p : sel) in_sel[p] = 1;
    for(size_t p = 0; p < inst.obs.size(); p++){
        if(!in_sel[p] && s[inst.obs[p].from] >= inst.obs[p].amt) return false;
    }
    return true;
}

static void expect_set(const char* label, const Instance& inst, const vector<int>& got, const vector<int>& want){
    CHECK(got == want);
    if(got != want){
        cerr << "  [" << label << "] expected {";
        for(int x : want) cerr << " " << x;
        cerr << " } got {";
        for(int x : got) cerr << " " << x;
        cerr << " }\n";
    }
    CHECK(validate_gridlock(inst, got).ok);
}

// ------------------------------------------------------------------ A. hand cases with exact answers
static void test_hand_cases(){
    DropViolatorsSolver dv;
    CHECK(dv.name() == "drop_violators");

    // gridlocks the sequential baseline cannot resolve
    expect_set("cycle",   make({{0,0},{0,0}}, {{0,1,10},{1,0,10}}),
               dv.solve(make({{0,0},{0,0}}, {{0,1,10},{1,0,10}})), {0, 1});
    expect_set("partial", make({{0,0},{6,0}}, {{0,1,5},{1,0,10}}),
               dv.solve(make({{0,0},{6,0}}, {{0,1,5},{1,0,10}})), {0, 1});

    {   // 3-cycle plus an extra payment that breaks bank 0: only the extra one (the latest of bank 0) is dropped
        Instance inst = make({{0,0},{0,0},{0,0}}, {{0,1,10},{1,2,10},{2,0,10},{0,2,1}});
        expect_set("cycle + extra", inst, dv.solve(inst), {0, 1, 2});
    }
    {   // bank 0 has 4: both payments are dropped, then the smaller one fits back
        Instance inst = make({{4,0},{0,0},{0,0}}, {{0,1,5},{0,2,3}});
        expect_set("drop then re-add", inst, dv.solve(inst), {1});
    }
    {   // everything affordable already: nothing is dropped
        Instance inst = make({{5,0},{0,0},{0,0}}, {{0,1,5},{1,2,5}});
        expect_set("all feasible", inst, dv.solve(inst), {0, 1});
    }
    {   // the extra payment at the end of the queue is the one that goes
        Instance inst = make({{5,0},{0,0},{0,0}}, {{0,1,5},{1,2,5},{0,1,3}});
        expect_set("last one goes", inst, dv.solve(inst), {0, 1});
    }
    {   // dropping one payment starves the next bank, which starves bank 0 in turn: everything goes
        Instance inst = make({{0,0},{0,0},{0,0}}, {{1,0,5},{0,2,5}});
        expect_set("cascade", inst, dv.solve(inst), {});
    }
    {   // exactly enough liquidity
        Instance inst = make({{7,0},{0,0}}, {{0,1,7}});
        expect_set("exact", inst, dv.solve(inst), {0});
    }
    {   // credit limit is spendable
        Instance inst = make({{0,5},{0,0}}, {{0,1,5}});
        expect_set("credit", inst, dv.solve(inst), {0});
    }
    {   // empty queue
        Instance inst = make({{1,1}}, {});
        expect_set("empty", inst, dv.solve(inst), {});
    }
}

// ------------------------------------------------------------------ B. where the rule loses: it drops the LAST payment, not the cheapest loss
static void test_known_gap(){
    DropViolatorsSolver dv;
    // bank 1 owes 1 then 6 but has only 6. Dropping the last payment keeps the 1; the best choice is the 6.
    Instance inst = make({{5,0},{6,0}}, {{1,0,1},{1,0,6}});
    vector<int> sel = dv.solve(inst);
    expect_set("known gap", inst, sel, {0});
    GridlockCheck r = validate_gridlock(inst, sel);
    CHECK(r.settled_value == 1);
    CHECK(brute_gridlock(inst).best_value == 6);
}

// ------------------------------------------------------------------ C. properties on random instances
static void test_properties(){
    DropViolatorsSolver dv; FifoBypassSolver fifo;
    const Amount As[3] = {5, 20, 1000000000000LL};

    int total = 0, below_opt = 0, dv_better = 0, dv_worse = 0, dv_equal = 0;

    for(int seed = 0; seed < 400; seed++){
        Rng rng(6000 + seed);
        int n = (int)rng.uniform(2, 7), m = (int)rng.uniform(0, 14);
        Instance inst = random_gridlock(rng, n, m, As[rng.uniform(0, 2)]);

        vector<int> sel = dv.solve(inst);
        GridlockCheck r = validate_gridlock(inst, sel);
        CHECK(r.ok);                                        // simultaneous settlement is feasible
        if(!r.ok) cerr << "  seed " << seed << ": " << r.error << "\n";
        CHECK(is_maximal(inst, sel));                       // nothing dropped can still be added
        CHECK(dv.solve(inst) == sel);                       // deterministic

        BruteGridlock opt = brute_gridlock(inst);
        CHECK(r.settled_value <= opt.best_value);

        // a feasible selection is a fixed point: solving the instance restricted to it returns all of it
        {
            Instance sub = inst;
            sub.obs.clear();
            for(int p : sel) sub.obs.push_back(inst.obs[p]);
            vector<int> again = dv.solve(sub);
            CHECK(again.size() == sel.size());
        }

        Amount fv = validate_gridlock(inst, fifo.solve(inst)).settled_value;
        total++;
        if(r.settled_value < opt.best_value) below_opt++;
        if(r.settled_value > fv) dv_better++;
        else if(r.settled_value < fv) dv_worse++;
        else dv_equal++;
    }
    cout << "drop_violators on " << total << " small instances: below the optimum on " << below_opt
         << "; vs fifo_bypass better on " << dv_better << ", equal on " << dv_equal << ", worse on " << dv_worse << "\n";
    CHECK(dv_better > 0);          // it does resolve gridlocks the baseline cannot
}

// ------------------------------------------------------------------ D. instances that are feasible as a whole
static void test_all_feasible(){
    DropViolatorsSolver dv; FifoBypassSolver fifo;
    int fifo_stuck = 0;

    for(int seed = 0; seed < 200; seed++){
        Rng rng(7000 + seed);
        int n = (int)rng.uniform(2, 10), m = (int)rng.uniform(0, 25);
        Instance inst = random_gridlock(rng, n, m, 50);

        // give every bank exactly the liquidity its net outflow needs: the whole queue is feasible
        vector<__int128> net(n, 0);
        for(const Obligation& o : inst.obs){ net[o.from] -= o.amt; net[o.to] += o.amt; }
        for(int i = 0; i < n; i++){
            inst.credit_limit[i] = 0;
            inst.balance[i] = net[i] < 0 ? (Amount)(-net[i]) : 0;
        }

        vector<int> all(m);
        iota(all.begin(), all.end(), 0);
        CHECK(validate_gridlock(inst, all).ok);

        CHECK(dv.solve(inst) == all);                       // nothing to drop
        if(fifo.solve(inst).size() < all.size()) fifo_stuck++;
    }
    cout << "feasible-as-a-whole instances: the sequential baseline got stuck on " << fifo_stuck << " of 200\n";
    CHECK(fifo_stuck > 0);
}

// ------------------------------------------------------------------ E. scale
static void test_scale(bool fast){
    using clk = chrono::steady_clock;
    auto ms = [](clk::time_point a, clk::time_point b){
        return chrono::duration_cast<chrono::milliseconds>(b - a).count();
    };
    DropViolatorsSolver dv; FifoBypassSolver fifo;

    {   // every bank starts with nothing; 400k payment pairs (a->b and b->a, same amount) queue first, 200k one-way payments last.
        // Only the pairs can settle, and only together.
        int n = fast ? 20000 : 100000;
        int pairs = fast ? 80000 : 400000;
        int noise = fast ? 40000 : 200000;
        Rng rng(99);
        Instance inst;
        inst.n = n;
        inst.balance.assign(n, 0);
        inst.credit_limit.assign(n, 0);
        Amount pair_value = 0;
        for(int i = 0; i < pairs; i++){
            Obligation a;
            a.from = rng.uniform(0, n - 1);
            a.to = rng.uniform(0, n - 2);
            if(a.to >= a.from) a.to++;
            a.amt = rng.uniform(1, 1000000);
            Obligation b = a;
            swap(b.from, b.to);
            inst.obs.push_back(a);
            inst.obs.push_back(b);
            pair_value += 2 * a.amt;
        }
        for(int i = 0; i < noise; i++){
            Obligation o;
            o.from = rng.uniform(0, n - 1);
            o.to = rng.uniform(0, n - 2);
            if(o.to >= o.from) o.to++;
            o.amt = rng.uniform(1, 1000000);
            inst.obs.push_back(o);
        }

        auto t0 = clk::now();
        vector<int> sel = dv.solve(inst);
        auto t1 = clk::now();
        GridlockCheck r = validate_gridlock(inst, sel);
        CHECK(r.ok);
        CHECK(r.settled_value >= pair_value);                 // all pairs survive (late noise is dropped first)
        CHECK(is_maximal(inst, sel));

        vector<int> fs = fifo.solve(inst);
        CHECK(fs.empty());                                    // nobody can start: the baseline settles nothing
        cout << "drop_violators on " << n << " banks, " << inst.obs.size() << " payments (all balances 0): settled "
             << r.settled_count << " payments, value " << r.settled_value << " (pairs alone: " << pair_value << "), "
             << ms(t0, t1) << " ms; fifo_bypass settled " << fs.size() << "\n";
    }

    if(!fast){   // same random instance as the FIFO scale test
        Rng rng(77);
        Instance inst;
        inst.n = 100000;
        for(int i = 0; i < inst.n; i++){
            inst.balance.push_back(rng.uniform(0, 3000000));
            inst.credit_limit.push_back(0);
        }
        for(int p = 0; p < 1000000; p++){
            Obligation o;
            o.from = rng.uniform(0, inst.n - 1);
            o.to = rng.uniform(0, inst.n - 2);
            if(o.to >= o.from) o.to++;
            o.amt = rng.uniform(1, 1000000);
            inst.obs.push_back(o);
        }
        auto t0 = clk::now();
        vector<int> sel = dv.solve(inst);
        auto t1 = clk::now();
        GridlockCheck r = validate_gridlock(inst, sel);
        CHECK(r.ok);
        CHECK(is_maximal(inst, sel));
        GridlockCheck f = validate_gridlock(inst, fifo.solve(inst));
        cout << "random n=1e5 m=1e6: drop_violators settled " << r.settled_count << " (value " << r.settled_value << "), "
             << ms(t0, t1) << " ms; fifo_bypass settled " << f.settled_count << " (value " << f.settled_value << ")\n";
    }
}

int main(int argc, char** argv){
    bool fast = (argc > 1 && string(argv[1]) == "--fast");

    test_hand_cases();
    test_known_gap();
    test_properties();
    test_all_feasible();
    test_scale(fast);

    if(failures){
        cerr << failures << " check(s) failed\n";
        return 1;
    }
    cout << "all tests passed\n";
    return 0;
}