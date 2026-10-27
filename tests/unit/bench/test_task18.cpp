#include "bench/gridlock_bench.hpp"
#include "core/validator.hpp"
#include "gen/generators.hpp"
#include "gridlock/fifo_bypass.hpp"
#include "gridlock/drop_violators.hpp"
#include "gridlock/branch_bound.hpp"
#include "gridlock/maxflow_relax.hpp"
#include "brute/brute_gridlock.hpp"

using namespace std;

// Contract used by this file (includes/bench/gridlock_bench.hpp, src/bench/gridlock_bench.cpp):
//
//   Instance with_liquidity(const Instance& base, int permille)
//       copy of base (n and obs unchanged) with balance_i = floor(gross_out_i * permille / 1000) and credit_limit_i = 0.
//       gross_out_i * permille is computed in __int128 (it can exceed int64). Throws std::invalid_argument if permille < 0.
//       permille = 1000: every bank can pay all of its outgoing payments from its own balance.
//
//   RunResult run_gridlock(GridlockSolver& s, const Instance& inst)
//       times s.solve(inst) (wall clock, solve() only) and checks the result with validate_gridlock.
//       ok    = solve() returned without throwing     (on a throw: ok = valid = false, error = e.what())
//       valid = validate_gridlock accepted the set    (if not: valid = false, error = the validator's message)
//       value, count = settled value / count, only when ok && valid (0 otherwise)
//
//   string csv_header()
//   string csv_row(const BenchRow&)            no trailing newline; columns as in csv_header():
//       optimum      empty when unknown (-1)
//       settled_pct  100 * value / total (100 when total == 0), 3 decimals; empty unless ok && valid
//       gap_pct      100 * (optimum - value) / optimum (0 when optimum == 0), 3 decimals; empty unless ok && valid
//                    and optimum known
//       time_ms      3 decimals;   ok, valid as 0/1
//
//   map<pair<string,int>, GapStats> summarize(const vector<BenchRow>&)     key = (solver, permille)
//       runs, mean_time_ms         over all rows of the group
//       failed                     rows with !(ok && valid)
//       mean_settled_pct           over the ok && valid rows
//       with_opt, optimal          ok && valid rows with a known optimum / of those, value == optimum
//       mean_gap_pct, max_gap_pct  over the with_opt rows (0 when there are none)
//
//   void print_gap_table(ostream&, const map<...>&)    markdown table: 2 header lines + one line per group

static int failures = 0;

