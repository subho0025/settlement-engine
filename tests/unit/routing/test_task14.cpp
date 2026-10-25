#include "routing/router.hpp"
#include "core/rng.hpp"

using namespace std;

// Contract used by this file (includes/routing/router.hpp, src/routing/router.cpp):
//
//   struct Channel { int from, to; Amount cap; int fee_bps; };      a directed link: up to `cap` can move from -> to,
//                                                                    every unit moved costs fee_bps (1 bps = 0.01%)
//   struct Route   { bool ok; Amount amount; Amount fee; vector<Amount> flow; };
//       ok      the FULL amount was routed (all or nothing)
//       amount  the routed amount (the request if ok, else 0)
//       fee     sum over channels of flow_e * fee_bps_e   (in amount x bps; divide by 10000 for currency units)
//       flow    one entry per channel, in constructor order; all 0 when !ok
//
//   Router(int n, const vector<Channel>& channels)
//       throws invalid_argument: n < 1, endpoint out of range, from == to, cap < 0, fee_bps outside [0, 10000]
//   Route route(int src, int dst, Amount amt)
//       cheapest way to move exactly amt from src to dst over the REMAINING capacities, split over as many paths
//       as needed (min-cost flow). On success the flows are committed: remaining() drops by flow on every channel.
//       On failure nothing changes. src == dst or amt == 0: ok, fee 0, no flow.
//       throws invalid_argument: src/dst out of range, amt < 0
//   Route route_single_path(int src, int dst, Amount amt)
//       baseline: the whole amount on ONE path, every channel of which has remaining >= amt; cheapest such path
//       (fee = amt * sum of fee_bps on the path). Commits on success, same rules otherwise.
//   Amount max_routable(int src, int dst) const      max flow over the remaining capacities; changes nothing.
//                                                    src == dst: numeric_limits<Amount>::max()
//   const vector<Amount>& remaining() const          remaining capacity per channel, constructor order
//   int num_nodes() const
//
// Which optimal flow is returned is not pinned down when several have the same fee; the random sections check
// fees against independent oracles and check optimality with a certificate instead of comparing flows.

static int failures = 0;

