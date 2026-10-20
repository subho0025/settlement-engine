#include "core/validator.hpp"          // GridlockCheck / validate_gridlock
#include "gridlock/drop_violators.hpp"
#include "gridlock/branch_bound.hpp"
#include "gridlock/maxflow_relax.hpp"
#include "brute/brute_gridlock.hpp"
#include "core/rng.hpp"

using namespace std;

// Contract used by this file (Dinic itself is tested in tests/unit/flow/test_task11_dinic.cpp):
//
//   class MaxflowRelaxSolver : public GridlockSolver   (includes/gridlock/maxflow_relax.hpp, src/gridlock/maxflow_relax.cpp)
//       name() == "maxflow_relax"
//       solve(inst) -> ascending indices of a FEASIBLE and MAXIMAL set (no payment left out is affordable for its payer).
//                      Precondition: inst is valid as read_gridlock_instance guarantees (balance/credit_limit of size n,
//                      endpoints in range, no self-loops, balance and credit_limit >= 0). solve() does not re-check it.
//       flow_value()    -> Dinic flow of the last solve; always equals sum over banks of max(0, need_i),
//                          need_i = (gross_out_i - gross_in_i) - (balance_i + credit_limit_i)
//       relaxed_value() -> sum over payments of (amt_p - y_p): value of the fractional solution
//       unsettled()     -> const vector<Amount>&, y_p for every payment (size m)
//       All three describe the LAST solve() call and are reset by every call.
//
// Which maximum flow Dinic finds is NOT pinned down, so apart from cases whose answer is the same for every maximum
// flow, the relaxation is checked through invariants that hold for any correct implementation.

static int failures = 0;

