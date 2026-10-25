#include "core/validator.hpp"
#include "gridlock/lp_bound.hpp"
#include "gridlock/fifo_bypass.hpp"
#include "gridlock/drop_violators.hpp"
#include "gridlock/branch_bound.hpp"
#include "gridlock/maxflow_relax.hpp"
#include "brute/brute_gridlock.hpp"
#include "core/rng.hpp"

using namespace std;

// Contract used by this file (includes/gridlock/lp_bound.hpp, src/gridlock/lp_bound.cpp):
//
//   struct LpBound { Amount value; Amount total; vector<Amount> unsettled; };
//   LpBound gridlock_lp_bound(const Instance& inst)
//
//   The LP relaxation of gridlock resolution: every payment p may settle any amount x_p in [0, amt_p] (not just all or
//   nothing), every bank must end with  balance + credit_limit + in(x) - out(x) >= 0,  maximise sum of x_p.
//       total      = sum of amt_p over all payments
//       unsettled  = y_p = amt_p - x_p of an OPTIMAL fractional solution, one entry per payment (size m)
//       value      = total - sum of y_p  = the LP optimum
//   value is an upper bound on what ANY all-or-nothing solver can settle. Which optimal y is returned is not pinned
//   down (it is often not unique), so y is checked with an optimality certificate, not compared entry by entry.
//
//   Precondition: inst is valid as read_gridlock_instance guarantees (sizes, endpoints, no self-loops, balance and
//   credit_limit >= 0). Must be fast on large sparse instances (section G): solve each connected component on its own.
//
// Two independent oracles, neither shares code with MinCostFlow:
//   brute_lp      the constraint matrix is a network matrix (totally unimodular), so the LP has an INTEGER optimal
//                 solution; on tiny instances we can enumerate every integer x and take the best.
//   certificate   a feasible y is optimal iff (1) no bank unwinds more than it must and (2) the residual graph of the
//                 unwinding network has no negative-cost cycle. Checked with Bellman-Ford.

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

