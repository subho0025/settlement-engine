#include "gen/generators.hpp"
#include "core/io.hpp"
#include "core/net_balance.hpp"
#include "core/rng.hpp"

using namespace std;

static int failures = 0;

#define CHECK(cond) \
    do { if(!(cond)) { cerr << __FILE__ << ":" << __LINE__ << "  CHECK failed: " #cond "\n"; failures++; } } while(0)

static string to_str(const Instance& inst){
    ostringstream o;
    write_instance(o, inst);
    return o.str();
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

// ---------------------------------------------------------------- A. Rng
static void test_rng(){
    {
        Rng r(0);
        CHECK(r.next() == 0xE220A8397B1DCDAFULL);
        CHECK(r.next() == 0x6E789E6AA1B965F4ULL);
        CHECK(r.next() == 0x06C45D188009454FULL);
    }
    {
        Rng a(42), b(42);
        bool same = true;
        for(int i = 0; i < 1000; i++) if(a.next() != b.next()) same = false;
        CHECK(same);

        Rng c(1), d(2);
        bool differ = false;
        for(int i = 0; i < 10; i++) if(c.next() != d.next()) differ = true;
        CHECK(differ);
    }
    {
        Rng r(7);
        bool fixed_ok = true, range_ok = true;
        for(int i = 0; i < 10000; i++){
            if(r.uniform(5, 5) != 5) fixed_ok = false;
            int64_t v = r.uniform(-3, 3);
            if(v < -3 || v > 3) range_ok = false;
        }
        CHECK(fixed_ok);
        CHECK(range_ok);
    }
    {
        Rng r(99);
        long long cnt[3] = {0, 0, 0};
        for(int i = 0; i < 300000; i++) cnt[r.uniform(0, 2)]++;
        for(int b = 0; b < 3; b++) CHECK(llabs(cnt[b] - 100000) <= 2000);
    }
    {
        Rng r(3);
        bool ok = true;
        for(int i = 0; i < 100000; i++){
            double x = r.next_double();
            if(!(x >= 0.0 && x < 1.0)) ok = false;
        }
        CHECK(ok);
    }
}

// ---------------------------------------------------------------- B. universal properties
static void check_universal(const char* name, const Instance& inst, int n, long long expect_m, Amount max_amt){
    CHECK(inst.n == n);
    if(expect_m >= 0) CHECK((long long)inst.obs.size() == expect_m);

    bool bad = false;
    for(const Obligation& o : inst.obs){
        if(o.from == o.to || o.from < 0 || o.from >= n || o.to < 0 || o.to >= n || o.amt < 1 || o.amt > max_amt) bad = true;
    }
    CHECK(!bad);
    if(bad) cerr << "  [" << name << "] bad obligation\n";

    stringstream ss;
    write_instance(ss, inst);
    bool threw = false;
    try{ read_instance(ss); }catch(...){ threw = true; }
    CHECK(!threw);

    Amount sum = 0;
    for(Amount x : net_balance(inst)) sum += x;
    CHECK(sum == 0);
}

static void test_universal(){
    const Amount amts[3] = {1, 7, 1000000000000LL};

    for(int seed = 0; seed < 200; seed++){
        Rng p(1000 + seed);
        Amount max_amt = amts[p.uniform(0, 2)];

        {   // random
            int n = (int)p.uniform(2, 40), m = (int)p.uniform(0, 150);
            check_universal("random", gen_random(n, m, max_amt, seed), n, m, max_amt);
        }
        {   // hub
            int n = (int)p.uniform(2, 40), m = (int)p.uniform(0, 150);
            double skew = p.next_double() * 3.0;
            check_universal("hub", gen_hub(n, m, skew, max_amt, seed), n, m, max_amt);
        }
        {   // cycles
            int n = (int)p.uniform(2, 40);
            int lo = (int)p.uniform(2, n), hi = (int)p.uniform(lo, n);
            int cyc = (int)p.uniform(0, 10), noise = (int)p.uniform(0, 5);
            Instance inst = gen_cycles(n, cyc, lo, hi, noise, max_amt, seed);
            long long exact = (lo == hi) ? (long long)cyc * lo + noise : -1;
            check_universal("cycles", inst, n, exact, max_amt);
            CHECK((long long)inst.obs.size() >= (long long)cyc * lo + noise);
            CHECK((long long)inst.obs.size() <= (long long)cyc * hi + noise);
        }
        {   // clusters
            int groups = (int)p.uniform(1, 10);
            int n = (int)p.uniform(2 * groups, 60), m = (int)p.uniform(0, 200);
            check_universal("clusters", gen_clusters(n, groups, m, max_amt, seed), n, m, max_amt);
        }
    }
}

// ---------------------------------------------------------------- C. determinism
static void test_determinism(){
    CHECK(to_str(gen_random(50, 200, 1000, 5))      == to_str(gen_random(50, 200, 1000, 5)));
    CHECK(to_str(gen_random(50, 200, 1000, 5))      != to_str(gen_random(50, 200, 1000, 6)));
    CHECK(to_str(gen_hub(50, 200, 1.0, 1000, 5))    == to_str(gen_hub(50, 200, 1.0, 1000, 5)));
    CHECK(to_str(gen_hub(50, 200, 1.0, 1000, 5))    != to_str(gen_hub(50, 200, 1.0, 1000, 6)));
    CHECK(to_str(gen_cycles(50, 10, 2, 6, 3, 1000, 5)) == to_str(gen_cycles(50, 10, 2, 6, 3, 1000, 5)));
    CHECK(to_str(gen_cycles(50, 10, 2, 6, 3, 1000, 5)) != to_str(gen_cycles(50, 10, 2, 6, 3, 1000, 6)));
    CHECK(to_str(gen_clusters(50, 5, 200, 1000, 5)) == to_str(gen_clusters(50, 5, 200, 1000, 5)));
    CHECK(to_str(gen_clusters(50, 5, 200, 1000, 5)) != to_str(gen_clusters(50, 5, 200, 1000, 6)));
}

// ---------------------------------------------------------------- D. cycles
static void test_cycles(){
    for(int seed = 0; seed < 50; seed++){
        Instance a = gen_cycles(30, 20, 2, 10, 0, 1000000, seed);
        for(Amount x : net_balance(a)) CHECK(x == 0);

        Instance b = gen_cycles(30, 20, 2, 10, 1, 1000000, seed);
        bool nonzero = false;
        for(Amount x : net_balance(b)) if(x != 0) nonzero = true;
        CHECK(nonzero);
    }
    {   // one cycle through all 5 parties: every party exactly once as from and once as to
        Instance c = gen_cycles(5, 1, 5, 5, 0, 100, 1);
        vector<int> outd(5, 0), ind(5, 0);
        for(const Obligation& o : c.obs){ outd[o.from]++; ind[o.to]++; }
        for(int i = 0; i < 5; i++){
            CHECK(outd[i] == 1);
            CHECK(ind[i] == 1);
        }
    }
}

// ---------------------------------------------------------------- E. hub
static vector<long long> incident(const Instance& inst){
    vector<long long> d(inst.n, 0);
    for(const Obligation& o : inst.obs){ d[o.from]++; d[o.to]++; }
    return d;
}

static void test_hub(){
    {
        Instance u = gen_hub(100, 100000, 0.0, 1000, 11);
        vector<long long> d = incident(u);
        double avg = 2.0 * 100000 / 100;
        CHECK((double)*max_element(d.begin(), d.end()) < 2.0 * avg);
    }
    {
        Instance h = gen_hub(1000, 100000, 1.0, 1000, 12);
        vector<long long> d = incident(h);
        double avg = 2.0 * 100000 / 1000;
        CHECK((double)d[0] > 10.0 * avg);
        CHECK(d[0] > d[999]);
    }
}

// ---------------------------------------------------------------- F. clusters
static void test_clusters(){
    const int n = 100, groups = 7, m = 1000;
    vector<int> gid;
    Instance inst = gen_clusters(n, groups, m, 1000, 21, &gid);

    CHECK((int)gid.size() == n);
    bool ids_ok = true;
    vector<int> size(groups, 0);
    for(int g : gid){
        if(g < 0 || g >= groups) ids_ok = false;
        else size[g]++;
    }
    CHECK(ids_ok);
    if(!ids_ok) return;
    CHECK(*max_element(size.begin(), size.end()) - *min_element(size.begin(), size.end()) <= 1);

    bool same_group = true;
    for(const Obligation& o : inst.obs) if(gid[o.from] != gid[o.to]) same_group = false;
    CHECK(same_group);

    vector<Amount> bal = net_balance(inst);
    vector<Amount> gsum(groups, 0);
    for(int i = 0; i < n; i++) gsum[gid[i]] += bal[i];
    for(int g = 0; g < groups; g++) CHECK(gsum[g] == 0);
}

// ---------------------------------------------------------------- G. validation
static void test_validation(){
    CHECK(throws_invalid([]{ gen_random(1, 5, 100, 1); }));
    CHECK(throws_invalid([]{ gen_random(10, -1, 100, 1); }));
    CHECK(throws_invalid([]{ gen_random(10, 5, 0, 1); }));
    CHECK(throws_invalid([]{ gen_random(10, 5, 2000000000000LL, 1); }));
    CHECK(throws_invalid([]{ gen_random(100001, 5, 100, 1); }));
    CHECK(throws_invalid([]{ gen_random(10, 1000001, 100, 1); }));
    CHECK(throws_invalid([]{ gen_hub(10, 5, -0.5, 100, 1); }));
    CHECK(throws_invalid([]{ gen_hub(10, 5, 3.5, 100, 1); }));
    CHECK(throws_invalid([]{ gen_cycles(5, 1, 1, 3, 0, 100, 1); }));
    CHECK(throws_invalid([]{ gen_cycles(5, 1, 2, 6, 0, 100, 1); }));
    CHECK(throws_invalid([]{ gen_cycles(5, 1, 3, 2, 0, 100, 1); }));
    CHECK(throws_invalid([]{ gen_cycles(5, 1, 2, 3, -1, 100, 1); }));
    CHECK(throws_invalid([]{ gen_cycles(100, 200000, 2, 10, 0, 100, 1); }));
    CHECK(throws_invalid([]{ gen_clusters(5, 3, 10, 100, 1); }));
    CHECK(throws_invalid([]{ gen_clusters(10, 0, 10, 100, 1); }));

    // boundaries that must be accepted
    CHECK(!throws_invalid([]{ gen_random(2, 0, 1, 1); }));
    CHECK(!throws_invalid([]{ gen_hub(2, 10, 3.0, 1, 1); }));
    CHECK(!throws_invalid([]{ gen_cycles(2, 3, 2, 2, 0, 100, 1); }));
    CHECK(!throws_invalid([]{ gen_clusters(4, 2, 10, 100, 1); }));
}

// ---------------------------------------------------------------- H. scale
static void test_scale(){
    using clk = chrono::steady_clock;
    auto ms = [](clk::time_point a, clk::time_point b){
        return chrono::duration_cast<chrono::milliseconds>(b - a).count();
    };

    auto t0 = clk::now();
    Instance r = gen_random(100000, 1000000, 1000000000000LL, 1);
    auto t1 = clk::now();
    Instance h = gen_hub(100000, 1000000, 1.0, 1000000000000LL, 1);
    auto t2 = clk::now();
    Instance c = gen_cycles(100000, 100000, 2, 10, 1000, 1000000000000LL, 1);
    auto t3 = clk::now();
    Instance k = gen_clusters(100000, 1000, 1000000, 1000000000000LL, 1);
    auto t4 = clk::now();

    CHECK(r.obs.size() == 1000000);
    CHECK(h.obs.size() == 1000000);
    CHECK(k.obs.size() == 1000000);
    CHECK(c.obs.size() > 0);

    cout << "scale (n=1e5, m~1e6): random " << ms(t0,t1) << " ms, hub " << ms(t1,t2)
         << " ms, cycles " << ms(t2,t3) << " ms, clusters " << ms(t3,t4) << " ms\n";
}

int main(int argc, char** argv){
    bool run_scale = !(argc > 1 && string(argv[1]) == "--fast");

    test_rng();
    test_universal();
    test_determinism();
    test_cycles();
    test_hub();
    test_clusters();
    test_validation();
    if(run_scale) test_scale();

    if(failures){
        cerr << failures << " check(s) failed\n";
        return 1;
    }
    cout << "all tests passed\n";
    return 0;
}