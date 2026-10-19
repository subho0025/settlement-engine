#include "netting/subset_dp.hpp"
#include "brute/brute_netting.hpp"
#include "netting/greedy.hpp"
#include "netting/naive.hpp"
#include "gen/generators.hpp"
#include "core/net_balance.hpp"
#include "core/validator.hpp"
#include "core/rng.hpp"

using namespace std;

using Groups = vector<vector<int>>;

static int failures = 0;

#define CHECK(cond) \
    do { if(!(cond)) { cerr << __FILE__ << ":" << __LINE__ << "  CHECK failed: " #cond "\n"; failures++; } } while(0)

// ------------------------------------------------------------------ helpers
// Builds an instance whose net balances are exactly `bal` (sum must be 0):
// party 0 trades with everyone else, so party 0's balance comes out as bal[0].
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

// k nonzero values in [-R, R] except the last one, summing to 0 (empty result if it failed)
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

// Is `groups` a real answer for b? Every position used exactly once, every group
// has at least 2 members and sums to zero. Returns "" when fine, else the reason.
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

// runs the DP, checks the partition is genuine, returns the number of groups
static int checked_groups(const char* label, const vector<Amount>& b){
    Groups g = dp_max_groups(b);
    string why = partition_problem(b, g);
    CHECK(why.empty());
    if(!why.empty()){
        cerr << "  [" << label << "] bad partition: " << why << "  balances:";
        for(Amount x : b) cerr << " " << x;
        cerr << "\n";
    }
    return (int)g.size();
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
        return false;   // wrong exception type
    }
    return false;
}

// ------------------------------------------------------------------ A. hand cases (brute AND dp, and the groups themselves)
static void test_hand_cases(){
    struct Case { vector<Amount> b; int groups; };
    vector<Case> cases = {
        { {},                     0 },
        { {5, -5},                1 },
        { {1, -1, 2, -2},         2 },
        { {-3, 2, 2, 3, -4},      2 },   // {-3,3} and {2,2,-4}
        { {1, 1, -2},             1 },
        { {1, 1, -1, -1},         2 },
        { {1, 2, 3, -6},          1 },
        { {1, -1, 1, -1, 1, -1},  3 },
        { {4, -1, -3, 2, -2},     2 },   // {4,-1,-3} and {2,-2}
        { {2, 3, -2, -3},         2 },   // index order 2,3,-2,-3 has only one zero prefix: needs real reordering
        { {3, 1, -3, 2, -1, -2},  3 },   // {3,-3} {1,-1} {2,-2}, interleaved on purpose
    };
    for(const Case& c : cases){
        CHECK(brute_max_groups(c.b) == c.groups);
        CHECK(checked_groups("hand", c.b) == c.groups);
    }

    // exact groups for two cases where the partition is unique
    {
        Groups g = dp_max_groups({5, -5});
        CHECK(g.size() == 1);
        if(g.size() == 1){
            vector<int> only = g[0];
            sort(only.begin(), only.end());
            CHECK(only == vector<int>({0, 1}));
        }
    }
    {
        Groups g = dp_max_groups({1, 2, 3, -6});
        CHECK(g.size() == 1);
        if(g.size() == 1) CHECK(g[0].size() == 4);
    }
}

// ------------------------------------------------------------------ B. stress: dp == brute on random balance vectors
static void test_stress_dp_vs_brute(bool fast){
    const int Rs[4] = {2, 3, 5, 10};   // small ranges => many zero-sum subsets
    int compared = 0;
    for(int seed = 0; seed < 300; seed++){
        Rng rng(31000 + seed);
        int k = (int)rng.uniform(2, 10);
        vector<Amount> b = random_balances(rng, k, Rs[rng.uniform(0, 3)]);
        if(b.empty()) continue;

        int g_dp = checked_groups("stress", b);
        int g_br = brute_max_groups(b);
        CHECK(g_dp == g_br);
        if(g_dp != g_br){
            cerr << "  mismatch on seed " << seed << ", balances:";
            for(Amount x : b) cerr << " " << x;
            cerr << "  dp=" << g_dp << " brute=" << g_br << "\n";
        }
        CHECK(g_dp >= 1);
        CHECK(2 * g_dp <= (int)b.size());   // every group has at least 2 members

        // order of the input must not matter for the count
        vector<Amount> shuffled = b;
        shuffle_vec(rng, shuffled);
        CHECK(checked_groups("stress/shuffled", shuffled) == g_dp);

        // deterministic: same input, same groups
        CHECK(dp_max_groups(b) == dp_max_groups(b));
        compared++;
    }
    CHECK(compared > 250);

    if(!fast){   // k = 11 is the largest size brute force handles comfortably
        for(int seed = 0; seed < 15; seed++){
            Rng rng(41000 + seed);
            vector<Amount> b = random_balances(rng, 11, 4);
            if(b.empty()) continue;
            CHECK(checked_groups("k=11", b) == brute_max_groups(b));
        }
    }
}

// ------------------------------------------------------------------ B2. planted groups: the answer can never be below the number planted
static void test_planted(){
    for(int seed = 0; seed < 150; seed++){
        Rng rng(52000 + seed);
        int planted = (int)rng.uniform(2, 4);

        vector<Amount> b;
        int made = 0;
        for(int g = 0; g < planted; g++){
            vector<Amount> part = random_balances(rng, (int)rng.uniform(2, 3), 6);
            if(part.empty()) continue;
            b.insert(b.end(), part.begin(), part.end());
            made++;
        }
        if(made == 0) continue;
        shuffle_vec(rng, b);

        int found = checked_groups("planted", b);
        CHECK(found >= made);                 // concatenating zero-sum blocks gives at least that many groups
        if(b.size() <= 10) CHECK(found == brute_max_groups(b));
    }
}

// ------------------------------------------------------------------ C. solver on generated instances
static void test_solver_on_generated(){
    SubsetDpSolver exact; GreedySolver greedy; HubSolver hub;
    const Amount amts[3] = {2, 5, 1000000000000LL};

    int instances = 0, greedy_worse = 0;
    long long gap_sum = 0;

    for(int seed = 0; seed < 200; seed++){
        Rng p(9000 + seed);
        Amount max_amt = amts[p.uniform(0, 2)];

        Instance inst;
        vector<int> gid;
        bool planted = false;

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
                inst = gen_clusters(n, groups, m, max_amt, seed, &gid);
                planted = true;
                break;
            }
        }

        vector<Amount> bal = net_balance(inst);
        vector<Amount> nzb = nonzero_balances(inst);
        size_t nz = nzb.size();

        Plan pe = exact.solve(inst);
        expect_valid("exact_dp", inst, pe);

        int g = checked_groups("generated", nzb);
        CHECK(pe.transfers.size() == nz - (size_t)g);
        if(nz <= 10) CHECK(g == brute_max_groups(nzb));

        // exact is never worse than the baselines, and never below ceil(nz/2)
        Plan pr = greedy.solve(inst), ph = hub.solve(inst);
        CHECK(pe.transfers.size() <= pr.transfers.size());
        CHECK(pe.transfers.size() <= ph.transfers.size());
        CHECK(pe.transfers.size() >= (nz + 1) / 2);

        if(planted){
            // each planted group with a nonzero party can be settled in (its nonzero count - 1) transfers
            set<int> touched;
            for(int i = 0; i < inst.n; i++) if(bal[i] != 0) touched.insert(gid[i]);
            CHECK(pe.transfers.size() <= nz - touched.size());
        }

        instances++;
        if(pr.transfers.size() > pe.transfers.size()){
            greedy_worse++;
            gap_sum += (long long)(pr.transfers.size() - pe.transfers.size());
        }
    }
    cout << "greedy vs exact on " << instances << " small instances: greedy strictly worse on "
         << greedy_worse << ", total extra transfers " << gap_sum << "\n";
}

