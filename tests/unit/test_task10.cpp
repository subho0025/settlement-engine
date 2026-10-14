#include "io.hpp"
#include "validator.hpp"           // GridlockCheck / validate_gridlock
#include "gridlock.hpp"            // GridlockSolver
#include "fifo_bypass.hpp"
#include "drop_violators.hpp"
#include "branch_bound.hpp"
#include "brute_gridlock.hpp"      // tests/brute/
#include "rng.hpp"

using namespace std;

// Contract used by this file:
//   class BranchBoundSolver : public GridlockSolver
//       explicit BranchBoundSolver(long long node_limit = -1)     -1 = unlimited
//       name() == "branch_bound"
//       solve(inst)  -> ascending indices of an OPTIMAL set (maximum settled value). If the optimum is not unique,
//                       which optimal set is returned is not tested (only its value).
//                       Throws std::runtime_error once more than node_limit search nodes are needed.
//       nodes_explored() -> search nodes used by the last solve() call, counting every call of the recursive search
//                       function (root included) across all components; 0 for an empty queue.

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

static vector<__int128> slack_after(const Instance& inst, const vector<int>& sel){
    vector<__int128> s(inst.n);
    for(int i = 0; i < inst.n; i++) s[i] = (__int128)inst.balance[i] + inst.credit_limit[i];
    for(int p : sel){
        s[inst.obs[p].from] -= inst.obs[p].amt;
        s[inst.obs[p].to]   += inst.obs[p].amt;
    }
    return s;
}

// an optimal set cannot be extended: every payment left out is unaffordable for its payer
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

template <class F>
static bool throws_runtime(F f){
    try{
        f();
    }catch(const runtime_error&){
        return true;
    }catch(...){
        return false;      // wrong exception type
    }
    return false;
}

// ------------------------------------------------------------------ A. hand cases with a unique optimum
static void test_hand_cases(){
    BranchBoundSolver bb;
    CHECK(bb.name() == "branch_bound");

    {   // the case drop_violators gets wrong: bank 1 owes 1 then 6 but has 6, so the single payment of 6 wins
        Instance inst = make({{5,0},{6,0}}, {{1,0,1},{1,0,6}});
        expect_set("gap", inst, bb.solve(inst), {1});
    }
    {   // bank 0 has 6 and owes 4, 3, 3: the two 3s (value 6) beat the 4
        Instance inst = make({{6,0},{0,0},{0,0},{0,0}}, {{0,1,4},{0,2,3},{0,3,3}});
        expect_set("two small beat one big", inst, bb.solve(inst), {1, 2});
    }
    {   // classic gridlocks are settled together
        Instance cyc = make({{0,0},{0,0}}, {{0,1,10},{1,0,10}});
        expect_set("cycle", cyc, bb.solve(cyc), {0, 1});
        Instance part = make({{0,0},{6,0}}, {{0,1,5},{1,0,10}});
        expect_set("partial", part, bb.solve(part), {0, 1});
    }
    {   // 3-cycle plus an extra payment that cannot be covered
        Instance inst = make({{0,0},{0,0},{0,0}}, {{0,1,10},{1,2,10},{2,0,10},{0,2,1}});
        expect_set("cycle + extra", inst, bb.solve(inst), {0, 1, 2});
    }
    {   // credit limit is spendable, exactly zero is fine
        Instance inst = make({{0,5},{0,0}}, {{0,1,5}});
        expect_set("credit", inst, bb.solve(inst), {0});
    }
    {   // nothing affordable
        Instance inst = make({{0,0},{0,0}}, {{0,1,3}});
        expect_set("nothing", inst, bb.solve(inst), {});
    }
    {   // empty queue
        Instance inst = make({{1,1},{2,2}}, {});
        expect_set("empty", inst, bb.solve(inst), {});
        CHECK(bb.nodes_explored() == 0);
    }
}

// ------------------------------------------------------------------ B. exact against exhaustive search
static void test_against_brute(bool fast){
    BranchBoundSolver bb; DropViolatorsSolver dv;
    const Amount As[3] = {5, 20, 1000000000000LL};

    int total = 0, dv_below = 0;

    int seeds = fast ? 120 : 300;
    for(int seed = 0; seed < seeds; seed++){
        Rng rng(8000 + seed);
        int n = (int)rng.uniform(2, 7), m = (int)rng.uniform(0, 14);
        Instance inst = random_gridlock(rng, n, m, As[rng.uniform(0, 2)]);

        vector<int> sel = bb.solve(inst);
        GridlockCheck r = validate_gridlock(inst, sel);
        CHECK(r.ok);
        if(!r.ok) cerr << "  seed " << seed << ": " << r.error << "\n";

        Amount opt = brute_gridlock(inst).best_value;
        CHECK(r.settled_value == opt);
        if(r.settled_value != opt) cerr << "  seed " << seed << ": got value " << r.settled_value << ", optimum " << opt << "\n";

        CHECK(is_maximal(inst, sel));                       // an optimum can always be extended no further
        CHECK(bb.solve(inst) == sel);                       // deterministic
        if(m > 0) CHECK(bb.nodes_explored() >= 1);

        Amount dv_value = validate_gridlock(inst, dv.solve(inst)).settled_value;
        CHECK(dv_value <= r.settled_value);
        total++;
        if(dv_value < r.settled_value) dv_below++;

        // an optimal set is feasible as a whole: solving the instance restricted to it returns everything
        Instance sub = inst;
        sub.obs.clear();
        for(int p : sel) sub.obs.push_back(inst.obs[p]);
        CHECK(validate_gridlock(sub, bb.solve(sub)).settled_value == r.settled_value);
    }
    cout << "branch_bound on " << total << " small instances: always equal to the optimum; drop_violators was strictly below it on "
         << dv_below << "\n";
    CHECK(dv_below > 0);

    // denser instances (few banks, many payments): 17-18 payments is still brute-forceable
    for(int seed = 0; seed < (fast ? 4 : 12); seed++){
        Rng rng(9000 + seed);
        int n = (int)rng.uniform(3, 4), m = (int)rng.uniform(17, 18);
        Instance inst = random_gridlock(rng, n, m, 20);
        vector<int> sel = bb.solve(inst);
        GridlockCheck r = validate_gridlock(inst, sel);
        CHECK(r.ok);
        CHECK(r.settled_value == brute_gridlock(inst).best_value);
    }
}