#define CHECK(cond) \
    do { if(!(cond)) { cerr << __FILE__ << ":" << __LINE__ << "  CHECK failed: " #cond "\n"; failures++; } } while(0)

// ------------------------------------------------------------------ helpers
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

static vector<__int128> slack_after(const Instance& inst, const vector<int>& sel){
    vector<__int128> s(inst.n);
    for(int i = 0; i < inst.n; i++) s[i] = (__int128)inst.balance[i] + inst.credit_limit[i];
    for(int p : sel){
        s[inst.obs[p].from] -= inst.obs[p].amt;
        s[inst.obs[p].to]   += inst.obs[p].amt;
    }
    return s;
}

// every payment left out is unaffordable for its payer
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

template <class E, class F>
static bool throws(F f){
    try{
        f();
    }catch(const E&){
        return true;
    }catch(...){
        return false;      // wrong exception type
    }
    return false;
}

// sum over banks of max(0, need_i): what the unwinding flow must carry
static Amount total_need(const Instance& inst){
    vector<__int128> d(inst.n, 0);
    for(const Obligation& o : inst.obs){
        d[o.from] += o.amt;
        d[o.to]   -= o.amt;
    }
    __int128 tot = 0;
    for(int i = 0; i < inst.n; i++){
        __int128 need = d[i] - ((__int128)inst.balance[i] + inst.credit_limit[i]);
        if(need > 0) tot += need;
    }
    return (Amount)tot;
}

// Checks everything the last solve() reported about its fractional solution. Returns false (and prints why) on the
// first violated invariant.
static bool relaxation_ok(const Instance& inst, const MaxflowRelaxSolver& s, string& why){
    const vector<Amount>& y = s.unsettled();
    if(y.size() != inst.obs.size()){ why = "unsettled() has the wrong size"; return false; }

    vector<__int128> pos(inst.n);
    for(int i = 0; i < inst.n; i++) pos[i] = (__int128)inst.balance[i] + inst.credit_limit[i];
    __int128 relaxed = 0, sum_y = 0;
    for(size_t p = 0; p < y.size(); p++){
        const Obligation& o = inst.obs[p];
        if(y[p] < 0 || y[p] > o.amt){ why = "y[" + to_string(p) + "] outside [0, amt]"; return false; }
        Amount x = o.amt - y[p];
        pos[o.from] -= x;
        pos[o.to]   += x;
        relaxed += x;
        sum_y += y[p];
    }
    for(int i = 0; i < inst.n; i++){
        if(pos[i] < 0){ why = "fractional solution leaves bank " + to_string(i) + " below its limit"; return false; }
    }
    if(relaxed != s.relaxed_value()){ why = "relaxed_value() != sum of (amt - y)"; return false; }
    if(s.flow_value() != total_need(inst)){ why = "flow_value() != sum of positive needs"; return false; }
    // every unit of unwinding crosses at least one payment (no bank has both an s-edge and a t-edge)
    if(sum_y < s.flow_value()){ why = "sum of y is smaller than the flow"; return false; }
    return true;
}

// ------------------------------------------------------------------ D. relaxation: hand cases (same answer for every maximum flow)
static void test_relax_hand(){
    MaxflowRelaxSolver r;
    CHECK(r.name() == "maxflow_relax");
    string why;

    {   // pure 3-cycle with no money: nothing to unwind, everything settles
        Instance inst = make({{0,0},{0,0},{0,0}}, {{0,1,100},{1,2,100},{2,0,100}});
        expect_set("cycle", inst, r.solve(inst), {0, 1, 2});
        CHECK(r.flow_value() == 0);
        CHECK(r.relaxed_value() == 300);
        CHECK(r.unsettled() == vector<Amount>({0, 0, 0}));
        CHECK(relaxation_ok(inst, r, why));
    }
    {   // one payment the payer can only cover 30 of: fractional 30, integer nothing
        Instance inst = make({{30,0},{0,0}}, {{0,1,100}});
        expect_set("unaffordable", inst, r.solve(inst), {});
        CHECK(r.flow_value() == 70);
        CHECK(r.relaxed_value() == 30);
        CHECK(r.unsettled() == vector<Amount>({70}));
        CHECK(relaxation_ok(inst, r, why));
    }
    {   // cycle plus a leak A -> D: the only bank that can absorb is D, so the whole unwind goes through A -> D
        Instance inst = make({{0,0},{0,0},{0,0},{0,0}}, {{0,1,100},{1,2,100},{2,0,100},{0,3,50}});
        expect_set("cycle + leak", inst, r.solve(inst), {0, 1, 2});
        CHECK(r.flow_value() == 50);
        CHECK(r.relaxed_value() == 300);
        CHECK(r.unsettled() == vector<Amount>({0, 0, 0, 50}));
        CHECK(relaxation_ok(inst, r, why));
    }
    {   // exactly enough money (credit counts)
        Instance inst = make({{60,40},{0,0}}, {{0,1,100}});
        expect_set("exact", inst, r.solve(inst), {0});
        CHECK(r.flow_value() == 0);
        CHECK(r.relaxed_value() == 100);
    }
    {   // repair: 30 cannot cover the 100, whatever the flow; the 20 must end up settled (directly or by the add-back pass)
        Instance inst = make({{30,0},{0,0},{0,0}}, {{0,1,100},{0,2,20}});
        expect_set("drop then add back", inst, r.solve(inst), {1});
        CHECK(r.flow_value() == 90);
        CHECK(relaxation_ok(inst, r, why));
    }
    {   // a chain: 0 pays 1 pays 2, only 0 has money -> everything settles, nothing to unwind
        Instance inst = make({{50,0},{0,0},{0,0}}, {{0,1,50},{1,2,50}});
        expect_set("chain", inst, r.solve(inst), {0, 1});
        CHECK(r.flow_value() == 0);
    }
    {   // empty queue
        Instance inst = make({{1,1},{2,2}}, {});
        expect_set("empty", inst, r.solve(inst), {});
        CHECK(r.flow_value() == 0);
        CHECK(r.relaxed_value() == 0);
        CHECK(r.unsettled().empty());
    }
    {   // stats are reset by every solve(): a big instance followed by an empty one
        Instance big = make({{30,0},{0,0}}, {{0,1,100}});
        Instance empty = make({{0,0}}, {});
        r.solve(big);
        r.solve(empty);
        CHECK(r.flow_value() == 0 && r.relaxed_value() == 0 && r.unsettled().empty());
    }
}

// ------------------------------------------------------------------ E. relaxation: smallest valid instance
static void test_relax_contract(){
    MaxflowRelaxSolver r;
    CHECK(!throws<exception>([&]{ Instance i = make({{0,0}}, {}); r.solve(i); }));                             // n = 1, m = 0
}

// ------------------------------------------------------------------ F. relaxation: against the exhaustive optimum
static void test_relax_vs_brute(bool fast){
    MaxflowRelaxSolver r; DropViolatorsSolver dv;
    const Amount As[3] = {5, 20, 1000000000000LL};

    int seeds = fast ? 600 : 3000;
    int total = 0, relax_opt = 0, dv_opt = 0, relax_better = 0, dv_better = 0;
    double relax_gap_sum = 0, dv_gap_sum = 0, relax_gap_max = 0, dv_gap_max = 0;

    for(int seed = 0; seed < seeds; seed++){
        Rng rng(12000 + seed);
        int n = (int)rng.uniform(2, 6), m = (int)rng.uniform(0, 12);
        Instance inst = random_gridlock(rng, n, m, As[rng.uniform(0, 2)]);

        vector<int> sel = r.solve(inst);
        GridlockCheck c = validate_gridlock(inst, sel);
        CHECK(c.ok);
        if(!c.ok) cerr << "  seed " << seed << ": " << c.error << "\n";

        string why;
        bool rel = relaxation_ok(inst, r, why);
        CHECK(rel);
        if(!rel) cerr << "  seed " << seed << ": " << why << "\n";

        CHECK(is_maximal(inst, sel));
        CHECK(r.solve(inst) == sel);                         // deterministic

        Amount opt = brute_gridlock(inst).best_value;
        CHECK(c.settled_value <= opt);
        Amount dv_value = validate_gridlock(inst, dv.solve(inst)).settled_value;

        total++;
        if(c.settled_value == opt) relax_opt++;
        if(dv_value == opt) dv_opt++;
        if(c.settled_value > dv_value) relax_better++;
        if(c.settled_value < dv_value) dv_better++;
        if(opt > 0){
            double g1 = 100.0 * (double)(opt - c.settled_value) / (double)opt;
            double g2 = 100.0 * (double)(opt - dv_value) / (double)opt;
            relax_gap_sum += g1; dv_gap_sum += g2;
            relax_gap_max = max(relax_gap_max, g1); dv_gap_max = max(dv_gap_max, g2);
        }
    }

    cout << fixed << setprecision(2);
    cout << "on " << total << " small instances (gap = % of the exhaustive optimum left unsettled):\n";
    cout << "  maxflow_relax : optimal on " << relax_opt << ", mean gap " << relax_gap_sum / total << "%, max gap " << relax_gap_max << "%\n";
    cout << "  drop_violators: optimal on " << dv_opt << ", mean gap " << dv_gap_sum / total << "%, max gap " << dv_gap_max << "%\n";
    cout << "  relax strictly better on " << relax_better << ", drop_violators strictly better on " << dv_better << "\n";
    cout.unsetf(ios::fixed);
}

// ------------------------------------------------------------------ G. relaxation: scale
static void test_relax_scale(bool fast){
    using clk = chrono::steady_clock;
    auto ms = [](clk::time_point a, clk::time_point b){
        return chrono::duration_cast<chrono::milliseconds>(b - a).count();
    };

    // same sparse instance as Task 10's scale test, so the numbers can be compared directly
    int n = fast ? 20000 : 100000;
    int m = fast ? 8000 : 40000;
    Rng rng(123);
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

    MaxflowRelaxSolver r; BranchBoundSolver bb; DropViolatorsSolver dv;
    auto t0 = clk::now();
    vector<int> sel = r.solve(inst);
    auto t1 = clk::now();

    GridlockCheck c = validate_gridlock(inst, sel);
    CHECK(c.ok);
    CHECK(is_maximal(inst, sel));
    string why;
    CHECK(relaxation_ok(inst, r, why));

    Amount opt = validate_gridlock(inst, bb.solve(inst)).settled_value;   // exact here: the components are small
    Amount dv_value = validate_gridlock(inst, dv.solve(inst)).settled_value;
    CHECK(c.settled_value <= opt);

    cout << "maxflow_relax on n=" << n << ", m=" << m << ": value " << c.settled_value << " (optimum " << opt
         << ", drop_violators " << dv_value << ", fractional " << r.relaxed_value() << "), flow " << r.flow_value()
         << ", " << ms(t0, t1) << " ms\n";
}

int main(int argc, char** argv){
    bool fast = (argc > 1 && string(argv[1]) == "--fast");

    test_relax_hand();
    test_relax_contract();
    test_relax_vs_brute(fast);
    test_relax_scale(fast);

    if(failures){
        cerr << failures << " check(s) failed\n";
        return 1;
    }
    cout << "all tests passed\n";
    return 0;
}