#include "netting/mitm.hpp"
#include "netting/match.hpp"
#include "netting/subset_dp.hpp"
#include "netting/greedy.hpp"
#include "gen/generators.hpp"
#include "core/net_balance.hpp"
#include "core/validator.hpp"
#include "core/rng.hpp"

using namespace std;

// Contract used by this file (matches mitm.hpp):
//   vector<int> mitm_zero_subset(const vector<Amount>& v)
//       v.size() <= 40, no zero entries (invalid_argument otherwise).
//       Returns ascending positions of a NONEMPTY subset with sum 0 and the FEWEST elements; {} if none exists.
//   vector<vector<int>> refine_groups(const vector<Amount>& b, vector<vector<int>> groups)
//       groups = zero-sum partition of positions in b. Groups of 4..40 elements are split (smallest zero-sum
//       subset first) until no proper zero-sum subset is left; larger groups are untouched.
// Which of several equally small subsets is returned, and the order of groups, are not tested.

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

static vector<Amount> random_nonzero(Rng& rng, int k, int R){      // no sum constraint
    vector<Amount> v;
    for(int i = 0; i < k; i++){
        Amount x = 0;
        while(x == 0) x = rng.uniform(-R, R);
        v.push_back(x);
    }
    return v;
}

static void shuffle_vec(Rng& rng, vector<Amount>& v){
    for(size_t i = v.size(); i > 1; i--){
        size_t j = (size_t)rng.uniform(0, (int64_t)i - 1);
        swap(v[i - 1], v[j]);
    }
}

// size of the smallest nonempty zero-sum subset by plain enumeration; 0 if none (v.size() <= ~24)
static int brute_min_zero_subset(const vector<Amount>& v){
    int k = (int)v.size();
    uint32_t total = 1u << k;
    vector<Amount> sum(total, 0);
    int best = 0;
    for(uint32_t mask = 1; mask < total; mask++){
        sum[mask] = sum[mask & (mask - 1)] + v[__builtin_ctz(mask)];
        if(sum[mask] == 0){
            int sz = __builtin_popcount(mask);
            if(best == 0 || sz < best) best = sz;
        }
    }
    return best;
}

// returns "" if `sub` is a legal answer shape: ascending, in range, distinct, nonempty, sums to zero
static string subset_problem(const vector<Amount>& v, const vector<int>& sub){
    if(sub.empty()) return "empty subset";
    __int128 sum = 0;
    for(size_t i = 0; i < sub.size(); i++){
        if(sub[i] < 0 || sub[i] >= (int)v.size()) return "position out of range";
        if(i > 0 && sub[i] <= sub[i - 1]) return "positions not strictly ascending";
        sum += v[sub[i]];
    }
    if(sum != 0) return "does not sum to zero";
    return "";
}

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

static void expect_partition(const char* label, const vector<Amount>& b, const Groups& g){
    string why = partition_problem(b, g);
    CHECK(why.empty());
    if(!why.empty()) cerr << "  [" << label << "] bad partition: " << why << "\n";
}