// ------------------------------------------------------------------ C. independent components must be solved separately
static void test_components(){
    using clk = chrono::steady_clock;
    auto ms = [](clk::time_point a, clk::time_point b){
        return chrono::duration_cast<chrono::milliseconds>(b - a).count();
    };

    Rng rng(5);
    Instance big;
    big.n = 0;
    Amount expected = 0;
    const int K = 40;
    for(int c = 0; c < K; c++){
        Instance part = random_gridlock(rng, (int)rng.uniform(3, 4), 10, 20);
        expected += brute_gridlock(part).best_value;       // exhaustive optimum of this component alone
        int off = big.n;
        big.n += part.n;
        for(int i = 0; i < part.n; i++){
            big.balance.push_back(part.balance[i]);
            big.credit_limit.push_back(part.credit_limit[i]);
        }
        for(Obligation o : part.obs){
            o.from += off;
            o.to += off;
            big.obs.push_back(o);
        }
    }
    // interleave the queue: the components' payments are mixed together
    for(size_t i = big.obs.size(); i > 1; i--){
        size_t j = (size_t)rng.uniform(0, (int64_t)i - 1);
        swap(big.obs[i - 1], big.obs[j]);
    }

    BranchBoundSolver bb;
    auto t0 = clk::now();
    vector<int> sel = bb.solve(big);
    auto t1 = clk::now();
    GridlockCheck r = validate_gridlock(big, sel);
    CHECK(r.ok);
    CHECK(r.settled_value == expected);
    CHECK(bb.nodes_explored() < 100000);       // 40 components of 10 payments: at most 40 * 2^11 even with no pruning at all;
                                               // searching the whole queue as one problem takes millions of nodes
    cout << "branch_bound on " << K << " interleaved components (" << big.obs.size() << " payments): value " << r.settled_value
         << " (= sum of exhaustive optima), " << bb.nodes_explored() << " nodes, " << ms(t0, t1) << " ms\n";
}

// ------------------------------------------------------------------ D. node limit
static void test_node_limit(){
    Instance gap = make({{5,0},{6,0}}, {{1,0,1},{1,0,6}});     // needs real branching: the incumbent is not optimal

    CHECK(throws_runtime([&]{ BranchBoundSolver s(1); s.solve(gap); }));
    CHECK(!throws_runtime([&]{ BranchBoundSolver s(1000000); s.solve(gap); }));
    CHECK(!throws_runtime([&]{ BranchBoundSolver s; s.solve(gap); }));

    // a solver that threw can be used again (an empty queue needs no search nodes, whatever the limit)
    BranchBoundSolver s(1);
    CHECK(throws_runtime([&]{ s.solve(gap); }));
    Instance empty = make({{1,1},{2,2}}, {});
    CHECK(!throws_runtime([&]{ s.solve(empty); }));
    CHECK(s.nodes_explored() == 0);
    CHECK(throws_runtime([&]{ s.solve(gap); }));        // and it still enforces its limit afterwards
}

// ------------------------------------------------------------------ E. sparse instance at scale (many small components)
static void test_scale(bool fast){
    using clk = chrono::steady_clock;
    auto ms = [](clk::time_point a, clk::time_point b){
        return chrono::duration_cast<chrono::milliseconds>(b - a).count();
    };

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

    BranchBoundSolver bb; DropViolatorsSolver dv; FifoBypassSolver fifo;
    auto t0 = clk::now();
    vector<int> sel = bb.solve(inst);
    auto t1 = clk::now();

    GridlockCheck r = validate_gridlock(inst, sel);
    CHECK(r.ok);
    CHECK(is_maximal(inst, sel));
    Amount dv_value = validate_gridlock(inst, dv.solve(inst)).settled_value;
    Amount fifo_value = validate_gridlock(inst, fifo.solve(inst)).settled_value;
    CHECK(r.settled_value >= dv_value);
    CHECK(r.settled_value >= fifo_value);

    double gain = dv_value > 0 ? 100.0 * (double)(r.settled_value - dv_value) / (double)dv_value : 0.0;
    cout << "branch_bound on n=" << n << ", m=" << m << ": value " << r.settled_value << " (drop_violators " << dv_value
         << ", +" << gain << "%; fifo_bypass " << fifo_value << "), " << bb.nodes_explored() << " nodes, " << ms(t0, t1) << " ms\n";
}

int main(int argc, char** argv){
    bool fast = (argc > 1 && string(argv[1]) == "--fast");

    test_hand_cases();
    test_against_brute(fast);
    test_components();
    test_node_limit();
    test_scale(fast);

    if(failures){
        cerr << failures << " check(s) failed\n";
        return 1;
    }
    cout << "all tests passed\n";
    return 0;
}