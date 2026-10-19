#include "netting/match.hpp"
#include "netting/subset_dp.hpp"
#include "netting/greedy.hpp"
#include "netting/naive.hpp"
#include "gen/generators.hpp"
#include "core/net_balance.hpp"
#include "core/validator.hpp"
#include "core/rng.hpp"

using namespace std;

// Contract used by this file (matches match.hpp):
//   vector<vector<int>> match_groups(const vector<Amount>& b)
//   b = nonzero balances summing to 0, ANY size (invalid_argument on a zero entry or a nonzero sum).
//   Returns zero-sum groups, each a list of POSITIONS in b, that partition all of b.
// Algorithm that is pinned down by these tests:
//   1. cancel every +v with a -v (as many pairs as possible)
//   2. if at most 3000 elements are left: greedy zero-sum triples
//   3. whatever is left is ONE group
// The order of groups / members is not tested.

using Groups = vector<vector<int>>;

static int failures = 0;

#define CHECK(cond) \
    do { if(!(cond)) { cerr << __FILE__ << ":" << __LINE__ << "  CHECK failed: " #cond "\n"; failures++; } } while(0)

// ------------------------------------------------------------------ helpers
static Instance instance_from_balances(const vector<Amount>& bal){
    Instance inst;
    inst.n = (int)bal.size();
    for(size_t i = 1; i < bal.size(); i++){
        Obligation o;
        if(bal[i] > 0){
            o.from = 0; o.to = (PartyId)i; o.amt = bal[i];
            inst.obs.push_back(o);
        }else if(bal[i] < 0){
            o.from = (PartyId)i; o.to = 0; o.amt = -bal[i];
            inst.obs.push_back(o);
        }
    }
    return inst;
}

static vector<Amount> nonzero_balances(const Instance& inst){
    vector<Amount> out;
    for(Amount x : net_balance(inst)) if(x != 0) out.push_back(x);
    return out;
}

static vector<Amount> random_balances(Rng& rng, int k, int R){
    for(int attempt = 0; attempt < 100; attempt++){
        vector<Amount> b;
        Amount sum = 0;
        for(int i = 0; i < k - 1; i++){
            Amount v = 0;
            while(v == 0) v = rng.uniform(-R, R);
            b.push_back(v);
            sum += v;
        }
        if(sum != 0){
            b.push_back(-sum);
            return b;
        }
    }
    return {};
}

static void shuffle_vec(Rng& rng, vector<Amount>& v){
    for(size_t i = v.size(); i > 1; i--){
        size_t j = (size_t)rng.uniform(0, (int64_t)i - 1);
        swap(v[i - 1], v[j]);
    }
}

// every position exactly once, every group >= 2 members and zero-sum; "" when fine
static string partition_problem(const vector<Amount>& b, const Groups& groups){
    vector<int> seen(b.size(), 0);
    for(size_t g = 0; g < groups.size(); g++){
        if(groups[g].size() < 2) return "group " + to_string(g) + " has fewer than 2 members";
        __int128 sum = 0;
        for(int pos : groups[g]){
            if(pos < 0 || pos >= (int)b.size()) return "position out of range in group " + to_string(g);
            seen[pos]++;
            sum += b[pos];
        }
        if(sum != 0) return "group " + to_string(g) + " does not sum to zero";
    }
    for(size_t i = 0; i < b.size(); i++){
        if(seen[i] != 1) return "position " + to_string(i) + " used " + to_string(seen[i]) + " times";
    }
    return "";
}

static int checked_groups(const char* label, const vector<Amount>& b){
    Groups g = match_groups(b);
    string why = partition_problem(b, g);
    CHECK(why.empty());
    if(!why.empty()){
        cerr << "  [" << label << "] bad partition: " << why << "\n";
        if(b.size() <= 20){ cerr << "  balances:"; for(Amount x : b) cerr << " " << x; cerr << "\n"; }
    }
    return (int)g.size();
}

static size_t count_size(const Groups& g, size_t sz){
    size_t c = 0;
    for(const auto& x : g) if(x.size() == sz) c++;
    return c;
}

// number of +v / -v pairs that can be formed: sum over magnitudes of min(count(+v), count(-v))
static size_t max_pairs(const vector<Amount>& b){
    map<Amount, long long> pos, neg;
    for(Amount x : b){
        if(x > 0) pos[x]++; else neg[-x]++;
    }
    size_t pairs = 0;
    for(const auto& [v, c] : pos){
        auto it = neg.find(v);
        if(it != neg.end()) pairs += (size_t)min(c, it->second);
    }
    return pairs;
}

// b with the maximum number of +v/-v pairs removed
static vector<Amount> remove_pairs(const vector<Amount>& b){
    map<Amount, long long> pos, neg;
    for(Amount x : b){
        if(x > 0) pos[x]++; else neg[-x]++;
    }
    vector<Amount> out;
    for(auto& [v, c] : pos){
        long long cancel = min(c, neg.count(v) ? neg[v] : 0LL);
        for(long long i = 0; i < c - cancel; i++) out.push_back(v);
    }
    for(auto& [v, c] : neg){
        long long cancel = min(c, pos.count(v) ? pos[v] : 0LL);
        for(long long i = 0; i < c - cancel; i++) out.push_back(-v);
    }
    return out;
}

static void expect_valid(const char* label, const Instance& inst, const Plan& p){
    ValidationResult r = validate_netting(inst, p);
    CHECK(r.ok);
    if(!r.ok) cerr << "  [" << label << "] invalid plan: " << r.error << "\n";
}

template <class F>
static bool throws_invalid(F f){
    try{
        f();
    }catch(const invalid_argument&){
        return true;
    }catch(...){
        return false;
    }
    return false;
}

// ------------------------------------------------------------------ A. hand cases (group counts that any compliant implementation must give)
static void test_hand_cases(){
    struct Case { vector<Amount> b; int groups; const char* why; };
    vector<Case> cases = {
        { {},                           0, "empty" },
        { {5, -5},                      1, "one pair" },
        { {1, -1, 2, -2},               2, "two pairs" },
        { {3, 3, -3, -3},               2, "pairs with multiplicity" },
        { {1, 2, -3},                   1, "one triple" },
        { {1, 2, -3, 4, -4},            2, "pair + triple" },
        { {-3, 2, 2, 3, -4},            2, "pair {-3,3} then triple {2,2,-4}" },
        { {1, 2, 4, -7},                1, "no pair, no triple: one remainder group" },
        { {3, 5, -3, -5, 1, 1, -2},     3, "two pairs + triple {1,1,-2}" },
        { {1, 1, 1, -3},                1, "triple impossible, four elements: one group" },
        { {1, 2, -3, 10, 20, -30},      2, "two triples (a single remainder group would give 1)" },
        { {1, 2, -3, 5, 5, -10, 7, -7}, 3, "one pair + two triples" },
        { {1, 2, -3, 10, 20, -30, 100, 200, 400, -700}, 3, "two triples + a 4-element remainder" },
        { {1, 2, 4, -7, 10, 20, 40, -70, 100, 200, 400, -700}, 1, "three 4-groups hidden: heuristic finds none" },
    };
    for(const Case& c : cases){
        int got = checked_groups(c.why, c.b);
        CHECK(got == c.groups);
        if(got != c.groups) cerr << "  [" << c.why << "] expected " << c.groups << " groups, got " << got << "\n";
    }
}

// ------------------------------------------------------------------ B. the heuristic against exact, on random small vectors
static void test_against_exact(){
    const int Rs[3] = {3, 5, 10};
    int total = 0, worse = 0;
    long long lost = 0;

    for(int seed = 0; seed < 400; seed++){
        Rng rng(61000 + seed);
        int k = (int)rng.uniform(2, 12);
        vector<Amount> b = random_balances(rng, k, Rs[rng.uniform(0, 2)]);
        if(b.empty()) continue;

        Groups g = match_groups(b);
        string why = partition_problem(b, g);
        CHECK(why.empty());

        int exact = (int)dp_max_groups(b).size();
        CHECK((int)g.size() >= 1);
        CHECK((int)g.size() <= exact);                         // a heuristic can never beat the optimum

        // stage 1 takes exactly the maximum number of pairs
        CHECK(count_size(g, 2) == max_pairs(b));

        // pair cancellation never loses optimality: opt(b) == pairs + opt(b without the pairs)
        vector<Amount> reduced = remove_pairs(b);
        int opt_reduced = reduced.empty() ? 0 : (int)dp_max_groups(reduced).size();
        CHECK(exact == (int)max_pairs(b) + opt_reduced);

        total++;
        if((int)g.size() < exact){ worse++; lost += exact - (int)g.size(); }
    }
    cout << "match vs exact on " << total << " small vectors: heuristic strictly worse on " << worse
         << ", total groups lost " << lost << "\n";
}

// ------------------------------------------------------------------ C. planted structure at larger sizes
static void test_planted_large(){
    using clk = chrono::steady_clock;
    auto ms = [](clk::time_point a, clk::time_point b){
        return chrono::duration_cast<chrono::milliseconds>(b - a).count();
    };

    // C1: 50,000 pairs, shuffled: exactly 50,000 groups, no remainder
    {
        Rng rng(71);
        vector<Amount> b;
        for(int i = 0; i < 50000; i++){
            Amount v = rng.uniform(1, 1000000000000LL);
            b.push_back(v);
            b.push_back(-v);
        }
        shuffle_vec(rng, b);
        auto t0 = clk::now();
        Groups g = match_groups(b);
        auto t1 = clk::now();
        CHECK(partition_problem(b, g).empty());
        CHECK(g.size() == 50000);
        CHECK(count_size(g, 2) == 50000);
        cout << "match k=100000 (50000 pairs): " << g.size() << " groups, " << ms(t0, t1) << " ms\n";
    }

    // C2: 7,000 pairs + 1,000 planted triples (leftover 3,000 = the limit, so the triple stage runs)
    {
        Rng rng(72);
        vector<Amount> b;
        for(int i = 0; i < 7000; i++){
            Amount v = rng.uniform(1, 1000000000000LL);
            b.push_back(v);
            b.push_back(-v);
        }
        for(int i = 0; i < 1000; i++){
            Amount a = rng.uniform(1, 400000000000LL), c = rng.uniform(1, 400000000000LL);
            b.push_back(a);
            b.push_back(c);
            b.push_back(-(a + c));
        }
        shuffle_vec(rng, b);
        auto t0 = clk::now();
        Groups g = match_groups(b);
        auto t1 = clk::now();
        CHECK(partition_problem(b, g).empty());
        CHECK(g.size() >= 8000);                       // 7000 pairs + 1000 triples (accidental extras only help)
        cout << "match k=17000 (7000 pairs + 1000 triples): " << g.size() << " groups, " << ms(t0, t1) << " ms\n";
    }

    // C3: 100 pairs + 1,500 planted triples: 4,500 left > 3,000, so the triple stage is skipped
    {
        Rng rng(73);
        vector<Amount> b;
        for(int i = 0; i < 100; i++){
            Amount v = rng.uniform(1, 1000000000000LL);
            b.push_back(v);
            b.push_back(-v);
        }
        for(int i = 0; i < 1500; i++){
            Amount a = rng.uniform(1, 400000000000LL), c = rng.uniform(1, 400000000000LL);
            b.push_back(a);
            b.push_back(c);
            b.push_back(-(a + c));
        }
        shuffle_vec(rng, b);
        Groups g = match_groups(b);
        CHECK(partition_problem(b, g).empty());
        CHECK(g.size() == 101);                        // 100 pairs + ONE remainder group
    }

    // C4: 100,000 random values with no structure: pairs are the only thing found, near O(k log k)
    {
        Rng rng(74);
        vector<Amount> b = random_balances(rng, 100000, 1000000000LL);
        auto t0 = clk::now();
        Groups g = match_groups(b);
        auto t1 = clk::now();
        CHECK(partition_problem(b, g).empty());
        CHECK(g.size() >= 1);
        cout << "match k=100000 (random): " << g.size() << " groups, " << ms(t0, t1) << " ms\n";
    }
}

// ------------------------------------------------------------------ D. the solver on generated instances
static void test_solver_on_generated(){
    MatchSolver match; SubsetDpSolver exact; GreedySolver greedy; HubSolver hub;
    const Amount amts[3] = {2, 5, 1000000000000LL};

    int instances = 0, vs_exact_worse = 0, vs_greedy_better = 0, vs_greedy_worse = 0;

    for(int seed = 0; seed < 200; seed++){
        Rng p(8000 + seed);
        Amount max_amt = amts[p.uniform(0, 2)];

        Instance inst;
        switch(seed % 4){
            case 0: {
                int n = (int)p.uniform(2, 14), m = (int)p.uniform(0, 40);
                inst = gen_random(n, m, max_amt, seed);
                break;
            }
            case 1: {
                int n = (int)p.uniform(2, 14), m = (int)p.uniform(0, 40);
                inst = gen_hub(n, m, p.next_double() * 3.0, max_amt, seed);
                break;
            }
            case 2: {
                int n = (int)p.uniform(2, 14);
                int lo = (int)p.uniform(2, n), hi = (int)p.uniform(lo, n);
                inst = gen_cycles(n, (int)p.uniform(0, 4), lo, hi, (int)p.uniform(0, 4), max_amt, seed);
                break;
            }
            default: {
                int groups = (int)p.uniform(1, 5);
                int n = (int)p.uniform(2 * groups, 16), m = (int)p.uniform(0, 60);
                inst = gen_clusters(n, groups, m, max_amt, seed);
                break;
            }
        }

        size_t nz = nonzero_balances(inst).size();
        Plan pm = match.solve(inst);
        expect_valid("match", inst, pm);

        Groups g = match_groups(nonzero_balances(inst));
        CHECK(pm.transfers.size() == nz - g.size());       // transfers = nonzero parties - groups

        size_t te = exact.solve(inst).transfers.size();
        size_t tg = greedy.solve(inst).transfers.size();
        size_t th = hub.solve(inst).transfers.size();
        CHECK(pm.transfers.size() >= te);                  // never better than exact
        CHECK(pm.transfers.size() <= th);                  // never worse than the single-hub plan

        instances++;
        if(pm.transfers.size() > te) vs_exact_worse++;
        if(pm.transfers.size() < tg) vs_greedy_better++;
        if(pm.transfers.size() > tg) vs_greedy_worse++;
    }
    cout << "match solver on " << instances << " small instances: worse than exact on " << vs_exact_worse
         << ", better than greedy on " << vs_greedy_better << ", worse than greedy on " << vs_greedy_worse << "\n";
}

// ------------------------------------------------------------------ E. edge cases, input checks, determinism
static void test_edges(){
    MatchSolver match;
    {   // everything cancels
        Instance inst;
        inst.n = 3;
        const long long e[3][3] = {{0,1,100},{1,2,100},{2,0,100}};
        for(auto& x : e){ Obligation o; o.from = x[0]; o.to = x[1]; o.amt = x[2]; inst.obs.push_back(o); }
        CHECK(match.solve(inst).transfers.empty());
    }
    {   // n = 1
        Instance inst;
        inst.n = 1;
        CHECK(match.solve(inst).transfers.empty());
    }
    {   // zero-balance parties between nonzero ones must not confuse the position -> party mapping
        Instance inst = instance_from_balances({3, 0, -3, 0, 5, -5});
        Plan p = match.solve(inst);
        expect_valid("zeros between", inst, p);
        CHECK(p.transfers.size() == 2);
    }
    {   // more than 22 nonzero parties is fine here (the exact DP would refuse)
        vector<Amount> bal;
        for(int i = 1; i <= 40; i++){ bal.push_back(i); bal.push_back(-i); }
        Instance inst = instance_from_balances(bal);
        Plan p = match.solve(inst);
        expect_valid("40 pairs", inst, p);
        CHECK(p.transfers.size() == 40);
    }

    CHECK(throws_invalid([]{ match_groups({1, 0, -1}); }));      // zero entry
    CHECK(throws_invalid([]{ match_groups({1, 2, -2}); }));      // nonzero sum
    CHECK(!throws_invalid([]{ match_groups({}); }));

    {   // deterministic
        Rng rng(5);
        vector<Amount> b = random_balances(rng, 12, 6);
        CHECK(match_groups(b) == match_groups(b));
    }
}

// ------------------------------------------------------------------ F. where exact wins: three hidden 4-groups
static void test_gap_to_exact(){
    vector<Amount> bal = {1, 2, 4, -7, 10, 20, 40, -70, 100, 200, 400, -700};
    CHECK(dp_max_groups(bal).size() == 3);

    Instance inst = instance_from_balances(bal);
    expect_valid("gap/match", inst, MatchSolver().solve(inst));
    CHECK(MatchSolver().solve(inst).transfers.size() == 11);       // one group of 12
    CHECK(SubsetDpSolver().solve(inst).transfers.size() == 9);     // 12 - 3 groups
}

// ------------------------------------------------------------------ G. scale through the generators
static void test_scale(){
    using clk = chrono::steady_clock;
    auto ms = [](clk::time_point a, clk::time_point b){
        return chrono::duration_cast<chrono::milliseconds>(b - a).count();
    };

    MatchSolver match; HubSolver hub; GreedySolver greedy;
    struct Run { const char* name; Instance inst; };
    vector<Run> runs;
    runs.push_back({"random",   gen_random(100000, 1000000, 1000000000000LL, 1)});
    runs.push_back({"cycles",   gen_cycles(100000, 20000, 2, 6, 5000, 1000000000000LL, 1)});
    runs.push_back({"clusters", gen_clusters(100000, 20000, 400000, 1000000000000LL, 1)});

    for(Run& r : runs){
        size_t nz = nonzero_balances(r.inst).size();
        auto t0 = clk::now();
        Plan pm = match.solve(r.inst);
        auto t1 = clk::now();
        expect_valid(r.name, r.inst, pm);
        size_t th = hub.solve(r.inst).transfers.size();
        size_t tg = greedy.solve(r.inst).transfers.size();
        CHECK(pm.transfers.size() <= th);
        cout << "scale " << r.name << ": nz=" << nz << "  hub " << th << "  greedy " << tg
             << "  match " << pm.transfers.size() << "  (" << ms(t0, t1) << " ms)\n";
    }
}

int main(int argc, char** argv){
    bool fast = (argc > 1 && string(argv[1]) == "--fast");

    test_hand_cases();
    test_against_exact();
    test_planted_large();
    test_solver_on_generated();
    test_edges();
    test_gap_to_exact();
    if(!fast) test_scale();

    if(failures){
        cerr << failures << " check(s) failed\n";
        return 1;
    }
    cout << "all tests passed\n";
    return 0;
}