#define CHECK(cond) \
    do { if(!(cond)) { cerr << __FILE__ << ":" << __LINE__ << "  CHECK failed: " #cond "\n"; failures++; } } while(0)

// ------------------------------------------------------------------ helpers
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

static vector<Channel> chans(const vector<array<long long,4>>& v){
    vector<Channel> c;
    for(const auto& x : v){
        Channel ch;
        ch.from = (int)x[0];
        ch.to = (int)x[1];
        ch.cap = x[2];
        ch.fee_bps = (int)x[3];
        c.push_back(ch);
    }
    return c;
}

static vector<Channel> random_channels(Rng& rng, int n, int k, Amount maxcap, int maxfee){
    vector<Channel> c;
    for(int i = 0; i < k; i++){
        Channel ch;
        ch.from = (int)rng.uniform(0, n - 1);
        ch.to = (int)rng.uniform(0, n - 2);
        if(ch.to >= ch.from) ch.to++;
        ch.cap = rng.uniform(0, maxcap);
        ch.fee_bps = (int)rng.uniform(0, maxfee);
        c.push_back(ch);
    }
    return c;
}

// Checks that `r` is a legal routing of amt from src to dst given capacities `cap` (before routing).
static bool route_legal(int n, const vector<Channel>& ch, const vector<Amount>& cap, int src, int dst, Amount amt,
                        const Route& r, string& why){
    if(r.flow.size() != ch.size()){ why = "flow has the wrong size"; return false; }
    if(!r.ok){
        if(r.amount != 0 || r.fee != 0){ why = "failed route must have amount 0 and fee 0"; return false; }
        for(Amount f : r.flow) if(f != 0){ why = "failed route must have no flow"; return false; }
        return true;
    }
    if(r.amount != amt){ why = "amount != requested"; return false; }
    vector<__int128> net(n, 0);
    __int128 fee = 0;
    for(size_t e = 0; e < ch.size(); e++){
        if(r.flow[e] < 0 || r.flow[e] > cap[e]){ why = "flow on channel " + to_string(e) + " outside [0, remaining]"; return false; }
        net[ch[e].from] += r.flow[e];
        net[ch[e].to]   -= r.flow[e];
        fee += (__int128)r.flow[e] * ch[e].fee_bps;
    }
    if(fee != r.fee){ why = "fee != sum of flow * fee_bps"; return false; }
    for(int v = 0; v < n; v++){
        __int128 want = (src == dst) ? 0 : (v == src ? amt : (v == dst ? -(__int128)amt : 0));
        if(net[v] != want){ why = "conservation broken at node " + to_string(v); return false; }
    }
    return true;
}

// Optimality certificate: a legal flow of fixed value is min-cost iff its residual graph has no negative cycle.
static bool min_cost_certificate(int n, const vector<Channel>& ch, const vector<Amount>& cap, const Route& r){
    struct RE { int u, v; long long c; };
    vector<RE> res;
    for(size_t e = 0; e < ch.size(); e++){
        if(r.flow[e] < cap[e]) res.push_back({ch[e].from, ch[e].to, ch[e].fee_bps});
        if(r.flow[e] > 0)      res.push_back({ch[e].to, ch[e].from, -ch[e].fee_bps});
    }
    vector<long long> d(n, 0);
    for(int round = 0; round <= n; round++){
        bool changed = false;
        for(const RE& x : res) if(d[x.u] + x.c < d[x.v]){ d[x.v] = d[x.u] + x.c; changed = true; }
        if(!changed) return true;
    }
    return false;
}

// ---- independent oracles (share no code with the router)

// cheapest integer flow of exactly amt, by enumerating every flow value on every channel. {feasible, min fee}
static pair<bool, Amount> brute_multi(int n, const vector<Channel>& ch, const vector<Amount>& cap, int src, int dst, Amount amt){
    int k = (int)ch.size();
    vector<Amount> f(k, 0), lim(k);
    for(int e = 0; e < k; e++) lim[e] = min(cap[e], amt);
    bool found = false;
    Amount best = 0;
    while(true){
        vector<Amount> net(n, 0);
        Amount fee = 0;
        for(int e = 0; e < k; e++){
            net[ch[e].from] += f[e];
            net[ch[e].to]   -= f[e];
            fee += f[e] * ch[e].fee_bps;
        }
        bool ok = true;
        for(int v = 0; v < n && ok; v++){
            Amount want = v == src ? amt : (v == dst ? -amt : 0);
            if(net[v] != want) ok = false;
        }
        if(ok && (!found || fee < best)){ found = true; best = fee; }
        int i = 0;
        while(i < k && f[i] == lim[i]){ f[i] = 0; i++; }
        if(i == k) break;
        f[i]++;
    }
    return {found, best};
}

// cheapest simple path whose channels all have cap >= amt, by DFS over every simple path. {feasible, min fee}
static pair<bool, Amount> brute_single(int n, const vector<Channel>& ch, const vector<Amount>& cap, int src, int dst, Amount amt){
    bool found = false;
    Amount best = 0;
    vector<char> on(n, 0);
    function<void(int, Amount)> go = [&](int u, Amount bps){
        if(u == dst){
            if(!found || amt * bps < best){ found = true; best = amt * bps; }
            return;
        }
        on[u] = 1;
        for(size_t e = 0; e < ch.size(); e++){
            if(ch[e].from == u && !on[ch[e].to] && cap[e] >= amt) go(ch[e].to, bps + ch[e].fee_bps);
        }
        on[u] = 0;
    };
    go(src, 0);
    return {found, best};
}

static void expect_route(const char* label, const Route& r, bool ok, Amount fee, const vector<Amount>* flow = nullptr){
    CHECK(r.ok == ok);
    CHECK(r.fee == fee);
    if(r.ok != ok || r.fee != fee) cerr << "  [" << label << "] got ok=" << r.ok << " fee=" << r.fee << ", expected ok=" << ok << " fee=" << fee << "\n";
    if(flow){
        CHECK(r.flow == *flow);
        if(r.flow != *flow) cerr << "  [" << label << "] flows differ from the unique optimum\n";
    }
}

// ------------------------------------------------------------------ A. hand cases
static void test_hand(){
    {   // a line: fee adds up along the path; remaining drops
        Router R(3, chans({{0,1,100,10},{1,2,100,20}}));
        CHECK(R.num_nodes() == 3);
        vector<Amount> f = {50, 50};
        expect_route("line", R.route(0, 2, 50), true, 50 * 30, &f);
        CHECK(R.remaining() == vector<Amount>({50, 50}));
        CHECK(R.route(0, 2, 50).amount == 50);
        CHECK(R.remaining() == vector<Amount>({0, 0}));
    }
    {   // split: the cheap path takes what it can (60), the rest (40) goes the expensive way
        auto c = chans({{0,1,60,5},{1,3,60,5},{0,2,100,20},{2,3,100,20}});
        Router A(4, c), B(4, c);
        vector<Amount> f = {60, 60, 40, 40};
        expect_route("split", A.route(0, 3, 100), true, 60 * 10 + 40 * 40, &f);
        vector<Amount> g = {0, 0, 100, 100};
        expect_route("split, single path", B.route_single_path(0, 3, 100), true, 100 * 40, &g);
    }
    {   // a longer path can be cheaper than the direct channel
        Router R(4, chans({{0,3,100,50},{0,1,100,10},{1,2,100,10},{2,3,100,10}}));
        vector<Amount> f = {0, 10, 10, 10};
        expect_route("long but cheap", R.route(0, 3, 10), true, 10 * 30, &f);
    }
    {   // not enough capacity: all or nothing, nothing changes
        Router R(3, chans({{0,1,50,1},{0,2,30,1},{1,2,50,1}}));
        CHECK(R.max_routable(0, 2) == 80);
        Route r = R.route(0, 2, 100);
        expect_route("too big", r, false, 0);
        CHECK(r.amount == 0 && r.flow == vector<Amount>({0, 0, 0}));
        CHECK(R.remaining() == vector<Amount>({50, 30, 50}));
        CHECK(!R.route_single_path(0, 2, 100).ok);
        CHECK(R.remaining() == vector<Amount>({50, 30, 50}));
        CHECK(R.max_routable(0, 2) == 80);                  // max_routable changes nothing
        CHECK(R.route(0, 2, 80).ok);
        CHECK(R.max_routable(0, 2) == 0);
    }
    {   // multi-path succeeds where no single path is wide enough
        auto c = chans({{0,1,50,1},{1,3,50,1},{0,2,50,1},{2,3,50,1}});
        Router A(4, c), B(4, c);
        CHECK(A.route(0, 3, 80).ok);
        CHECK(!B.route_single_path(0, 3, 80).ok);
        CHECK(B.route_single_path(0, 3, 50).ok);
    }
    {   // commits accumulate across requests
        Router R(2, chans({{0,1,100,7}}));
        CHECK(R.route(0, 1, 60).ok);
        CHECK(!R.route(0, 1, 60).ok);
        CHECK(R.route(0, 1, 40).ok);
        CHECK(R.remaining() == vector<Amount>({0}));
        CHECK(!R.route(0, 1, 1).ok);
    }
    {   // channels are one-way
        Router R(2, chans({{0,1,100,1}}));
        CHECK(!R.route(1, 0, 1).ok);
        CHECK(R.max_routable(1, 0) == 0);
    }
    {   // parallel channels between the same banks: cheapest first, then the next
        Router R(2, chans({{0,1,30,2},{0,1,30,1}}));
        vector<Amount> f = {20, 30};
        expect_route("parallel", R.route(0, 1, 50), true, 30 * 1 + 20 * 2, &f);
    }
    {   // free channels, trivial requests
        Router R(3, chans({{0,1,10,0},{1,2,10,0}}));
        expect_route("free", R.route(0, 2, 10), true, 0);
        Route same = R.route(1, 1, 999);
        CHECK(same.ok && same.amount == 999 && same.fee == 0 && same.flow == vector<Amount>({0, 0}));
        Route zero = R.route(0, 2, 0);
        CHECK(zero.ok && zero.amount == 0 && zero.fee == 0);
        Route zs = R.route_single_path(2, 0, 0);
        CHECK(zs.ok && zs.fee == 0);
        CHECK(R.max_routable(2, 2) == numeric_limits<Amount>::max());
        CHECK(R.remaining() == vector<Amount>({0, 0}));
    }
    {   // a node with no channels at all
        Router R(1, {});
        CHECK(R.route(0, 0, 5).ok);
        CHECK(R.remaining().empty());
    }
    {   // bad input
        CHECK(throws<invalid_argument>([]{ Router R(0, {}); }));
        CHECK(throws<invalid_argument>([]{ Router R(2, chans({{0,2,1,1}})); }));
        CHECK(throws<invalid_argument>([]{ Router R(2, chans({{-1,1,1,1}})); }));
        CHECK(throws<invalid_argument>([]{ Router R(2, chans({{1,1,1,1}})); }));
        CHECK(throws<invalid_argument>([]{ Router R(2, chans({{0,1,-1,1}})); }));
        CHECK(throws<invalid_argument>([]{ Router R(2, chans({{0,1,1,-1}})); }));
        CHECK(throws<invalid_argument>([]{ Router R(2, chans({{0,1,1,10001}})); }));
        CHECK(!throws<exception>([]{ Router R(2, chans({{0,1,0,10000}})); }));
        Router R(2, chans({{0,1,5,1}}));
        CHECK(throws<invalid_argument>([&]{ R.route(0, 2, 1); }));
        CHECK(throws<invalid_argument>([&]{ R.route(-1, 1, 1); }));
        CHECK(throws<invalid_argument>([&]{ R.route(0, 1, -1); }));
        CHECK(throws<invalid_argument>([&]{ R.route_single_path(0, 1, -1); }));
        CHECK(throws<invalid_argument>([&]{ R.route_single_path(2, 1, 1); }));
        CHECK(throws<invalid_argument>([&]{ R.max_routable(0, 5); }));
        CHECK(R.remaining() == vector<Amount>({5}));
    }
}

// ------------------------------------------------------------------ B. against exhaustive oracles (tiny graphs)
static void test_vs_brute(bool fast){
    int seeds = fast ? 1500 : 5000;
    int bad = 0, multi_only = 0, cheaper = 0;
    for(int seed = 0; seed < seeds; seed++){
        Rng rng(21000 + seed);
        int n = (int)rng.uniform(2, 4);
        vector<Channel> ch = random_channels(rng, n, (int)rng.uniform(0, 6), 3, 20);
        int src = (int)rng.uniform(0, n - 1), dst = (int)rng.uniform(0, n - 1);
        Amount amt = rng.uniform(1, 4);

        vector<Amount> cap;
        for(const Channel& c : ch) cap.push_back(c.cap);
        Router A(n, ch), B(n, ch);
        Route rm = A.route(src, dst, amt);
        Route rs = B.route_single_path(src, dst, amt);

        string why;
        bool legal = route_legal(n, ch, cap, src, dst, amt, rm, why) && route_legal(n, ch, cap, src, dst, amt, rs, why);
        auto [mok, mfee] = src == dst ? pair<bool, Amount>{true, 0} : brute_multi(n, ch, cap, src, dst, amt);
        auto [sok, sfee] = src == dst ? pair<bool, Amount>{true, 0} : brute_single(n, ch, cap, src, dst, amt);
        bool same = legal && rm.ok == mok && (!mok || rm.fee == mfee) && rs.ok == sok && (!sok || rs.fee == sfee);
        if(!same && bad++ < 5){
            cerr << "  seed " << seed << ": multi ok=" << rm.ok << " fee=" << rm.fee << " (brute " << mok << ", " << mfee << "), single ok="
                 << rs.ok << " fee=" << rs.fee << " (brute " << sok << ", " << sfee << ")" << (legal ? "" : ", " + why) << "\n";
        }
        if(mok && !sok) multi_only++;
        if(mok && sok && mfee < sfee) cheaper++;
    }
    CHECK(bad == 0);
    CHECK(multi_only > 0 && cheaper > 0);
    cout << "router on " << seeds << " tiny graphs: both strategies match exhaustive search; multi-path routed "
         << multi_only << " requests single-path could not, and was cheaper on " << cheaper << "\n";
}

// ------------------------------------------------------------------ C. sequences of requests, medium graphs
static void test_sequences(bool fast){
    int seeds = fast ? 150 : 500;
    int bad = 0;
    long long reqs = 0, single_but_not_multi = 0;
    for(int seed = 0; seed < seeds; seed++){
        Rng rng(22000 + seed);
        int n = (int)rng.uniform(2, 25);
        vector<Channel> ch = random_channels(rng, n, (int)rng.uniform(0, 80), rng.uniform(0, 1) ? 50 : 1000000000000LL, 300);
        Router A(n, ch);
        vector<Amount> model;                               // our own copy of the remaining capacities
        for(const Channel& c : ch) model.push_back(c.cap);

        for(int q = 0; q < 12; q++){
            int src = (int)rng.uniform(0, n - 1), dst = (int)rng.uniform(0, n - 1);
            Amount mr = A.max_routable(src, dst);
            Amount amt = (src == dst || mr == 0) ? rng.uniform(0, 10) : rng.uniform(1, mr + mr / 4 + 1);

            Router B = A;                                   // same state, single path
            Route rs = B.route_single_path(src, dst, amt);
            vector<Amount> before = A.remaining();
            Route rm = A.route(src, dst, amt);
            reqs++;

            string why;
            bool ok = route_legal(n, ch, before, src, dst, amt, rm, why);
            if(ok && rm.ok && src != dst && !min_cost_certificate(n, ch, before, rm)){ ok = false; why = "not min-cost: residual graph has a negative cycle"; }
            if(ok && rm.ok != (src == dst || mr >= amt)){ ok = false; why = "ok != (max_routable >= amt)"; }
            if(ok && rs.ok && !rm.ok){ ok = false; why = "single path succeeded but multi-path failed"; }
            if(ok && rs.ok && rm.fee > rs.fee){ ok = false; why = "multi-path fee above single-path fee"; }
            if(ok && !route_legal(n, ch, before, src, dst, amt, rs, why)) ok = false;

            if(rm.ok) for(size_t e = 0; e < ch.size(); e++) model[e] -= rm.flow[e];
            if(ok && A.remaining() != model){ ok = false; why = "remaining() does not match the committed flows"; }
            if(rs.ok && !rm.ok) single_but_not_multi++;

            if(!ok && bad++ < 5) cerr << "  seed " << seed << " request " << q << ": " << why << "\n";
        }
    }
    CHECK(bad == 0);
    CHECK(single_but_not_multi == 0);
    cout << "router on " << reqs << " requests over " << seeds << " medium graphs: every route legal, min-cost (certificate), "
         << "consistent with max_routable and with remaining()\n";
}

// ------------------------------------------------------------------ D. big numbers
static void test_big(){
    const Amount T = 1000000000000LL;
    vector<Channel> ch;
    for(int i = 0; i < 10; i++){
        Channel c;
        c.from = i;
        c.to = i + 1;
        c.cap = T;
        c.fee_bps = 10000;
        ch.push_back(c);
    }
    Router R(11, ch);
    expect_route("1e12 over 10 hops at 100%", R.route(0, 10, T), true, T * 10 * 10000);   // 1e17
    CHECK(R.max_routable(0, 10) == 0);

    // 100 parallel paths of 1e12: one request of 1e14
    vector<Channel> par;
    for(int i = 0; i < 100; i++){
        Channel a, b;
        a.from = 0; a.to = 2 + i; a.cap = T; a.fee_bps = i;
        b.from = 2 + i; b.to = 1; b.cap = T; b.fee_bps = 0;
        par.push_back(a);
        par.push_back(b);
    }
    Router P(102, par);
    CHECK(P.max_routable(0, 1) == 100 * T);
    expect_route("100 x 1e12", P.route(0, 1, 100 * T), true, T * (99 * 100 / 2));
}

// ------------------------------------------------------------------ E. scale: a payment network with hubs
static void test_scale(bool fast){
    using clk = chrono::steady_clock;
    int n = fast ? 300 : 1000, hubs = n / 50, reqs = fast ? 300 : 1000;
    Rng rng(777);
    vector<Channel> ch;
    auto link = [&](int a, int b, Amount cap, int fee){
        Channel c;
        c.from = a; c.to = b; c.cap = cap; c.fee_bps = fee;
        ch.push_back(c);
        c.from = b; c.to = a;
        ch.push_back(c);
    };
    for(int h = 0; h < hubs; h++) for(int g = h + 1; g < hubs; g++) link(h, g, rng.uniform(5000000, 20000000), (int)rng.uniform(1, 10));
    for(int v = hubs; v < n; v++){
        link(v, (int)rng.uniform(0, hubs - 1), rng.uniform(200000, 2000000), (int)rng.uniform(5, 40));
        for(int k = 0; k < 2; k++){
            int w = (int)rng.uniform(hubs, n - 1);
            if(w != v) link(v, w, rng.uniform(10000, 300000), (int)rng.uniform(1, 15));
        }
    }

    Router multi(n, ch), single(n, ch);
    long long ok_m = 0, ok_s = 0;
    __int128 fee_m = 0, fee_s = 0, vol_m = 0, vol_s = 0;
    double t_m = 0, t_s = 0;
    for(int q = 0; q < reqs; q++){
        int src = (int)rng.uniform(hubs, n - 1), dst = (int)rng.uniform(hubs, n - 1);
        Amount amt = rng.uniform(10000, 400000);
        vector<Amount> before = multi.remaining();
        auto t0 = clk::now();
        Route a = multi.route(src, dst, amt);
        auto t1 = clk::now();
        Route b = single.route_single_path(src, dst, amt);
        auto t2 = clk::now();
        t_m += chrono::duration<double, milli>(t1 - t0).count();
        t_s += chrono::duration<double, milli>(t2 - t1).count();
        string why;
        CHECK(route_legal(n, ch, before, src, dst, amt, a, why));
        if(a.ok){ ok_m++; fee_m += a.fee; vol_m += amt; }
        if(b.ok){ ok_s++; fee_s += b.fee; vol_s += amt; }
    }
    CHECK(ok_m > 0);
    auto bps = [](__int128 fee, __int128 vol){ return vol ? (double)fee / (double)vol : 0.0; };
    cout << fixed << setprecision(2);
    cout << "routing on " << n << " banks, " << ch.size() << " channels, " << reqs << " requests (each strategy on its own network):\n"
         << "  multi-path : " << ok_m << " routed, volume " << (long long)vol_m << ", avg fee " << bps(fee_m, vol_m) << " bps, "
         << t_m / reqs << " ms/request\n"
         << "  single-path: " << ok_s << " routed, volume " << (long long)vol_s << ", avg fee " << bps(fee_s, vol_s) << " bps, "
         << t_s / reqs << " ms/request\n";
    cout.unsetf(ios::fixed);
}

int main(int argc, char** argv){
    bool fast = (argc > 1 && string(argv[1]) == "--fast");

    test_hand();
    test_vs_brute(fast);
    test_sequences(fast);
    test_big();
    test_scale(fast);

    if(failures){
        cerr << failures << " check(s) failed\n";
        return 1;
    }
    cout << "all tests passed\n";
    return 0;
}