// ------------------------------------------------------------------ D. edge cases and input checks
static void test_edges(){
    SubsetDpSolver exact;
    {   // everything cancels
        Instance inst;
        inst.n = 3;
        const long long e[3][3] = {{0,1,100},{1,2,100},{2,0,100}};
        for(auto& x : e){ Obligation o; o.from = x[0]; o.to = x[1]; o.amt = x[2]; inst.obs.push_back(o); }
        CHECK(exact.solve(inst).transfers.empty());
    }
    {   // single pair
        Instance inst = instance_from_balances({7, -7});
        Plan p = exact.solve(inst);
        expect_valid("pair", inst, p);
        CHECK(p.transfers.size() == 1);
    }
    {   // n = 1
        Instance inst;
        inst.n = 1;
        CHECK(exact.solve(inst).transfers.empty());
    }
    {   // zero-balance parties in the middle must be skipped, not confuse the position -> party mapping
        Instance inst = instance_from_balances({3, 0, -3, 0, 5, -5});
        Plan p = exact.solve(inst);
        expect_valid("zeros between", inst, p);
        CHECK(p.transfers.size() == 2);
    }

    // dp_max_groups preconditions (invalid_argument, exactly that type)
    CHECK(throws_invalid([]{ dp_max_groups({1, 0, -1}); }));                 // zero entry
    CHECK(throws_invalid([]{ dp_max_groups({1, 2, -2}); }));                 // does not sum to 0
    CHECK(throws_invalid([]{                                                  // 23 entries
        vector<Amount> b(22, 1);
        b.push_back(-22);
        dp_max_groups(b);
    }));
    CHECK(!throws_invalid([]{                                                 // 22 entries are allowed
        vector<Amount> b(21, 1);
        b.push_back(-21);
        dp_max_groups(b);
    }));

    // solver rejects instances with more than 22 nonzero parties
    CHECK(throws_invalid([&]{
        Instance inst;
        inst.n = 24;
        for(int i = 0; i < 23; i++){
            Obligation o; o.from = i; o.to = 23; o.amt = i + 1;
            inst.obs.push_back(o);
        }
        exact.solve(inst);       // 24 nonzero parties
    }));
}

