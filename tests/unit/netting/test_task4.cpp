#include "netting/naive.hpp"
#include "netting/greedy.hpp"
#include "gen/generators.hpp"
#include "core/net_balance.hpp"
#include "core/validator.hpp"
#include "core/rng.hpp"

using namespace std;

static int failures = 0;

#define CHECK(cond) \
    do { if(!(cond)) { cerr << __FILE__ << ":" << __LINE__ << "  CHECK failed: " #cond "\n"; failures++; } } while(0)

using Edges = vector<array<long long,3>>;

static Instance make(int n, const Edges& edges){
    Instance inst;
    inst.n = n;
    for(const auto& e : edges){
        Obligation o;
        o.from = e[0];
        o.to = e[1];
        o.amt = e[2];
        inst.obs.push_back(o);
    }
    return inst;
}

static bool plan_is(const Plan& p, const Edges& e){
    if(p.transfers.size() != e.size()) return false;
    for(size_t i = 0; i < e.size(); i++){
        if(p.transfers[i].from != e[i][0] || p.transfers[i].to != e[i][1] || p.transfers[i].amt != e[i][2]) return false;
    }
    return true;
}

static bool same_plan(const Plan& a, const Plan& b){
    if(a.transfers.size() != b.transfers.size()) return false;
    for(size_t i = 0; i < a.transfers.size(); i++){
        if(a.transfers[i].from != b.transfers[i].from || a.transfers[i].to != b.transfers[i].to
           || a.transfers[i].amt != b.transfers[i].amt) return false;
    }
    return true;
}

static void expect_valid(const char* label, const Instance& inst, const Plan& p){
    ValidationResult r = validate_netting(inst, p);
    CHECK(r.ok);
    if(!r.ok) cerr << "  [" << label << "] invalid plan: " << r.error << "\n";
}

static size_t count_nonzero(const vector<Amount>& bal){
    size_t nz = 0;
    for(Amount x : bal) if(x != 0) nz++;
    return nz;
}

// ---------------------------------------------------------------- A. hand cases
static void test_hand_cases(){
    Instance inst = make(4, {{2,0,5},{2,1,1},{3,1,2}});   // bal = {+5, +3, -6, -2}

    GrossSolver gross; HubSolver hub; GreedySolver greedy;
    Plan pg = gross.solve(inst), ph = hub.solve(inst), pr = greedy.solve(inst);

    CHECK(plan_is(pg, {{2,0,5},{2,1,1},{3,1,2}}));
    CHECK(plan_is(ph, {{0,1,3},{2,0,6},{3,0,2}}));
    CHECK(plan_is(pr, {{2,0,5},{3,1,2},{2,1,1}}));

    expect_valid("gross", inst, pg);
    expect_valid("hub", inst, ph);
    expect_valid("greedy", inst, pr);

    CHECK(gross.name() == "gross");
    CHECK(hub.name() == "hub");
    CHECK(greedy.name() == "greedy");
}

// ties: equal amounts must be served smaller party id first
static void test_tie_break(){
    GreedySolver greedy;
    {
        Instance inst = make(3, {{2,0,2},{2,1,2}});          // creditors 0 and 1 tie (+2), debtor 2 (-4)
        CHECK(plan_is(greedy.solve(inst), {{2,0,2},{2,1,2}}));
    }
    {
        Instance inst = make(3, {{0,2,2},{1,2,2}});          // debtors 0 and 1 tie (-2), creditor 2 (+4)
        CHECK(plan_is(greedy.solve(inst), {{0,2,2},{1,2,2}}));
    }
}

// ---------------------------------------------------------------- B. properties on generated instances
static void check_solvers(const char* label, const Instance& inst){
    vector<Amount> bal = net_balance(inst);
    size_t nz = count_nonzero(bal);
    size_t tree = nz == 0 ? 0 : nz - 1;
    size_t lower = (nz + 1) / 2;

    GrossSolver gross; HubSolver hub; GreedySolver greedy;
    Plan pg = gross.solve(inst), ph = hub.solve(inst), pr = greedy.solve(inst);

    expect_valid(label, inst, pg);
    expect_valid(label, inst, ph);
    expect_valid(label, inst, pr);

    CHECK(pg.transfers.size() == inst.obs.size());   // generators never emit self-loops
    CHECK(ph.transfers.size() == tree);
    CHECK(pr.transfers.size() <= tree);

    CHECK(pg.transfers.size() >= lower);
    CHECK(ph.transfers.size() >= lower);
    CHECK(pr.transfers.size() >= lower);

    bool direction_ok = true;
    for(const Transfer& t : pr.transfers){
        if(!(bal[t.from] < 0 && bal[t.to] > 0)) direction_ok = false;
    }
    CHECK(direction_ok);
}

static void test_properties(){
    const Amount amts[2] = {3, 1000000000000LL};   // tiny amounts make ties and exact cancellations common

    for(int seed = 0; seed < 100; seed++){
        Rng p(7000 + seed);
        Amount max_amt = amts[p.uniform(0, 1)];

        {
            int n = (int)p.uniform(2, 30), m = (int)p.uniform(0, 100);
            check_solvers("random", gen_random(n, m, max_amt, seed));
        }
        {
            int n = (int)p.uniform(2, 30), m = (int)p.uniform(0, 100);
            check_solvers("hub", gen_hub(n, m, p.next_double() * 3.0, max_amt, seed));
        }
        {
            int n = (int)p.uniform(2, 30);
            int lo = (int)p.uniform(2, n), hi = (int)p.uniform(lo, n);
            check_solvers("cycles", gen_cycles(n, (int)p.uniform(0, 5), lo, hi, (int)p.uniform(0, 3), max_amt, seed));
        }
        {
            int groups = (int)p.uniform(1, 10);
            int n = (int)p.uniform(2 * groups, 40), m = (int)p.uniform(0, 100);
            check_solvers("clusters", gen_clusters(n, groups, m, max_amt, seed));
        }
    }
}

// ---------------------------------------------------------------- C. cycles without noise cancel completely
static void test_cycles_cancel(){
    for(int seed = 0; seed < 20; seed++){
        Instance inst = gen_cycles(20, 5, 2, 6, 0, 1000, seed);
        GrossSolver gross; HubSolver hub; GreedySolver greedy;
        CHECK(hub.solve(inst).transfers.empty());
        CHECK(greedy.solve(inst).transfers.empty());
        CHECK(gross.solve(inst).transfers.size() == inst.obs.size());
    }
}

// ---------------------------------------------------------------- D. greedy is not optimal
static void test_greedy_suboptimal(){
    // bal = {-3, +2, +2, +3, -4}
    // optimum is 3 transfers: groups {-3,+3} and {+2,+2,-4}. Greedy needs 4.
    Instance inst = make(5, {{0,1,2},{0,2,2},{0,3,3},{4,0,4}});
    vector<Amount> bal = net_balance(inst);
    CHECK(bal == vector<Amount>({-3, 2, 2, 3, -4}));

    GreedySolver greedy;
    Plan p = greedy.solve(inst);
    expect_valid("greedy suboptimal", inst, p);
    CHECK(p.transfers.size() == 4);
}

// ---------------------------------------------------------------- E. edge cases
static void test_edges(){
    GrossSolver gross; HubSolver hub; GreedySolver greedy;
    {
        Instance inst = make(1, {});
        CHECK(gross.solve(inst).transfers.empty());
        CHECK(hub.solve(inst).transfers.empty());
        CHECK(greedy.solve(inst).transfers.empty());
    }
    {
        Instance inst = make(3, {{0,1,100},{1,2,100},{2,0,100}});
        CHECK(gross.solve(inst).transfers.size() == 3);
        CHECK(hub.solve(inst).transfers.empty());
        CHECK(greedy.solve(inst).transfers.empty());
    }
    {   // hand-written self-loops: gross skips them
        Instance inst = make(2, {{0,0,5},{0,1,7},{1,1,9}});
        Plan p = gross.solve(inst);
        CHECK(plan_is(p, {{0,1,7}}));
        expect_valid("gross self-loops", inst, p);
    }
}

// ---------------------------------------------------------------- F. determinism
static void test_determinism(){
    Instance inst = gen_random(50, 300, 5, 3);
    GreedySolver greedy;
    CHECK(same_plan(greedy.solve(inst), greedy.solve(inst)));
}

// ---------------------------------------------------------------- G. scale
static void test_scale(){
    using clk = chrono::steady_clock;
    auto ms = [](clk::time_point a, clk::time_point b){
        return chrono::duration_cast<chrono::milliseconds>(b - a).count();
    };

    Instance inst = gen_random(100000, 1000000, 1000000000000LL, 1);
    size_t nz = count_nonzero(net_balance(inst));

    GrossSolver gross; HubSolver hub; GreedySolver greedy;
    NettingSolver* solvers[3] = {&gross, &hub, &greedy};

    size_t counts[3];
    for(int i = 0; i < 3; i++){
        auto t0 = clk::now();
        Plan p = solvers[i]->solve(inst);
        auto t1 = clk::now();
        expect_valid(solvers[i]->name().c_str(), inst, p);
        counts[i] = p.transfers.size();
        cout << "scale " << solvers[i]->name() << ": " << counts[i] << " transfers, " << ms(t0, t1) << " ms\n";
    }
    CHECK(counts[0] == 1000000);
    CHECK(counts[1] == nz - 1);
    CHECK(counts[2] <= nz - 1);
}

int main(int argc, char** argv){
    bool run_scale = !(argc > 1 && string(argv[1]) == "--fast");

    test_hand_cases();
    test_tie_break();
    test_properties();
    test_cycles_cancel();
    test_greedy_suboptimal();
    test_edges();
    test_determinism();
    if(run_scale) test_scale();

    if(failures){
        cerr << failures << " check(s) failed\n";
        return 1;
    }
    cout << "all tests passed\n";
    return 0;
}