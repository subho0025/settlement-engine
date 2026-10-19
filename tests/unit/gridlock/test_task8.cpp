#include "core/io.hpp"
#include "core/validator.hpp"
#include "gridlock/gridlock.hpp"
#include "gridlock/fifo_bypass.hpp"
#include "brute/brute_gridlock.hpp"
#include "core/rng.hpp"

using namespace std;

// Contract used by this file. WHERE each declaration lives does not matter, as long as it is reachable through the
// includes above and declared in exactly one place:
//   Instance read_gridlock_instance(istream&)           void write_gridlock_instance(ostream&, const Instance&)
//   struct GridlockCheck { bool ok; string error; size_t settled_count; Amount settled_value; }
//   GridlockCheck validate_gridlock(const Instance&, const vector<int>& selected)
//   class GridlockSolver { name(); solve() }            class FifoBypassSolver : public GridlockSolver ("fifo_bypass")
// A payment set is feasible when, settling ALL of it at once, every bank ends with
//   balance + credit_limit + incoming - outgoing >= 0.

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

static string to_text(const Instance& inst){
    ostringstream o;
    write_gridlock_instance(o, inst);
    return o.str();
}

static bool read_throws(const string& text){
    istringstream in(text);
    try{
        read_gridlock_instance(in);
    }catch(const runtime_error&){
        return true;
    }catch(...){
        return false;      // wrong exception type
    }
    return false;
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

static void expect_ok(const char* label, const Instance& inst, const vector<int>& sel, size_t count, Amount value){
    GridlockCheck r = validate_gridlock(inst, sel);
    CHECK(r.ok);
    if(!r.ok){
        cerr << "  [" << label << "] unexpected error: " << r.error << "\n";
        return;
    }
    CHECK(r.error.empty());
    CHECK(r.settled_count == count);
    CHECK(r.settled_value == value);
}

static void expect_bad(const char* label, const Instance& inst, const vector<int>& sel){
    GridlockCheck r = validate_gridlock(inst, sel);
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    if(r.ok) cerr << "  [" << label << "] accepted an infeasible selection\n";
}

// ------------------------------------------------------------------ A. instance I/O
static void test_io(){
    Instance a = make({{5, 3}, {0, 0}}, {{0, 1, 4}});
    CHECK(to_text(a) == "2 1\n5 3\n0 0\n0 1 4\n");

    {   // read what was written
        istringstream in("2 1\n5 3\n0 0\n0 1 4\n");
        Instance b = read_gridlock_instance(in);
        CHECK(b.n == 2);
        CHECK(b.balance == vector<Amount>({5, 0}));
        CHECK(b.credit_limit == vector<Amount>({3, 0}));
        CHECK(b.obs.size() == 1);
        if(b.obs.size() == 1){
            CHECK(b.obs[0].from == 0);
            CHECK(b.obs[0].to == 1);
            CHECK(b.obs[0].amt == 4);
        }
    }

    for(int seed = 0; seed < 100; seed++){
        Rng rng(1000 + seed);
        int n = (int)rng.uniform(2, 12), m = (int)rng.uniform(0, 30);
        Instance x = random_gridlock(rng, n, m, 1000);
        istringstream in(to_text(x));
        Instance y = read_gridlock_instance(in);
        CHECK(to_text(y) == to_text(x));
    }

    // boundaries that must be accepted
    {
        istringstream in("1 0\n1000000000000 1000000000000\n");
        bool threw = false;
        try{ read_gridlock_instance(in); }catch(...){ threw = true; }
        CHECK(!threw);
    }
    {
        istringstream in("2 1\n0 0\n0 0\n0 1 1000000000000\n\n  \n");   // trailing whitespace is fine
        bool threw = false;
        try{ read_gridlock_instance(in); }catch(...){ threw = true; }
        CHECK(!threw);
    }

    // malformed input: each must throw std::runtime_error
    CHECK(read_throws(""));
    CHECK(read_throws("2"));
    CHECK(read_throws("0 0\n"));                              // n = 0
    CHECK(read_throws("100001 0\n"));                         // n too big
    CHECK(read_throws("2 1000001\n0 0\n0 0\n"));              // m too big
    CHECK(read_throws("2 0\n0 0\n"));                         // missing bank line
    CHECK(read_throws("2 0\n0 0\n0 x\n"));                    // non-numeric limit
    CHECK(read_throws("2 0\n-1 0\n0 0\n"));                   // negative balance
    CHECK(read_throws("2 0\n0 -1\n0 0\n"));                   // negative limit
    CHECK(read_throws("2 0\n1000000000001 0\n0 0\n"));        // balance > 1e12
    CHECK(read_throws("2 0\n0 1000000000001\n0 0\n"));        // limit > 1e12
    CHECK(read_throws("2 1\n0 0\n0 0\n"));                    // payment line missing
    CHECK(read_throws("2 1\n0 0\n0 0\n0 2 5\n"));             // to == n
    CHECK(read_throws("2 1\n0 0\n0 0\n-1 0 5\n"));            // negative index
    CHECK(read_throws("2 1\n0 0\n0 0\n1 1 5\n"));             // payer == payee
    CHECK(read_throws("2 1\n0 0\n0 0\n0 1 0\n"));             // amt 0
    CHECK(read_throws("2 1\n0 0\n0 0\n0 1 1000000000001\n"));  // amt > 1e12
    CHECK(read_throws("2 1\n0 0\n0 0\n0 1 5\n1 0 2\n"));      // trailing data
}

// ------------------------------------------------------------------ B. validator
static void test_validator(){
    // classic gridlock: nobody has any money, but the two payments cancel out
    Instance cyc = make({{0, 0}, {0, 0}}, {{0, 1, 10}, {1, 0, 10}});
    expect_ok ("cycle both",  cyc, {0, 1}, 2, 20);
    expect_ok ("cycle none",  cyc, {},     0, 0);
    expect_bad("cycle first", cyc, {0});
    expect_bad("cycle second",cyc, {1});

    // partial liquidity: bank 1 has 6, needs 10 for its payment, but receives 5 at the same time
    Instance part = make({{0, 0}, {6, 0}}, {{0, 1, 5}, {1, 0, 10}});
    expect_ok ("partial both", part, {0, 1}, 2, 15);
    expect_bad("partial only 0", part, {0});
    expect_bad("partial only 1", part, {1});

    // credit limit counts as spendable
    Instance cred = make({{0, 5}, {0, 0}}, {{0, 1, 5}, {0, 1, 1}});
    expect_ok ("credit exact",  cred, {0}, 1, 5);
    expect_bad("credit over",   cred, {0, 1});
    expect_ok ("credit second", cred, {1}, 1, 1);

    // exactly zero is fine
    Instance zero = make({{7, 0}, {0, 0}}, {{0, 1, 7}});
    expect_ok("ends at zero", zero, {0}, 1, 7);

    // structural problems
    Instance two = make({{100, 0}, {100, 0}}, {{0, 1, 1}, {1, 0, 1}, {0, 1, 2}});
    expect_ok ("sorted ok",     two, {0, 2}, 2, 3);
    expect_bad("unsorted",      two, {2, 0});
    expect_bad("duplicate",     two, {1, 1});
    expect_bad("out of range",  two, {3});
    expect_bad("negative index",two, {-1});

    {   // instance without balances (a netting instance) must be rejected, not crash
        Instance nb;
        nb.n = 2;
        Obligation o; o.from = 0; o.to = 1; o.amt = 1;
        nb.obs.push_back(o);
        expect_bad("no balances", nb, {0});
    }

    {   // many large payments: sums must not overflow
        Instance big = make({{1000000000000LL, 1000000000000LL}, {0, 0}}, {});
        for(int i = 0; i < 1000; i++){
            Obligation o; o.from = 1; o.to = 0; o.amt = 1000000000000LL;
            big.obs.push_back(o);
            Obligation q; q.from = 0; q.to = 1; q.amt = 1000000000000LL;
            big.obs.push_back(q);
        }
        vector<int> all(big.obs.size());
        iota(all.begin(), all.end(), 0);
        expect_ok("big", big, all, 2000, 2000000000000000LL);
    }
}

// ------------------------------------------------------------------ C. brute-force oracle sanity (so a wrong oracle cannot hide bugs)
static void test_brute(){
    Instance cyc = make({{0, 0}, {0, 0}}, {{0, 1, 10}, {1, 0, 10}});
    CHECK(brute_gridlock(cyc).best_value == 20);

    Instance part = make({{0, 0}, {6, 0}}, {{0, 1, 5}, {1, 0, 10}});
    CHECK(brute_gridlock(part).best_value == 15);

    Instance poor = make({{0, 0}, {0, 0}}, {{0, 1, 3}});
    CHECK(brute_gridlock(poor).best_value == 0);
    CHECK(brute_gridlock(poor).best_set.empty());

    // 3-cycle with one payment too large: only the two payments that fit are optimal
    Instance tri = make({{0, 0}, {0, 0}, {0, 0}}, {{0, 1, 5}, {1, 2, 5}, {2, 0, 5}, {0, 2, 1}});
    CHECK(brute_gridlock(tri).best_value == 15);
}

// ------------------------------------------------------------------ D. FIFO-bypass: hand cases
static void test_fifo_hand_cases(){
    FifoBypassSolver fifo;
    CHECK(fifo.name() == "fifo_bypass");

    {   // the first payment is blocked, the second is not and then unblocks the first (bypass + second pass)
        Instance inst = make({{0, 0}, {10, 0}}, {{0, 1, 5}, {1, 0, 10}});
        CHECK(fifo.solve(inst) == vector<int>({0, 1}));
    }
    {   // classic gridlock: sequential settlement is stuck although both payments could go through together
        Instance inst = make({{0, 0}, {0, 0}}, {{0, 1, 10}, {1, 0, 10}});
        CHECK(fifo.solve(inst).empty());
    }
    {   // partial-liquidity gridlock
        Instance inst = make({{0, 0}, {6, 0}}, {{0, 1, 5}, {1, 0, 10}});
        CHECK(fifo.solve(inst).empty());
    }
    {   // credit limit is spendable
        Instance inst = make({{0, 5}, {0, 0}}, {{0, 1, 5}});
        CHECK(fifo.solve(inst) == vector<int>({0}));
    }
    {   // a payment equal to the available liquidity settles
        Instance inst = make({{7, 0}, {0, 0}}, {{0, 1, 7}});
        CHECK(fifo.solve(inst) == vector<int>({0}));
    }
    {   // three passes are needed: 3->2 first, then 2->1, then 1->0
        Instance inst = make({{0, 0}, {0, 0}, {0, 0}, {5, 0}}, {{1, 0, 5}, {2, 1, 5}, {3, 2, 5}});
        CHECK(fifo.solve(inst) == vector<int>({0, 1, 2}));
    }
    {   // queue order decides who gets scarce money: bank 0 can pay only one of the two
        Instance inst = make({{5, 0}, {0, 0}, {0, 0}}, {{0, 1, 5}, {0, 2, 5}});
        CHECK(fifo.solve(inst) == vector<int>({0}));
    }
    {   // nothing to do
        Instance inst = make({{1, 1}}, {});
        CHECK(fifo.solve(inst).empty());
    }
}

// ------------------------------------------------------------------ E. FIFO-bypass: properties on random instances
static void test_fifo_properties(){
    FifoBypassSolver fifo;
    const Amount As[3] = {5, 20, 1000000000000LL};

    int total = 0, strictly_below_opt = 0;

    for(int seed = 0; seed < 300; seed++){
        Rng rng(5000 + seed);
        int n = (int)rng.uniform(2, 8), m = (int)rng.uniform(0, 12);
        Instance inst = random_gridlock(rng, n, m, As[rng.uniform(0, 2)]);

        vector<int> sel = fifo.solve(inst);
        GridlockCheck r = validate_gridlock(inst, sel);
        CHECK(r.ok);                                       // sequentially feasible => simultaneously feasible
        if(!r.ok) cerr << "  seed " << seed << ": " << r.error << "\n";

        // stuck state: every payment left in the queue is unaffordable with the final liquidity
        vector<__int128> liq(n);
        for(int i = 0; i < n; i++) liq[i] = (__int128)inst.balance[i] + inst.credit_limit[i];
        vector<char> in_sel(m, 0);
        for(int p : sel){
            in_sel[p] = 1;
            liq[inst.obs[p].from] -= inst.obs[p].amt;
            liq[inst.obs[p].to]   += inst.obs[p].amt;
        }
        for(int p = 0; p < m; p++){
            if(!in_sel[p]) CHECK(liq[inst.obs[p].from] < inst.obs[p].amt);
        }

        // never better than the optimum
        BruteGridlock opt = brute_gridlock(inst);
        CHECK(r.settled_value <= opt.best_value);
        total++;
        if(r.settled_value < opt.best_value) strictly_below_opt++;

        // deterministic
        CHECK(fifo.solve(inst) == sel);
    }
    cout << "fifo_bypass on " << total << " small instances: strictly below the optimum on " << strictly_below_opt << "\n";
    CHECK(strictly_below_opt > 0);       // the random set does contain gridlocks
}

// ------------------------------------------------------------------ F. scale
static void test_scale(bool fast){
    using clk = chrono::steady_clock;
    auto ms = [](clk::time_point a, clk::time_point b){
        return chrono::duration_cast<chrono::milliseconds>(b - a).count();
    };
    FifoBypassSolver fifo;

    {   // a chain needing one pass per link (queue in the worst order)
        int links = fast ? 1000 : 3000;
        Instance inst;
        inst.n = links + 1;
        for(int i = 0; i <= links; i++){
            inst.balance.push_back(i == links ? 5 : 0);
            inst.credit_limit.push_back(0);
        }
        // payment i: bank i+1 pays bank i. Only the last bank has money, and the queue lists the payments in the worst order:
        // payment links-1 settles in pass 1, payment links-2 only in pass 2, and so on.
        for(int i = 0; i < links; i++){
            Obligation o; o.from = i + 1; o.to = i; o.amt = 5;
            inst.obs.push_back(o);
        }
        auto t0 = clk::now();
        vector<int> sel = fifo.solve(inst);
        auto t1 = clk::now();
        CHECK((int)sel.size() == links);
        CHECK(validate_gridlock(inst, sel).ok);
        cout << "fifo_bypass chain of " << links << " links (" << links << " passes): " << ms(t0, t1) << " ms\n";
    }

    if(!fast){   // n = 1e5 banks, m = 1e6 payments
        Rng rng(77);
        Instance inst;
        inst.n = 100000;
        for(int i = 0; i < inst.n; i++){
            inst.balance.push_back(rng.uniform(0, 3000000));
            inst.credit_limit.push_back(0);
        }
        for(int p = 0; p < 1000000; p++){
            Obligation o;
            o.from = rng.uniform(0, inst.n - 1);
            o.to = rng.uniform(0, inst.n - 2);
            if(o.to >= o.from) o.to++;
            o.amt = rng.uniform(1, 1000000);
            inst.obs.push_back(o);
        }
        auto t0 = clk::now();
        vector<int> sel = fifo.solve(inst);
        auto t1 = clk::now();
        GridlockCheck r = validate_gridlock(inst, sel);
        CHECK(r.ok);
        CHECK(!sel.empty());
        cout << "fifo_bypass n=1e5 m=1e6: settled " << r.settled_count << " payments, " << ms(t0, t1) << " ms\n";
    }
}

int main(int argc, char** argv){
    bool fast = (argc > 1 && string(argv[1]) == "--fast");

    test_io();
    test_validator();
    test_brute();
    test_fifo_hand_cases();
    test_fifo_properties();
    test_scale(fast);

    if(failures){
        cerr << failures << " check(s) failed\n";
        return 1;
    }
    cout << "all tests passed\n";
    return 0;
}