// no proper nonempty zero-sum subset: mitm must hand back the whole group
static bool irreducible(const vector<Amount>& b, const vector<int>& group){
    vector<Amount> vals;
    for(int pos : group) vals.push_back(b[pos]);
    vector<int> sub = mitm_zero_subset(vals);
    return sub.size() == group.size();
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

// ------------------------------------------------------------------ A. mitm_zero_subset: hand cases
static void test_mitm_hand_cases(){
    struct Case { vector<Amount> v; size_t size; vector<int> exact; const char* why; };
    // exact = the only legal answer when it is unique (empty vector = not checked)
    vector<Case> cases = {
        { {},                     0, {},              "empty" },
        { {5},                    0, {},              "single nonzero element" },
        { {5, -5},                2, {0, 1},          "pair" },
        { {1, 2, 4, -7},          4, {0, 1, 2, 3},    "only the whole set cancels" },
        { {1, 2, -3, 10},         3, {0, 1, 2},       "triple among four" },
        { {3, 4, -7, 1, -1},      2, {3, 4},          "pair beats triple" },
        { {1, 1, 1},              0, {},              "no negatives: no zero subset" },
        { {2, 2, 2, -3},          0, {},              "no zero subset" },
        { {-5, 2, 3},             3, {0, 1, 2},       "negative first" },
        { {7, 1, 2, 4, -7},       2, {0, 4},          "pair at the ends" },
    };
    for(const Case& c : cases){
        vector<int> got = mitm_zero_subset(c.v);
        CHECK(got.size() == c.size);
        if(!c.exact.empty()) CHECK(got == c.exact);
        if(!got.empty()) CHECK(subset_problem(c.v, got).empty());
        if(got.size() != c.size) cerr << "  [" << c.why << "] expected size " << c.size << ", got " << got.size() << "\n";
    }
}

// ------------------------------------------------------------------ B. mitm_zero_subset against plain enumeration
static void test_mitm_vs_brute(bool fast){
    const int Rs[4] = {3, 5, 10, 40};
    int found = 0, none = 0;

    for(int seed = 0; seed < 300; seed++){
        Rng rng(81000 + seed);
        int k = (int)rng.uniform(1, 16);
        vector<Amount> v = random_nonzero(rng, k, Rs[rng.uniform(0, 3)]);

        vector<int> got = mitm_zero_subset(v);
        int want = brute_min_zero_subset(v);

        CHECK((int)got.size() == want);
        if((int)got.size() != want){
            cerr << "  seed " << seed << ": expected size " << want << ", got " << got.size() << ", values:";
            for(Amount x : v) cerr << " " << x;
            cerr << "\n";
        }
        if(!got.empty()){
            string why = subset_problem(v, got);
            CHECK(why.empty());
            found++;
        }else none++;
    }
    CHECK(found > 100 && none > 20);     // the random set exercises both outcomes

    // odd and even sizes 17..20 (halves of different length)
    if(!fast){
        for(int seed = 0; seed < 8; seed++){
            Rng rng(82000 + seed);
            int k = 17 + seed % 4;
            vector<Amount> v = random_nonzero(rng, k, 200);
            vector<int> got = mitm_zero_subset(v);
            CHECK((int)got.size() == brute_min_zero_subset(v));
            if(!got.empty()) CHECK(subset_problem(v, got).empty());
        }
    }

    // the input order must not change the SIZE of the answer
    for(int seed = 0; seed < 60; seed++){
        Rng rng(83000 + seed);
        vector<Amount> v = random_nonzero(rng, (int)rng.uniform(2, 14), 8);
        size_t a = mitm_zero_subset(v).size();
        shuffle_vec(rng, v);
        CHECK(mitm_zero_subset(v).size() == a);
    }
}

// ------------------------------------------------------------------ C. mitm at the size limit
static void test_mitm_large(bool fast){
    using clk = chrono::steady_clock;
    auto ms = [](clk::time_point a, clk::time_point b){
        return chrono::duration_cast<chrono::milliseconds>(b - a).count();
    };
    const int k = fast ? 28 : 36;   // size-40 behaviour is covered separately (limit test, refine at 40)

    {   // all positive: nothing cancels
        vector<Amount> v;
        for(int i = 0; i < k; i++) v.push_back((Amount)1 << i);
        auto t0 = clk::now();
        vector<int> got = mitm_zero_subset(v);
        auto t1 = clk::now();
        CHECK(got.empty());
        cout << "mitm k=" << k << " (no zero subset): " << ms(t0, t1) << " ms\n";
    }
    {   // 1, 2, 4, ... and minus their total: the only zero-sum subset is everything
        vector<Amount> v;
        Amount total = 0;
        for(int i = 0; i < k - 1; i++){ v.push_back((Amount)1 << i); total += (Amount)1 << i; }
        v.push_back(-total);
        auto t0 = clk::now();
        vector<int> got = mitm_zero_subset(v);
        auto t1 = clk::now();
        CHECK((int)got.size() == k);
        if((int)got.size() == k) for(int i = 0; i < k; i++) CHECK(got[i] == i);
        cout << "mitm k=" << k << " (only the whole set): " << ms(t0, t1) << " ms\n";
    }
    {   // a planted zero-sum subset of 5 among random large values: result must be at most 5 elements
        Rng rng(84);
        vector<Amount> v;
        for(int i = 0; i < k - 5; i++){
            Amount x = 0;
            while(x == 0) x = rng.uniform(-1000000000000LL, 1000000000000LL);
            v.push_back(x);
        }
        Amount a = rng.uniform(1, 1000000000000LL), b = rng.uniform(1, 1000000000000LL), c = rng.uniform(1, 1000000000000LL);
        Amount d = rng.uniform(1, 1000000000000LL);
        v.push_back(a); v.push_back(b); v.push_back(c); v.push_back(d); v.push_back(-(a + b + c + d));
        shuffle_vec(rng, v);
        vector<int> got = mitm_zero_subset(v);
        CHECK(!got.empty());
        CHECK(got.size() <= 5);
        if(!got.empty()) CHECK(subset_problem(v, got).empty());
    }
    {   // four values of 2^62 add up to 2^64, which WRAPS to 0 in 64-bit arithmetic: must not count as zero-sum
        vector<Amount> v(4, (Amount)1 << 62);
        v.push_back(1);
        CHECK(mitm_zero_subset(v).empty());
    }
    {   // partial sums beyond int64 (three 4e18 in one half add up to 1.2e19): needs 128-bit sums
        vector<Amount> v = {4000000000000000000LL, 4000000000000000000LL, 4000000000000000000LL,
                            -4000000000000000000LL, -4000000000000000000LL, -4000000000000000000LL, 5, -5};
        vector<int> got = mitm_zero_subset(v);
        CHECK(got.size() == 2);
        if(!got.empty()) CHECK(subset_problem(v, got).empty());
    }
}

// ------------------------------------------------------------------ D. preconditions
static void test_mitm_preconditions(){
    CHECK(throws_invalid([]{ vector<Amount> v(41, 1); v[0] = -40; mitm_zero_subset(v); }));   // 41 elements
    CHECK(!throws_invalid([]{ vector<Amount> v(40, 1); v[0] = -39; mitm_zero_subset(v); }));  // 40 are fine
    CHECK(throws_invalid([]{ mitm_zero_subset({1, 0, -1}); }));                               // zero entry
}

// ------------------------------------------------------------------ E. refine_groups
static void test_refine_hand_cases(){
    {   // three hidden 4-groups, handed over as one big group
        vector<Amount> b = {1, 2, 4, -7, 10, 20, 40, -70, 100, 200, 400, -700};
        Groups g = refine_groups(b, {{0,1,2,3,4,5,6,7,8,9,10,11}});
        expect_partition("hidden groups", b, g);
        CHECK(g.size() == 3);
        for(const auto& grp : g) CHECK(grp.size() == 4);
    }
    {   // already irreducible groups stay as they are
        vector<Amount> b = {5, -5, 1, 2, -3, 7, -7};
        Groups in = {{0, 1}, {2, 3, 4}, {5, 6}};
        Groups g = refine_groups(b, in);
        expect_partition("irreducible", b, g);
        CHECK(g.size() == 3);
    }
    {   // a 6-element group {1,-1,2,-2,3,-3} splits all the way down
        vector<Amount> b = {1, -1, 2, -2, 3, -3};
        Groups g = refine_groups(b, {{0,1,2,3,4,5}});
        expect_partition("three pairs", b, g);
        CHECK(g.size() == 3);
    }
    {   // order of the input groups is kept: untouched groups come out identical
        vector<Amount> b = {1, 2, 4, -7, 9, -9};
        Groups in = {{4, 5}, {0, 1, 2, 3}};
        Groups g = refine_groups(b, in);
        expect_partition("keeps small groups", b, g);
        CHECK(g.size() == 2);
    }
    {   // 41 elements: over the limit, so the group is returned untouched even though it hides a pair
        vector<Amount> b;
        b.push_back(7);
        b.push_back(-7);
        Amount total = 0;
        for(int i = 0; i < 38; i++){ b.push_back((Amount)1 << i); total += (Amount)1 << i; }
        b.push_back(-total);
        CHECK(b.size() == 41);
        vector<int> all;
        for(size_t i = 0; i < b.size(); i++) all.push_back((int)i);
        Groups g = refine_groups(b, {all});
        expect_partition("over the limit", b, g);
        CHECK(g.size() == 1);
    }
    {   // exactly 40 elements: 35 powers of two and minus their total (irreducible), plus two hidden pairs
        vector<Amount> b;
        Amount total = 0;
        for(int i = 0; i < 35; i++){ b.push_back((Amount)1 << i); total += (Amount)1 << i; }
        b.push_back(-total);
        b.push_back(7);
        b.push_back(-7);
        b.push_back(11);
        b.push_back(-11);
        CHECK(b.size() == 40);
        vector<int> all;
        for(size_t i = 0; i < b.size(); i++) all.push_back((int)i);
        Groups g = refine_groups(b, {all});
        expect_partition("40 elements", b, g);
        CHECK(g.size() == 3);                        // both pairs peeled off, one irreducible group of 36
        size_t pairs = 0, big = 0;
        for(const auto& grp : g){ if(grp.size() == 2) pairs++; if(grp.size() == 36) big++; }
        CHECK(pairs == 2);
        CHECK(big == 1);
    }
}

// ------------------------------------------------------------------ F. refine_groups properties
static void test_refine_properties(){
    const int Rs[3] = {3, 5, 10};
    int total = 0, improved = 0, equal_exact = 0, below_exact = 0;

    for(int seed = 0; seed < 150; seed++){
        Rng rng(91000 + seed);
        int k = (int)rng.uniform(4, 20);
        vector<Amount> b = random_balances(rng, k, Rs[rng.uniform(0, 2)]);
        if(b.empty()) continue;

        Groups base = match_groups(b);
        Groups refined = refine_groups(b, base);
        expect_partition("refined", b, refined);

        CHECK(refined.size() >= base.size());                       // never worse
        for(const auto& grp : refined) CHECK(irreducible(b, grp));   // nothing left to peel

        int exact = (int)dp_max_groups(b).size();
        CHECK((int)refined.size() <= exact);                        // heuristic never beats the optimum

        // from one single group of everything, peeling also ends with irreducible groups
        vector<int> all(b.size());
        iota(all.begin(), all.end(), 0);
        Groups from_one = refine_groups(b, {all});
        expect_partition("from one group", b, from_one);
        for(const auto& grp : from_one) CHECK(irreducible(b, grp));
        CHECK((int)from_one.size() <= exact);

        // idempotent: refining twice changes nothing
        CHECK(refine_groups(b, refined).size() == refined.size());

        total++;
        if(refined.size() > base.size()) improved++;
        if((int)refined.size() == exact) equal_exact++; else below_exact++;
    }
    cout << "refine on " << total << " vectors (k 4..19): improved on match for " << improved
         << ", equals exact on " << equal_exact << ", below exact on " << below_exact << "\n";
}

// ------------------------------------------------------------------ G. solver on generated instances
static void test_solver_on_generated(){
    MitmSolver mitm; MatchSolver match; SubsetDpSolver exact;
    const Amount amts[3] = {2, 5, 1000000000000LL};

    int instances = 0, better_than_match = 0, worse_than_exact = 0;

    for(int seed = 0; seed < 200; seed++){
        Rng p(7500 + seed);
        Amount max_amt = amts[p.uniform(0, 2)];

        Instance inst;
        switch(seed % 4){
            case 0: {
                int n = (int)p.uniform(2, 16), m = (int)p.uniform(0, 50);
                inst = gen_random(n, m, max_amt, seed);
                break;
            }
            case 1: {
                int n = (int)p.uniform(2, 16), m = (int)p.uniform(0, 50);
                inst = gen_hub(n, m, p.next_double() * 3.0, max_amt, seed);
                break;
            }
            case 2: {
                int n = (int)p.uniform(2, 16);
                int lo = (int)p.uniform(2, n), hi = (int)p.uniform(lo, n);
                inst = gen_cycles(n, (int)p.uniform(0, 4), lo, hi, (int)p.uniform(0, 4), max_amt, seed);
                break;
            }
            default: {
                int groups = (int)p.uniform(1, 5);
                int n = (int)p.uniform(2 * groups, 18), m = (int)p.uniform(0, 60);
                inst = gen_clusters(n, groups, m, max_amt, seed);
                break;
            }
        }

        size_t nz = nonzero_balances(inst).size();
        Plan pm = mitm.solve(inst);
        expect_valid("match_mitm", inst, pm);

        size_t tm = match.solve(inst).transfers.size();
        size_t te = exact.solve(inst).transfers.size();
        CHECK(pm.transfers.size() <= tm);            // refinement never costs transfers
        CHECK(pm.transfers.size() >= te);            // and never beats the optimum
        CHECK(pm.transfers.size() <= (nz == 0 ? 0 : nz - 1));

        instances++;
        if(pm.transfers.size() < tm) better_than_match++;
        if(pm.transfers.size() > te) worse_than_exact++;
    }
    cout << "match_mitm solver on " << instances << " small instances: fewer transfers than match on "
         << better_than_match << ", still above exact on " << worse_than_exact << "\n";
}

// ------------------------------------------------------------------ H. edges
static void test_edges(){
    MitmSolver mitm;
    {
        Instance inst;
        inst.n = 1;
        CHECK(mitm.solve(inst).transfers.empty());
    }
    {   // zero-balance parties between nonzero ones
        Instance inst = instance_from_balances({3, 0, -3, 0, 5, -5});
        Plan p = mitm.solve(inst);
        expect_valid("zeros between", inst, p);
        CHECK(p.transfers.size() == 2);
    }
    {   // hidden 4-groups through the full solver: match alone needs 11 transfers, with refinement 9
        vector<Amount> bal = {1, 2, 4, -7, 10, 20, 40, -70, 100, 200, 400, -700};
        Instance inst = instance_from_balances(bal);
        Plan p = mitm.solve(inst);
        expect_valid("hidden groups", inst, p);
        CHECK(p.transfers.size() == 9);
        CHECK(MatchSolver().solve(inst).transfers.size() == 11);
    }
}

// ------------------------------------------------------------------ I. scale: eight hidden groups of 4 in 32 elements
static void test_scale(){
    using clk = chrono::steady_clock;
    auto ms = [](clk::time_point a, clk::time_point b){
        return chrono::duration_cast<chrono::milliseconds>(b - a).count();
    };

    Rng rng(95);
    vector<Amount> bal;
    for(int g = 0; g < 8; g++){
        Amount a = rng.uniform(1000000000LL, 300000000000LL);
        Amount c = rng.uniform(1000000000LL, 300000000000LL);
        Amount d = rng.uniform(1000000000LL, 300000000000LL);
        bal.push_back(a); bal.push_back(c); bal.push_back(d); bal.push_back(-(a + c + d));
    }
    shuffle_vec(rng, bal);
    Instance inst = instance_from_balances(bal);

    MitmSolver mitm; MatchSolver match; GreedySolver greedy;
    auto t0 = clk::now();
    Plan pm = mitm.solve(inst);
    auto t1 = clk::now();
    expect_valid("scale", inst, pm);
    size_t tm = match.solve(inst).transfers.size();
    size_t tg = greedy.solve(inst).transfers.size();

    CHECK(pm.transfers.size() <= 24);                 // 32 elements in 8 groups of 4: at most 24 transfers
    CHECK(tm == 31);                                  // match alone sees no pairs or triples
    cout << "scale 8 hidden 4-groups (32 parties): greedy " << tg << ", match " << tm
         << ", match_mitm " << pm.transfers.size() << "  (" << ms(t0, t1) << " ms)\n";
}

int main(int argc, char** argv){
    bool fast = (argc > 1 && string(argv[1]) == "--fast");

    test_mitm_hand_cases();
    test_mitm_vs_brute(fast);
    test_mitm_large(fast);
    test_mitm_preconditions();
    test_refine_hand_cases();
    test_refine_properties();
    test_solver_on_generated();
    test_edges();
    test_scale();

    if(failures){
        cerr << failures << " check(s) failed\n";
        return 1;
    }
    cout << "all tests passed\n";
    return 0;
}