#define CHECK(cond) \
    do { if(!(cond)) { cerr << __FILE__ << ":" << __LINE__ << "  CHECK failed: " #cond "\n"; failures++; } } while(0)

// ------------------------------------------------------------------ helpers
static Instance make(int n, const vector<array<long long,3>>& pay){
    Instance inst;
    inst.n = n;
    for(const auto& p : pay){
        Obligation o;
        o.from = p[0];
        o.to = p[1];
        o.amt = p[2];
        inst.obs.push_back(o);
    }
    return inst;
}

static bool close(double a, double b){ return fabs(a - b) < 1e-9; }

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

static Amount total_of(const Instance& inst){
    Amount t = 0;
    for(const Obligation& o : inst.obs) t += o.amt;
    return t;
}

// returns every payment, sorted: infeasible whenever some bank cannot cover all of its payments
class AllSolver : public GridlockSolver {
public:
    string name() const override { return "all"; }
    vector<int> solve(const Instance& inst) override {
        vector<int> v(inst.obs.size());
        iota(v.begin(), v.end(), 0);
        return v;
    }
};

// returns a valid set, but in descending order: the validator must reject it
class UnsortedSolver : public GridlockSolver {
public:
    string name() const override { return "unsorted"; }
    vector<int> solve(const Instance&) override { return {1, 0}; }
};

// ------------------------------------------------------------------ A. with_liquidity
static void test_with_liquidity(){
    // bank 0 owes 500 + 499, bank 1 owes 7, bank 2 owes nothing
    Instance base = make(3, {{0,1,500},{0,2,499},{1,2,7}});

    Instance half = with_liquidity(base, 500);
    CHECK(half.n == 3);
    CHECK(half.obs.size() == base.obs.size());
    for(size_t p = 0; p < base.obs.size() && p < half.obs.size(); p++){
        CHECK(half.obs[p].from == base.obs[p].from && half.obs[p].to == base.obs[p].to && half.obs[p].amt == base.obs[p].amt);
    }
    CHECK(half.balance == vector<Amount>({499, 3, 0}));          // floor(999/2), floor(7/2), 0
    CHECK(half.credit_limit == vector<Amount>({0, 0, 0}));

    CHECK(with_liquidity(base, 0).balance == vector<Amount>({0, 0, 0}));
    CHECK(with_liquidity(base, 1000).balance == vector<Amount>({999, 7, 0}));
    CHECK(with_liquidity(base, 2000).balance == vector<Amount>({1998, 14, 0}));
    CHECK(with_liquidity(base, 1).balance == vector<Amount>({0, 0, 0}));       // floor(0.999), floor(0.007)

    // gross outflow 1e16: gross * permille = 1e19 does not fit in int64
    Instance big;
    big.n = 2;
    for(int i = 0; i < 10000; i++){
        Obligation o;
        o.from = 0;
        o.to = 1;
        o.amt = 1000000000000LL;
        big.obs.push_back(o);
    }
    CHECK(with_liquidity(big, 1000).balance == vector<Amount>({10000000000000000LL, 0}));
    CHECK(with_liquidity(big, 333).balance == vector<Amount>({3330000000000000LL, 0}));

    // a netting instance straight from a generator has no balances: that is fine
    Instance g = gen_random(10, 30, 1000, 7);
    Instance gl = with_liquidity(g, 250);
    CHECK((int)gl.balance.size() == 10 && (int)gl.credit_limit.size() == 10);
    CHECK(validate_gridlock(gl, {}).ok);

    CHECK(throws<invalid_argument>([&]{ with_liquidity(base, -1); }));
    CHECK(!throws<exception>([&]{ with_liquidity(make(1, {}), 500); }));
}

// at full liquidity every bank can pay everything on its own, so every solver must settle everything;
// and more liquidity can never lower the optimum
static void test_liquidity_semantics(bool fast){
    FifoBypassSolver fifo; DropViolatorsSolver dv; BranchBoundSolver bb; MaxflowRelaxSolver rx;
    vector<GridlockSolver*> solvers = {&fifo, &dv, &bb, &rx};

    int seeds = fast ? 20 : 60;
    for(int seed = 0; seed < seeds; seed++){
        vector<Instance> fams = {
            gen_random(6, 12, 50, seed),
            gen_hub(6, 12, 1.2, 50, seed),
            gen_cycles(6, 3, 2, 4, 2, 50, seed),
            gen_clusters(6, 2, 12, 50, seed),
        };
        for(const Instance& base : fams){
            Amount total = total_of(base);
            Instance full = with_liquidity(base, 1000);
            for(GridlockSolver* s : solvers){
                RunResult r = run_gridlock(*s, full);
                CHECK(r.ok && r.valid);
                CHECK(r.value == total);
                if(r.value != total) cerr << "  seed " << seed << ": " << s->name() << " settled " << r.value << " of " << total << " at full liquidity\n";
            }

            Amount prev = -1;
            for(int pm : {0, 100, 250, 500, 750, 1000}){
                Amount opt = brute_gridlock(with_liquidity(base, pm)).best_value;
                CHECK(opt >= prev);
                prev = opt;
            }
        }
    }
}

// ------------------------------------------------------------------ B. run_gridlock
static void test_run_gridlock(){
    Instance inst = with_liquidity(make(3, {{0,1,10},{1,2,10},{2,0,10},{0,2,5}}), 0);   // cycle settles, 0->2 cannot

    DropViolatorsSolver dv;
    RunResult r = run_gridlock(dv, inst);
    CHECK(r.ok && r.valid);
    CHECK(r.value == 30 && r.count == 3);
    CHECK(r.time_ms >= 0);
    CHECK(r.error.empty());

    // a solver that throws: B&B with a 1-node limit on an instance that needs branching
    Instance gap;
    gap.n = 2;
    gap.balance = {5, 6};
    gap.credit_limit = {0, 0};
    gap.obs = make(2, {{1,0,1},{1,0,6}}).obs;
    BranchBoundSolver tiny(1);
    RunResult t = run_gridlock(tiny, gap);
    CHECK(!t.ok && !t.valid);
    CHECK(t.value == 0 && t.count == 0);
    CHECK(!t.error.empty());
    CHECK(t.time_ms >= 0);

    // a solver whose answer is infeasible
    AllSolver all;
    RunResult a = run_gridlock(all, inst);
    CHECK(a.ok && !a.valid);
    CHECK(a.value == 0 && a.count == 0);
    CHECK(!a.error.empty());

    // a solver whose answer is not in ascending order
    UnsortedSolver uns;
    Instance two = with_liquidity(make(2, {{0,1,1},{0,1,1}}), 1000);
    RunResult u = run_gridlock(uns, two);
    CHECK(u.ok && !u.valid);
}

// ------------------------------------------------------------------ C. CSV
static void test_csv(){
    CHECK(csv_header() == "solver,family,n,m,seed,liquidity_permille,total,optimum,settled_value,settled_count,"
                          "settled_pct,gap_pct,time_ms,ok,valid");

    BenchRow r;
    r.solver = "maxflow_relax";
    r.family = "random";
    r.n = 8;
    r.m = 24;
    r.seed = 3;
    r.permille = 250;
    r.total = 1000;
    r.optimum = 300;
    r.run.ok = true;
    r.run.valid = true;
    r.run.value = 250;
    r.run.count = 4;
    r.run.time_ms = 1.5;
    CHECK(csv_row(r) == "maxflow_relax,random,8,24,3,250,1000,300,250,4,25.000,16.667,1.500,1,1");

    BenchRow unknown = r;
    unknown.optimum = -1;
    CHECK(csv_row(unknown) == "maxflow_relax,random,8,24,3,250,1000,,250,4,25.000,,1.500,1,1");

    BenchRow failed = r;
    failed.solver = "branch_bound";
    failed.optimum = -1;
    failed.run = RunResult();
    failed.run.time_ms = 2;
    failed.run.error = "Nodes limit exceeded";
    CHECK(csv_row(failed) == "branch_bound,random,8,24,3,250,1000,,0,0,,,2.000,0,0");

    BenchRow invalid = r;
    invalid.run.valid = false;
    invalid.run.value = 0;
    invalid.run.count = 0;
    CHECK(csv_row(invalid) == "maxflow_relax,random,8,24,3,250,1000,300,0,0,,,1.500,1,0");

    BenchRow empty = r;
    empty.total = 0;
    empty.optimum = 0;
    empty.run.value = 0;
    empty.run.count = 0;
    CHECK(csv_row(empty) == "maxflow_relax,random,8,24,3,250,0,0,0,0,100.000,0.000,1.500,1,1");

    // every row has as many columns as the header
    auto cols = [](const string& s){ return count(s.begin(), s.end(), ',') + 1; };
    CHECK(cols(csv_row(r)) == cols(csv_header()));
    CHECK(cols(csv_row(failed)) == cols(csv_header()));
}

// ------------------------------------------------------------------ D. summarize + table
static BenchRow row(const string& solver, int permille, Amount total, Amount optimum, Amount value, double ms, bool ok = true){
    BenchRow r;
    r.solver = solver;
    r.family = "x";
    r.permille = permille;
    r.total = total;
    r.optimum = optimum;
    r.run.ok = ok;
    r.run.valid = ok;
    r.run.value = ok ? value : 0;
    r.run.time_ms = ms;
    return r;
}

static void test_summarize(){
    vector<BenchRow> rows = {
        row("a", 100, 100, 50, 50, 1),          // gap 0,  settled 50
        row("a", 100, 200, 100, 80, 3),         // gap 20, settled 40
        row("a", 100, 300, 10, 0, 5, false),    // failed: counts only in runs, failed, time
        row("a", 500, 100, -1, 70, 2),          // optimum unknown
        row("b", 100, 0, 0, 0, 4),              // empty queue: settled 100, gap 0, optimal
    };
    auto s = summarize(rows);
    CHECK(s.size() == 3);

    const GapStats& a1 = s[{"a", 100}];
    CHECK(a1.runs == 3 && a1.failed == 1 && a1.with_opt == 2 && a1.optimal == 1);
    CHECK(close(a1.mean_gap_pct, 10.0) && close(a1.max_gap_pct, 20.0));
    CHECK(close(a1.mean_settled_pct, 45.0));
    CHECK(close(a1.mean_time_ms, 3.0));

    const GapStats& a5 = s[{"a", 500}];
    CHECK(a5.runs == 1 && a5.failed == 0 && a5.with_opt == 0 && a5.optimal == 0);
    CHECK(close(a5.mean_gap_pct, 0.0) && close(a5.max_gap_pct, 0.0));
    CHECK(close(a5.mean_settled_pct, 70.0) && close(a5.mean_time_ms, 2.0));

    const GapStats& b1 = s[{"b", 100}];
    CHECK(b1.runs == 1 && b1.with_opt == 1 && b1.optimal == 1);
    CHECK(close(b1.mean_settled_pct, 100.0) && close(b1.mean_gap_pct, 0.0));

    CHECK(summarize({}).empty());

    ostringstream out;
    print_gap_table(out, s);
    string text = out.str();
    CHECK(count(text.begin(), text.end(), '\n') == 5);       // 2 header lines + 3 groups
    CHECK(text.find("| a ") != string::npos && text.find("| b ") != string::npos);
}

// ------------------------------------------------------------------ E. a small end-to-end benchmark
static void test_end_to_end(bool fast){
    FifoBypassSolver fifo; DropViolatorsSolver dv; MaxflowRelaxSolver rx;
    vector<GridlockSolver*> heuristics = {&fifo, &dv, &rx};

    vector<BenchRow> rows;
    int seeds = fast ? 5 : 15;
    for(int seed = 0; seed < seeds; seed++){
        vector<pair<string, Instance>> fams = {
            {"random",   gen_random(8, 14, 100, 100 + seed)},
            {"hub",      gen_hub(8, 14, 1.5, 100, 100 + seed)},
            {"cycles",   gen_cycles(8, 3, 2, 5, 3, 100, 100 + seed)},
            {"clusters", gen_clusters(8, 2, 14, 100, 100 + seed)},
        };
        for(const auto& [family, base] : fams){
            for(int pm : {0, 200, 500}){
                Instance inst = with_liquidity(base, pm);
                BranchBoundSolver bb(5000000);
                RunResult exact = run_gridlock(bb, inst);
                CHECK(exact.ok && exact.valid);
                CHECK(exact.value == brute_gridlock(inst).best_value);
                Amount opt = exact.ok && exact.valid ? exact.value : -1;

                auto add = [&](const string& name, const RunResult& r){
                    BenchRow b;
                    b.solver = name;
                    b.family = family;
                    b.n = inst.n;
                    b.m = inst.obs.size();
                    b.seed = 100 + seed;
                    b.permille = pm;
                    b.total = total_of(inst);
                    b.optimum = opt;
                    b.run = r;
                    rows.push_back(b);
                };
                add("branch_bound", exact);
                for(GridlockSolver* h : heuristics){
                    RunResult r = run_gridlock(*h, inst);
                    CHECK(r.ok && r.valid);
                    CHECK(r.value <= opt);
                    add(h->name(), r);
                }
            }
        }
    }

    auto s = summarize(rows);
    CHECK(s.size() == 4 * 3);                                       // 4 solvers x 3 liquidity levels
    for(int pm : {0, 200, 500}){
        const GapStats& b = s[{"branch_bound", pm}];
        CHECK(b.failed == 0 && b.optimal == b.with_opt && close(b.max_gap_pct, 0.0));
    }
    for(const auto& [key, g] : s){
        CHECK(g.runs == seeds * 4);
        CHECK(g.failed == 0);
        CHECK(g.mean_gap_pct >= 0 && g.mean_gap_pct <= g.max_gap_pct + 1e-9);
    }

    print_gap_table(cout, s);
}

int main(int argc, char** argv){
    bool fast = (argc > 1 && string(argv[1]) == "--fast");

    test_with_liquidity();
    test_liquidity_semantics(fast);
    test_run_gridlock();
    test_csv();
    test_summarize();
    test_end_to_end(fast);

    if(failures){
        cerr << failures << " check(s) failed\n";
        return 1;
    }
    cout << "all tests passed\n";
    return 0;
}