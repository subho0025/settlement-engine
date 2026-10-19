#include "core/io.hpp"
#include "core/net_balance.hpp"

using namespace std;

static int failures = 0;

#define CHECK(cond) \
    do { if(!(cond)) { cerr << __FILE__ << ":" << __LINE__ << "  CHECK failed: " #cond "\n"; failures++; } } while(0)

static Instance make(int n, const vector<array<long long,3>>& edges){
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

// O(n*m) reference that shares no code with net_balance
static vector<Amount> slow_balance(const Instance& inst){
    vector<Amount> b(inst.n, 0);
    for(int i = 0; i < inst.n; i++){
        for(const Obligation& o : inst.obs){
            if(o.to == i)   b[i] += o.amt;
            if(o.from == i) b[i] -= o.amt;
        }
    }
    return b;
}

static Instance random_instance(mt19937_64& rng, int maxn, int maxm){
    int n = uniform_int_distribution<int>(1, maxn)(rng);
    int m = uniform_int_distribution<int>(0, maxm)(rng);
    Instance inst;
    inst.n = n;
    for(int i = 0; i < m; i++){
        Obligation o;
        o.from = uniform_int_distribution<int>(0, n - 1)(rng);
        o.to   = uniform_int_distribution<int>(0, n - 1)(rng);
        o.amt  = uniform_int_distribution<long long>(1, 1000000000000LL)(rng);
        inst.obs.push_back(o);
    }
    return inst;
}

static bool read_throws(const string& text){
    istringstream in(text);
    try{
        read_instance(in);
    }catch(const runtime_error&){
        return true;
    }catch(...){
        return false; // wrong exception type
    }
    return false;
}

static void test_hand_cases(){
    CHECK(net_balance(make(3, {{0,1,100},{1,2,100},{2,0,100}})) == vector<Amount>({0,0,0}));
    CHECK(net_balance(make(5, {{1,0,10},{2,0,20},{3,0,30},{4,0,40}})) == vector<Amount>({100,-10,-20,-30,-40}));
    CHECK(net_balance(make(2, {{1,1,50},{0,0,7}})) == vector<Amount>({0,0}));
    CHECK(net_balance(make(2, {{0,1,5},{0,1,7}})) == vector<Amount>({-12,12}));
    CHECK(net_balance(make(1, {})) == vector<Amount>({0}));
    CHECK(net_balance(make(4, {})) == vector<Amount>({0,0,0,0}));
}

static void test_random_property(){
    for(int seed = 0; seed < 1000; seed++){
        mt19937_64 rng(seed);
        Instance inst = random_instance(rng, 50, 200);
        vector<Amount> b = net_balance(inst);
        Amount sum = 0;
        for(Amount x : b) sum += x;
        CHECK(sum == 0);
        CHECK(b == slow_balance(inst));
    }
}

static void test_round_trip(){
    {
        ostringstream out;
        write_instance(out, make(3, {{0,1,100},{1,2,100},{2,0,100}}));
        CHECK(out.str() == "3 3\n0 1 100\n1 2 100\n2 0 100\n");
    }
    for(int seed = 0; seed < 200; seed++){
        mt19937_64 rng(1000 + seed);
        Instance a = random_instance(rng, 50, 200);
        stringstream ss;
        write_instance(ss, a);
        Instance b = read_instance(ss);
        CHECK(a.n == b.n);
        CHECK(a.obs.size() == b.obs.size());
        if(a.obs.size() != b.obs.size()) continue;
        for(size_t i = 0; i < a.obs.size(); i++){
            CHECK(a.obs[i].from == b.obs[i].from);
            CHECK(a.obs[i].to   == b.obs[i].to);
            CHECK(a.obs[i].amt  == b.obs[i].amt);
        }
    }
}

static void test_malformed(){
    CHECK(read_throws(""));                                   // empty
    CHECK(read_throws("3"));                                  // no m
    CHECK(read_throws("3 2\n0 1 5\n"));                       // truncated
    CHECK(read_throws("2 1\n0 a 5"));                         // non-numeric
    CHECK(read_throws("0 0"));                                // n = 0
    CHECK(read_throws("2 1000001"));                          // m too big
    CHECK(read_throws("2 1\n0 2 5"));                         // to == n
    CHECK(read_throws("2 1\n-1 0 5"));                        // negative index
    CHECK(read_throws("2 1\n0 1 0"));                         // amt = 0
    CHECK(read_throws("2 1\n0 1 -3"));                        // amt < 0
    CHECK(read_throws("2 1\n0 1 1000000000001"));             // amt > 1e12
    CHECK(read_throws("2 1\n0 1 5\n1 0 2"));                  // trailing data

    // valid input with trailing whitespace must not throw
    {
        istringstream in("2 1\n0 1 5\n\n  \n");
        bool threw = false;
        Instance inst;
        try{ inst = read_instance(in); }catch(...){ threw = true; }
        CHECK(!threw);
        if(!threw){
            CHECK(inst.n == 2);
            CHECK(inst.obs.size() == 1);
        }
    }
    // valid input with no trailing newline must not throw
    {
        istringstream in("2 1\n0 1 5");
        bool threw = false;
        try{ read_instance(in); }catch(...){ threw = true; }
        CHECK(!threw);
    }
    // boundary values accepted
    {
        istringstream in("100000 1\n99999 0 1000000000000\n");
        bool threw = false;
        try{ read_instance(in); }catch(...){ threw = true; }
        CHECK(!threw);
    }
}

static void test_scale(){
    using clk = chrono::steady_clock;
    auto ms = [](clk::time_point a, clk::time_point b){
        return chrono::duration_cast<chrono::milliseconds>(b - a).count();
    };

    mt19937_64 rng(12345);
    Instance inst;
    inst.n = 100000;
    inst.obs.reserve(1000000);
    for(int i = 0; i < 1000000; i++){
        Obligation o;
        o.from = uniform_int_distribution<int>(0, inst.n - 1)(rng);
        o.to   = uniform_int_distribution<int>(0, inst.n - 1)(rng);
        o.amt  = uniform_int_distribution<long long>(1, 1000000000000LL)(rng);
        inst.obs.push_back(o);
    }

    stringstream ss;
    auto t0 = clk::now();
    write_instance(ss, inst);
    auto t1 = clk::now();
    Instance back = read_instance(ss);
    auto t2 = clk::now();
    vector<Amount> b = net_balance(back);
    auto t3 = clk::now();

    Amount sum = 0;
    for(Amount x : b) sum += x;
    CHECK(sum == 0);
    CHECK(back.obs.size() == inst.obs.size());

    cout << "scale (n=1e5, m=1e6): write " << ms(t0,t1) << " ms, read " << ms(t1,t2)
         << " ms, net_balance " << ms(t2,t3) << " ms\n";
}

int main(int argc, char** argv){
    bool run_scale = !(argc > 1 && string(argv[1]) == "--fast");

    test_hand_cases();
    test_random_property();
    test_round_trip();
    test_malformed();
    if(run_scale) test_scale();

    if(failures){
        cerr << failures << " check(s) failed\n";
        return 1;
    }
    cout << "all tests passed\n";
    return 0;
}