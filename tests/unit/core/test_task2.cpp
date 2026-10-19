#include "core/validator.hpp"
#include "core/net_balance.hpp"

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

static Plan make_plan(const Edges& edges){
    Plan p;
    for(const auto& e : edges){
        Transfer t;
        t.from = e[0];
        t.to = e[1];
        t.amt = e[2];
        p.transfers.push_back(t);
    }
    return p;
}

static void expect_ok(const char* name, const Instance& inst, const Plan& p, size_t transfers, Amount moved){
    ValidationResult r = validate_netting(inst, p);
    CHECK(r.ok);
    if(!r.ok){
        cerr << "  [" << name << "] unexpected error: " << r.error << "\n";
        return;
    }
    CHECK(r.error.empty());
    CHECK(r.transfers == transfers);
    CHECK(r.total_moved == moved);
}

static void expect_bad(const char* name, const Instance& inst, const Plan& p){
    ValidationResult r = validate_netting(inst, p);
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    if(r.ok) cerr << "  [" << name << "] accepted an invalid plan\n";
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

// throwaway helper: everyone settles through party 0
static Plan hub_plan(const Instance& inst){
    vector<Amount> bal = net_balance(inst);
    Plan p;
    for(int i = 1; i < inst.n; i++){
        Transfer t;
        if(bal[i] < 0){
            t.from = i; t.to = 0; t.amt = -bal[i];
            p.transfers.push_back(t);
        }else if(bal[i] > 0){
            t.from = 0; t.to = i; t.amt = bal[i];
            p.transfers.push_back(t);
        }
    }
    return p;
}

static void test_valid_plans(){
    Instance cycle = make(3, {{0,1,100},{1,2,100},{2,0,100}});
    Instance star  = make(5, {{1,0,10},{2,0,20},{3,0,30},{4,0,40}});

    expect_ok("cycle/empty",   cycle, make_plan({}), 0, 0);
    expect_ok("cycle/rotate",  cycle, make_plan({{0,1,5},{1,2,5},{2,0,5}}), 3, 15);
    expect_ok("star/direct",   star,  make_plan({{1,0,10},{2,0,20},{3,0,30},{4,0,40}}), 4, 100);
    expect_ok("star/chain",    star,  make_plan({{4,3,40},{3,2,70},{2,1,90},{1,0,100}}), 4, 300);
    expect_ok("n1",            make(1, {}), make_plan({}), 0, 0);
}

static void test_invalid_by_effect(){
    Instance cycle = make(3, {{0,1,100},{1,2,100},{2,0,100}});
    Instance star  = make(5, {{1,0,10},{2,0,20},{3,0,30},{4,0,40}});

    expect_bad("cycle/one transfer", cycle, make_plan({{0,1,5}}));
    expect_bad("star/off by one",    star,  make_plan({{1,0,10},{2,0,20},{3,0,30},{4,0,41}}));
    expect_bad("star/reversed",      star,  make_plan({{0,1,10},{0,2,20},{0,3,30},{0,4,40}}));
    expect_bad("star/missing party", star,  make_plan({{1,0,10},{2,0,20},{3,0,30}}));
}

static void test_invalid_by_structure(){
    Instance two = make(2, {});

    expect_bad("from == n",      two, make_plan({{2,0,5}}));
    expect_bad("to == n",        two, make_plan({{0,2,5}}));
    expect_bad("from < 0",       two, make_plan({{-1,0,5}}));
    expect_bad("to < 0",         two, make_plan({{0,-1,5}}));
    expect_bad("amt == 0",       two, make_plan({{0,1,0}}));
    expect_bad("amt < 0",        two, make_plan({{0,1,-5}}));
    expect_bad("amt == 2e18",    two, make_plan({{0,1,2000000000000000000LL}}));
    expect_bad("amt == 1e18+1",  two, make_plan({{0,1,1000000000000000001LL}}));
    expect_bad("self transfer",  two, make_plan({{1,1,5}}));
}

// 1e18 is the largest legal transfer; a balance of exactly 1e18 is reachable with 1e6 x 1e12
static void test_boundary_1e18(){
    Instance big;
    big.n = 2;
    for(int i = 0; i < 1000000; i++){
        Obligation o;
        o.from = 0;
        o.to = 1;
        o.amt = 1000000000000LL;
        big.obs.push_back(o);
    }
    expect_ok("1e18 transfer", big, make_plan({{0,1,1000000000000000000LL}}), 1, 1000000000000000000LL);
    expect_bad("1e18 split wrongly", big, make_plan({{0,1,999999999999999999LL}}));
}

static void test_overflow_safety(){
    Instance zero = make(2, {});
    Edges e;
    for(int i = 0; i < 10; i++) e.push_back({0,1,900000000000000000LL});
    for(int i = 0; i < 10; i++) e.push_back({1,0,900000000000000000LL});
    Plan p = make_plan(e);

    ValidationResult r = validate_netting(zero, p);   // net effect is zero, total moved is 1.8e19
    CHECK(!r.ok);
    CHECK(r.error.find("overflow") != string::npos);
}

static void test_random_property(){
    for(int seed = 0; seed < 500; seed++){
        mt19937_64 rng(5000 + seed);
        Instance inst = random_instance(rng, 30, 100);
        Plan hp = hub_plan(inst);

        Amount sum = 0;
        for(const Transfer& t : hp.transfers) sum += t.amt;
        expect_ok("hub plan", inst, hp, hp.transfers.size(), sum);

        if(hp.transfers.empty()) continue;
        size_t idx = uniform_int_distribution<size_t>(0, hp.transfers.size() - 1)(rng);

        Plan inc = hp;
        inc.transfers[idx].amt += 1;
        expect_bad("mutate: amt+1", inst, inc);

        Plan del = hp;
        del.transfers.erase(del.transfers.begin() + idx);
        expect_bad("mutate: delete", inst, del);

        Plan swp = hp;
        swap(swp.transfers[idx].from, swp.transfers[idx].to);
        expect_bad("mutate: swap", inst, swp);
    }
}

int main(){
    test_valid_plans();
    test_invalid_by_effect();
    test_invalid_by_structure();
    test_boundary_1e18();
    test_overflow_safety();
    test_random_property();

    if(failures){
        cerr << failures << " check(s) failed\n";
        return 1;
    }
    cout << "all tests passed\n";
    return 0;
}