// ------------------------------------------------------------------ E. the case greedy gets wrong
static void test_greedy_gap(){
    Instance inst;
    inst.n = 5;
    const long long e[4][3] = {{0,1,2},{0,2,2},{0,3,3},{4,0,4}};   // bal = {-3,+2,+2,+3,-4}
    for(auto& x : e){ Obligation o; o.from = x[0]; o.to = x[1]; o.amt = x[2]; inst.obs.push_back(o); }

    SubsetDpSolver exact; GreedySolver greedy;
    Plan pe = exact.solve(inst);
    expect_valid("gap/exact", inst, pe);
    CHECK(pe.transfers.size() == 3);
    CHECK(greedy.solve(inst).transfers.size() == 4);
}

// ------------------------------------------------------------------ F. largest size
static void test_large(bool fast){
    using clk = chrono::steady_clock;
    auto ms = [](clk::time_point a, clk::time_point b){
        return chrono::duration_cast<chrono::milliseconds>(b - a).count();
    };

    const int k = fast ? 16 : 22;
    SubsetDpSolver exact;

    {   // k/2 disjoint pairs: groups can't exceed k/2, so the answer is exactly k/2 transfers
        vector<Amount> bal;
        for(int i = 0; i < k / 2; i++){
            bal.push_back(1000 * (i + 1));
            bal.push_back(-1000 * (i + 1));
        }
        Instance inst = instance_from_balances(bal);
        auto t0 = clk::now();
        Plan p = exact.solve(inst);
        auto t1 = clk::now();
        expect_valid("large/pairs", inst, p);
        CHECK(p.transfers.size() == (size_t)(k / 2));
        cout << "exact_dp k=" << k << " (pairs):  " << p.transfers.size() << " transfers, " << ms(t0, t1) << " ms\n";
    }
    {   // 1,2,4,...,2^(k-2) and -(2^(k-1)-1): no proper subset sums to zero => one group, k-1 transfers
        vector<Amount> bal;
        Amount total = 0;
        for(int i = 0; i < k - 1; i++){
            bal.push_back((Amount)1 << i);
            total += (Amount)1 << i;
        }
        bal.push_back(-total);
        Instance inst = instance_from_balances(bal);
        auto t0 = clk::now();
        Plan p = exact.solve(inst);
        auto t1 = clk::now();
        expect_valid("large/powers", inst, p);
        CHECK(p.transfers.size() == (size_t)(k - 1));
        cout << "exact_dp k=" << k << " (powers): " << p.transfers.size() << " transfers, " << ms(t0, t1) << " ms\n";
    }
    {   // the groups returned at full size are a genuine partition too
        vector<Amount> bal;
        for(int i = 0; i < k / 2; i++){
            bal.push_back(7 * (i + 1));
            bal.push_back(-7 * (i + 1));
        }
        CHECK(checked_groups("large/groups", bal) == k / 2);
    }
}

int main(int argc, char** argv){
    bool fast = (argc > 1 && string(argv[1]) == "--fast");

    test_hand_cases();
    test_stress_dp_vs_brute(fast);
    test_planted();
    test_solver_on_generated();
    test_edges();
    test_greedy_gap();
    test_large(fast);

    if(failures){
        cerr << failures << " check(s) failed\n";
        return 1;
    }
    cout << "all tests passed\n";
    return 0;
}