static Instance random_gridlock(Rng& rng, int n, int m, Amount A, Amount B){
    Instance inst;
    inst.n = n;
    for(int i = 0; i < n; i++){
        inst.balance.push_back(rng.uniform(0, 2) == 0 ? 0 : rng.uniform(0, B));
        inst.credit_limit.push_back(rng.uniform(0, 3) == 0 ? rng.uniform(0, B / 2) : 0);
    }
    if(n < 2) return inst;
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

static Amount total_of(const Instance& inst){
    Amount t = 0;
    for(const Obligation& o : inst.obs) t += o.amt;
    return t;
}

static Amount sum_of(const vector<Amount>& v){
    Amount s = 0;
    for(Amount x : v) s += x;
    return s;
}

// LP optimum by enumerating every integer x (exact because the LP has an integral optimum)
static Amount brute_lp(const Instance& inst){
    int m = (int)inst.obs.size();
    vector<Amount> x(m, 0);
    Amount best = -1;
    while(true){
        vector<__int128> pos(inst.n);
        for(int i = 0; i < inst.n; i++) pos[i] = (__int128)inst.balance[i] + inst.credit_limit[i];
        Amount val = 0;
        for(int p = 0; p < m; p++){
            pos[inst.obs[p].from] -= x[p];
            pos[inst.obs[p].to]   += x[p];
            val += x[p];
        }
        bool ok = true;
        for(int i = 0; i < inst.n; i++) if(pos[i] < 0) ok = false;
        if(ok) best = max(best, val);

        int k = 0;                                      // next x in mixed radix
        while(k < m && x[k] == inst.obs[k].amt){ x[k] = 0; k++; }
        if(k == m) break;
        x[k]++;
    }
    return best;
}

// Full check of an LpBound result. Returns false with a reason on the first problem.
static bool lp_ok(const Instance& inst, const LpBound& r, string& why){
    int n = inst.n, m = (int)inst.obs.size();
    if((int)r.unsettled.size() != m){ why = "unsettled has size " + to_string(r.unsettled.size()) + ", expected " + to_string(m); return false; }
    if(r.total != total_of(inst)){ why = "total != sum of amounts"; return false; }

    vector<__int128> cap(n), need(n), e(n, 0);          // e_i = out(y) - in(y): what bank i unwinds
    __int128 sum_y = 0;
    for(int i = 0; i < n; i++) cap[i] = (__int128)inst.balance[i] + inst.credit_limit[i];
    for(int i = 0; i < n; i++) need[i] = -cap[i];
    for(int p = 0; p < m; p++){
        const Obligation& o = inst.obs[p];
        Amount y = r.unsettled[p];
        if(y < 0 || y > o.amt){ why = "unsettled[" + to_string(p) + "] = " + to_string(y) + " outside [0, amt]"; return false; }
        need[o.from] += o.amt;
        need[o.to]   -= o.amt;
        e[o.from] += y;
        e[o.to]   -= y;
        sum_y += y;
    }
    if((__int128)r.value != (__int128)r.total - sum_y){ why = "value != total - sum of unsettled"; return false; }

    // feasibility: settling x = amt - y leaves every bank >= 0   <=>   e_i >= need_i
    for(int i = 0; i < n; i++){
        if(e[i] < need[i]){ why = "bank " + to_string(i) + " ends below its limit in the fractional solution"; return false; }
    }
    // (1) an optimal y never makes a bank unwind more than it must (cutting the excess path would lower sum y)
    for(int i = 0; i < n; i++){
        if(e[i] > max(need[i], (__int128)0)){ why = "bank " + to_string(i) + " unwinds more than it needs: not optimal"; return false; }
    }
    // (2) no negative cycle in the residual graph. Nodes 0..n-1 banks, n = sink.
    //     payment u->v: residual u->v cost +1 if y < amt (unwind more), v->u cost -1 if y > 0 (unwind less)
    //     bank i with need_i < 0 absorbs a_i = -e_i in [0, -need_i]: i->sink cost 0 if a_i < -need_i, sink->i if a_i > 0
    //     (banks with need_i > 0 unwind exactly need_i, their source edge is saturated and lies on no cycle)
    struct RE { int u, v; int c; };
    vector<RE> res;
    for(int p = 0; p < m; p++){
        const Obligation& o = inst.obs[p];
        if(r.unsettled[p] < o.amt) res.push_back({(int)o.from, (int)o.to, +1});
        if(r.unsettled[p] > 0)     res.push_back({(int)o.to, (int)o.from, -1});
    }
    for(int i = 0; i < n; i++){
        if(need[i] >= 0) continue;
        __int128 a = -e[i];
        if(a < -need[i]) res.push_back({i, n, 0});
        if(a > 0)        res.push_back({n, i, 0});
    }
    vector<long long> d(n + 1, 0);                      // virtual source at distance 0 to everyone
    for(int round = 0; round <= n + 1; round++){
        bool ch = false;
        for(const RE& x : res) if(d[x.u] + x.c < d[x.v]){ d[x.v] = d[x.u] + x.c; ch = true; }
        if(!ch) return true;
    }
    why = "residual graph has a negative cycle: some payment could settle more";
    return false;
}

static void expect_lp(const char* label, const Instance& inst, Amount value, const vector<Amount>* y = nullptr){
    LpBound r = gridlock_lp_bound(inst);
    string why;
    bool ok = lp_ok(inst, r, why);
    CHECK(ok);
    if(!ok) cerr << "  [" << label << "] " << why << "\n";
    CHECK(r.value == value);
    if(r.value != value) cerr << "  [" << label << "] value " << r.value << ", expected " << value << "\n";
    if(y){
        CHECK(r.unsettled == *y);
        if(r.unsettled != *y) cerr << "  [" << label << "] unsettled differs from the unique optimum\n";
    }
}

// ------------------------------------------------------------------ A. hand cases
static void test_hand(){
    {   // gridlock: a pure cycle with no money settles in full
        Instance inst = make({{0,0},{0,0},{0,0}}, {{0,1,100},{1,2,100},{2,0,100}});
        vector<Amount> y = {0, 0, 0};
        expect_lp("cycle", inst, 300, &y);
        CHECK(gridlock_lp_bound(inst).total == 300);
    }
    {   // fractional: 30 of the 100 can settle (all-or-nothing optimum is 0)
        Instance inst = make({{30,0},{0,0}}, {{0,1,100}});
        vector<Amount> y = {70};
        expect_lp("partial", inst, 30, &y);
        CHECK(brute_gridlock(inst).best_value == 0);
    }
    {   // cycle + leak: the only bank with room is D, so all unwinding goes A -> D
        Instance inst = make({{0,0},{0,0},{0,0},{0,0}}, {{0,1,100},{1,2,100},{2,0,100},{0,3,50}});
        vector<Amount> y = {0, 0, 0, 50};
        expect_lp("cycle + leak", inst, 300, &y);
    }
    {   // the min-COST part: A must unwind 10. Direct A->D costs 10, the detour A->B->D costs 20. Only cheapest is optimal.
        Instance inst = make({{10,0},{0,0},{0,0}}, {{0,2,10},{0,1,10},{1,2,10}});
        vector<Amount> y = {10, 0, 0};
        expect_lp("cheapest unwind", inst, 20, &y);
    }
    {   // same, but the detour is listed first and is longer: still the direct edge
        Instance inst = make({{10,0},{0,0},{0,0},{0,0}}, {{0,1,10},{1,2,10},{2,3,10},{0,3,10}});
        vector<Amount> y = {0, 0, 0, 10};
        expect_lp("cheapest unwind, long detour", inst, 30, &y);
    }
    {   // credit counts as money; exactly enough is fine
        Instance inst = make({{60,40},{0,0}}, {{0,1,100}});
        vector<Amount> y = {0};
        expect_lp("credit", inst, 100, &y);
    }
    {   // parallel payments: 5 can settle in total, split is not unique
        Instance inst = make({{5,0},{0,0}}, {{0,1,10},{0,1,10}});
        expect_lp("parallel", inst, 5);
    }
    {   // a bank can pay with money it receives in the same round
        Instance inst = make({{50,0},{0,0},{0,0}}, {{0,1,50},{1,2,50}});
        vector<Amount> y = {0, 0};
        expect_lp("chain", inst, 100, &y);
    }
    {   // nothing to do
        Instance one = make({{0,0}}, {});
        expect_lp("n = 1", one, 0);
        LpBound r = gridlock_lp_bound(one);
        CHECK(r.total == 0 && r.unsettled.empty());
        Instance empty = make({{3,1},{0,0},{9,9}}, {});
        expect_lp("m = 0", empty, 0);
    }
    {   // a bank that is not in any payment, and banks with no need at all, must not break anything
        Instance inst = make({{0,0},{7,0},{0,0},{100,0}}, {{0,2,5},{2,0,5}});
        vector<Amount> y = {0, 0};
        expect_lp("isolated rich bank", inst, 10, &y);
    }
}

// ------------------------------------------------------------------ B. against the integer-enumeration oracle
static void test_vs_brute_lp(bool fast){
    int seeds = fast ? 1500 : 5000;
    int bad = 0, frac_gap = 0;
    for(int seed = 0; seed < seeds; seed++){
        Rng rng(13000 + seed);
        int n = (int)rng.uniform(1, 4);
        int m = (int)rng.uniform(0, 6);
        Instance inst = random_gridlock(rng, n, m, 3, 4);

        LpBound r = gridlock_lp_bound(inst);
        string why;
        bool ok = lp_ok(inst, r, why);
        Amount want = brute_lp(inst);
        if(!ok || r.value != want){
            if(bad++ < 5) cerr << "  seed " << seed << ": value " << r.value << ", LP optimum " << want << (ok ? "" : ", " + why) << "\n";
        }
        if(want > brute_gridlock(inst).best_value) frac_gap++;
    }
    CHECK(bad == 0);
    CHECK(frac_gap > 0);        // the generator does produce instances where the LP is strictly above the integer optimum
    cout << "lp_bound on " << seeds << " tiny instances: equal to the enumerated LP optimum on all; strictly above the "
         << "all-or-nothing optimum on " << frac_gap << "\n";
}

// ------------------------------------------------------------------ C. certificate + upper-bound property, medium sizes
static void test_certificate(bool fast){
    FifoBypassSolver fifo; DropViolatorsSolver dv; MaxflowRelaxSolver rx;
    vector<GridlockSolver*> heur = {&fifo, &dv, &rx};
    const Amount As[3] = {20, 1000000, 1000000000000LL};

    int seeds = fast ? 600 : 2000;
    int bad = 0, beats_dinic = 0;
    double gap_sum = 0; int gap_cnt = 0;
    for(int seed = 0; seed < seeds; seed++){
        Rng rng(14000 + seed);
        int n = (int)rng.uniform(2, 30);
        int m = (int)rng.uniform(0, 80);
        Amount A = As[rng.uniform(0, 2)];
        Instance inst = random_gridlock(rng, n, m, A, A / 4 * (Amount)rng.uniform(0, 4));

        LpBound r = gridlock_lp_bound(inst);
        string why;
        bool ok = lp_ok(inst, r, why);
        if(!ok && bad++ < 5) cerr << "  seed " << seed << ": " << why << "\n";

        CHECK(r.value >= 0 && r.value <= r.total);
        LpBound again = gridlock_lp_bound(inst);
        CHECK(again.value == r.value && again.unsettled == r.unsettled);      // deterministic

        for(GridlockSolver* h : heur){
            GridlockCheck c = validate_gridlock(inst, h->solve(inst));
            CHECK(c.ok && c.settled_value <= r.value);                          // an upper bound on every solver
        }
        // Dinic's fractional solution is feasible for the same LP, so it can only be worse or equal
        rx.solve(inst);
        CHECK(rx.relaxed_value() <= r.value);
        if(rx.relaxed_value() < r.value) beats_dinic++;

        if(m <= 16){
            Amount opt = brute_gridlock(inst).best_value;
            CHECK(opt <= r.value);
            if(r.value > 0){ gap_sum += 100.0 * (double)(r.value - opt) / (double)r.value; gap_cnt++; }
        }
    }
    CHECK(bad == 0);
    CHECK(beats_dinic > 0);
    cout << "lp_bound on " << seeds << " medium instances: optimality certificate holds on all; above every solver; "
         << "strictly above Dinic's fractional value on " << beats_dinic << "; mean integrality gap (m <= 16) "
         << fixed << setprecision(2) << (gap_cnt ? gap_sum / gap_cnt : 0.0) << "%\n";
    cout.unsetf(ios::fixed);
}

// ------------------------------------------------------------------ D. liquidity
static void test_liquidity(bool fast){
    int seeds = fast ? 200 : 600;
    for(int seed = 0; seed < seeds; seed++){
        Rng rng(15000 + seed);
        int n = (int)rng.uniform(2, 12);
        Instance inst = random_gridlock(rng, n, (int)rng.uniform(0, 30), 1000, 1000);

        // enough money to pay everything without any incoming: everything settles, nothing is unwound
        Instance rich = inst;
        vector<Amount> out(n, 0);
        for(const Obligation& o : inst.obs) out[o.from] += o.amt;
        for(int i = 0; i < n; i++){
            rich.balance[i] = out[i];
            rich.credit_limit[i] = 0;
        }
        LpBound rr = gridlock_lp_bound(rich);
        CHECK(rr.value == rr.total);
        CHECK(sum_of(rr.unsettled) == 0);

        // more money never lowers the bound
        Amount prev = gridlock_lp_bound(inst).value;
        for(int k = 0; k < 4; k++){
            int b = (int)rng.uniform(0, n - 1);
            inst.balance[b] += rng.uniform(0, 500);
            Amount cur = gridlock_lp_bound(inst).value;
            CHECK(cur >= prev);
            prev = cur;
        }
    }
}

// ------------------------------------------------------------------ E. big numbers
static void test_big_numbers(){
    const Amount T = 1000000000000LL;                   // 1e12, the largest amount io accepts
    {   // a chain of 50 payments of 1e12 where nobody has money: the whole chain unwinds, cost 5e13
        Instance inst;
        inst.n = 51;
        inst.balance.assign(51, 0);
        inst.credit_limit.assign(51, 0);
        for(int i = 0; i < 50; i++){
            Obligation o;
            o.from = i;
            o.to = i + 1;
            o.amt = T;
            inst.obs.push_back(o);
        }
        expect_lp("chain of 1e12, broke", inst, 0, nullptr);
        CHECK(gridlock_lp_bound(inst).total == 50 * T);
        inst.balance[0] = T;
        vector<Amount> y(50, 0);
        expect_lp("chain of 1e12, funded", inst, 50 * T, &y);
    }
    {   // 1000 parallel payments of 1e12 (total 1e15), payer holds 4e14 + 1e14 credit: 5e14 settles
        Instance inst;
        inst.n = 2;
        inst.balance = {400 * T, 0};
        inst.credit_limit = {100 * T, 0};
        for(int i = 0; i < 1000; i++){
            Obligation o;
            o.from = 0;
            o.to = 1;
            o.amt = T;
            inst.obs.push_back(o);
        }
        expect_lp("1000 x 1e12", inst, 500 * T, nullptr);
    }
}

// ------------------------------------------------------------------ G. scale
static Instance sparse(int n, int m, uint64_t seed){
    Rng rng(seed);
    Instance inst;
    inst.n = n;
    for(int i = 0; i < n; i++){
        inst.balance.push_back(rng.uniform(0, 400000));
        inst.credit_limit.push_back(0);
    }
    for(int p = 0; p < m; p++){
        Obligation o;
        o.from = rng.uniform(0, n - 1);
        o.to = rng.uniform(0, n - 2);
        if(o.to >= o.from) o.to++;
        o.amt = rng.uniform(1, 1000000);
        inst.obs.push_back(o);
    }
    return inst;
}

static void test_scale(bool fast){
    using clk = chrono::steady_clock;
    auto ms = [](clk::time_point a, clk::time_point b){
        return chrono::duration_cast<chrono::milliseconds>(b - a).count();
    };

    {   // the Task 10 instance: many small components. Needs per-component solving: one global min-cost flow takes minutes.
        int n = fast ? 20000 : 100000, m = fast ? 8000 : 40000;
        Instance inst = sparse(n, m, 123);
        auto t0 = clk::now();
        LpBound r = gridlock_lp_bound(inst);
        auto t1 = clk::now();
        string why;
        CHECK(lp_ok(inst, r, why));
        CHECK(ms(t0, t1) < 3000);

        BranchBoundSolver bb; MaxflowRelaxSolver rx; DropViolatorsSolver dv;
        Amount opt = validate_gridlock(inst, bb.solve(inst)).settled_value;
        Amount drop = validate_gridlock(inst, dv.solve(inst)).settled_value;
        rx.solve(inst);
        CHECK(opt <= r.value);
        CHECK(rx.relaxed_value() <= r.value);
        cout << "lp_bound sparse n=" << n << ", m=" << m << ": bound " << r.value << ", exact optimum " << opt
             << " (bound is " << fixed << setprecision(3) << 100.0 * (double)(r.value - opt) / (double)r.value
             << "% above it), drop_violators " << drop << ", Dinic fractional " << rx.relaxed_value() << ", "
             << ms(t0, t1) << " ms\n";
        cout.unsetf(ios::fixed);
    }
    {   // one dense component: here the min-cost flow itself has to be reasonably fast
        int n = fast ? 300 : 1000, m = fast ? 3000 : 10000;
        Instance inst = sparse(n, m, 456);
        auto t0 = clk::now();
        LpBound r = gridlock_lp_bound(inst);
        auto t1 = clk::now();
        string why;
        bool ok = lp_ok(inst, r, why);
        CHECK(ok);
        if(!ok) cerr << "  dense: " << why << "\n";

        MaxflowRelaxSolver rx; DropViolatorsSolver dv;
        Amount relax = validate_gridlock(inst, rx.solve(inst)).settled_value;
        Amount drop = validate_gridlock(inst, dv.solve(inst)).settled_value;
        CHECK(relax <= r.value && drop <= r.value);
        cout << "lp_bound dense n=" << n << ", m=" << m << ": bound " << r.value << ", maxflow_relax " << relax
             << ", drop_violators " << drop << ", Dinic fractional " << rx.relaxed_value() << ", " << ms(t0, t1) << " ms\n";
    }
}

int main(int argc, char** argv){
    bool fast = (argc > 1 && string(argv[1]) == "--fast");

    test_hand();
    test_vs_brute_lp(fast);
    test_certificate(fast);
    test_liquidity(fast);
    test_big_numbers();
    test_scale(fast);

    if(failures){
        cerr << failures << " check(s) failed\n";
        return 1;
    }
    cout << "all tests passed\n";
    return 0;
}