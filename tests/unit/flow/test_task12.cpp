#include "flow/mcmf.hpp"
#include "core/rng.hpp"
using namespace std;

// Contract (matches mcmf.hpp): MinCostFlow(n); add_edge(u,v,cap,cost) -> id 0,1,2..; min_cost_flow(s,t,limit) -> {flow, cost}
// of THIS call (successive calls continue); flow_on(id). invalid_argument for bad nodes/ids, cap < 0, limit < 0, s == t,
// and a negative-cost cycle reachable from s through residual capacity. Costs may be negative.

static int failures = 0;
#define CHECK(cond) do { if(!(cond)) { cerr << __FILE__ << ":" << __LINE__ << "  CHECK failed: " #cond "\n"; failures++; } } while(0)

struct E { int u, v; Amount cap, cost; };

// ---- test-local references (no code shared with MinCostFlow)
// successive shortest paths with plain Bellman-Ford; assumes no negative cycle
static pair<Amount,Amount> ref_mcmf(int n, const vector<E>& edges, int s, int t, Amount limit){
    struct A { int to; Amount cap, cost; };
    vector<A> a; vector<vector<int>> g(n);
    for(const E& e : edges){
        g[e.u].push_back((int)a.size()); a.push_back({e.v, e.cap, e.cost});
        g[e.v].push_back((int)a.size()); a.push_back({e.u, 0, -e.cost});
    }
    Amount flow = 0, cost = 0;
    while(flow < limit){
        const Amount INF = LLONG_MAX / 4;
        vector<Amount> d(n, INF); vector<int> pe(n, -1);
        d[s] = 0;
        for(int r = 0; r < n; r++){
            bool ch = false;
            for(int u = 0; u < n; u++) if(d[u] < INF) for(int id : g[u]) if(a[id].cap > 0 && d[u] + a[id].cost < d[a[id].to]){
                d[a[id].to] = d[u] + a[id].cost; pe[a[id].to] = id; ch = true;
            }
            if(!ch) break;
        }
        if(d[t] == INF) break;
        Amount push = limit - flow;
        for(int v = t; v != s; v = a[pe[v] ^ 1].to) push = min(push, a[pe[v]].cap);
        for(int v = t; v != s; v = a[pe[v] ^ 1].to){ a[pe[v]].cap -= push; a[pe[v] ^ 1].cap += push; }
        flow += push; cost += push * d[t];
    }
    return {flow, cost};
}

// Hungarian algorithm, minimum-cost perfect matching on an n x n matrix
static Amount hungarian(const vector<vector<Amount>>& c){
    int n = (int)c.size();
    vector<Amount> u(n + 1), v(n + 1); vector<int> p(n + 1), way(n + 1);
    for(int i = 1; i <= n; i++){
        p[0] = i; int j0 = 0;
        vector<Amount> minv(n + 1, LLONG_MAX / 4); vector<char> used(n + 1, 0);
        do{
            used[j0] = 1; int i0 = p[j0], j1 = 0; Amount delta = LLONG_MAX / 4;
            for(int j = 1; j <= n; j++) if(!used[j]){
                Amount cur = c[i0 - 1][j - 1] - u[i0] - v[j];
                if(cur < minv[j]){ minv[j] = cur; way[j] = j0; }
                if(minv[j] < delta){ delta = minv[j]; j1 = j; }
            }
            for(int j = 0; j <= n; j++){ if(used[j]){ u[p[j]] += delta; v[j] -= delta; } else minv[j] -= delta; }
            j0 = j1;
        }while(p[j0] != 0);
        do{ int j1 = way[j0]; p[j0] = p[j1]; j0 = j1; }while(j0);
    }
    Amount total = 0;
    for(int j = 1; j <= n; j++) total += c[p[j] - 1][j - 1];
    return total;
}

// ---- helpers
static vector<int> add_all(MinCostFlow& m, const vector<E>& edges){
    vector<int> ids;
    for(const E& e : edges) ids.push_back(m.add_edge(e.u, e.v, e.cap, e.cost));
    return ids;
}

// Proof of optimality for the flow currently in `m`: legal flow, matching value and cost, and NO negative cycle in the residual graph.
static void check_optimal(const char* label, const MinCostFlow& m, int n, const vector<E>& edges, const vector<int>& ids,
                          int s, int t, Amount flow, Amount cost){
    vector<__int128> net(n, 0); __int128 c = 0; bool legal = true;
    vector<Amount> f(edges.size());
    for(size_t k = 0; k < edges.size(); k++){
        f[k] = m.flow_on(ids[k]);
        if(f[k] < 0 || f[k] > edges[k].cap) legal = false;
        net[edges[k].u] += f[k]; net[edges[k].v] -= f[k]; c += (__int128)f[k] * edges[k].cost;
    }
    bool conserved = true;
    for(int v = 0; v < n; v++) if(v != s && v != t && net[v] != 0) conserved = false;
    // negative-cycle test on the residual graph, all nodes start at distance 0
    vector<__int128> d(n, 0); bool negative = false;
    for(int r = 0; r <= n; r++){
        bool ch = false;
        for(size_t k = 0; k < edges.size(); k++){
            const E& e = edges[k];
            if(f[k] < e.cap && d[e.u] + e.cost < d[e.v]){ d[e.v] = d[e.u] + e.cost; ch = true; }
            if(f[k] > 0 && d[e.v] - e.cost < d[e.u]){ d[e.u] = d[e.v] - e.cost; ch = true; }
        }
        if(!ch) break;
        if(r == n) negative = true;
    }
    bool ok = legal && conserved && net[s] == flow && net[t] == -flow && c == cost && !negative;
    CHECK(ok);
    if(!ok) cerr << "  [" << label << "] legal=" << legal << " conserved=" << conserved << " value=" << (net[s] == flow)
                 << " cost=" << (c == cost) << " negative_cycle=" << negative << "\n";
}

static void run_case(const char* label, int n, const vector<E>& edges, int s, int t, Amount limit, Amount want_flow, Amount want_cost){
    MinCostFlow m(n); vector<int> ids = add_all(m, edges);
    auto [fl, co] = m.min_cost_flow(s, t, limit);
    CHECK(fl == want_flow); CHECK(co == want_cost);
    if(fl != want_flow || co != want_cost) cerr << "  [" << label << "] expected (" << want_flow << "," << want_cost << ") got (" << fl << "," << co << ")\n";
    auto r = ref_mcmf(n, edges, s, t, limit);
    CHECK(r.first == want_flow); CHECK(r.second == want_cost);
    check_optimal(label, m, n, edges, ids, s, t, fl, co);
}

template <class F> static bool throws_invalid(F f){
    try{ f(); }catch(const invalid_argument&){ return true; }catch(...){ return false; }
    return false;
}

static void test_hand_cases(){
    run_case("single edge", 2, {{0,1,5,3}}, 0, 1, LLONG_MAX, 5, 15);
    run_case("limit smaller than capacity", 2, {{0,1,5,3}}, 0, 1, 2, 2, 6);
    run_case("cheap path first", 4, {{0,1,2,1},{1,3,2,1},{0,2,4,2},{2,3,4,2}}, 0, 3, LLONG_MAX, 6, 2*2 + 4*4);
    run_case("limit stops on the cheap path", 4, {{0,1,2,1},{1,3,2,1},{0,2,4,2},{2,3,4,2}}, 0, 3, 2, 2, 4);
    run_case("limit reaches the dear path", 4, {{0,1,2,1},{1,3,2,1},{0,2,4,2},{2,3,4,2}}, 0, 3, 3, 3, 4 + 4);
    run_case("disconnected", 3, {{0,1,5,1}}, 0, 2, LLONG_MAX, 0, 0);
    run_case("zero limit", 2, {{0,1,5,1}}, 0, 1, 0, 0, 0);
    run_case("zero costs", 3, {{0,1,4,0},{1,2,3,0}}, 0, 2, LLONG_MAX, 3, 0);
    run_case("a cheaper route needs a reverse arc", 4, {{0,1,1,1},{0,2,1,5},{1,2,1,1},{1,3,1,5},{2,3,1,1}}, 0, 3, LLONG_MAX, 2, 1+5+5+1);
    run_case("negative costs", 4, {{0,1,3,-2},{1,2,3,4},{0,2,2,1},{2,3,5,-1}}, 0, 3, LLONG_MAX, 5, 5*0 + 3*(-2+4-1) + 2*(1-1));
    run_case("parallel edges", 2, {{0,1,2,7},{0,1,2,3},{0,1,2,5}}, 0, 1, 5, 5, 2*3 + 2*5 + 1*7);
}

static void test_random(){
    for(int seed = 0; seed < 400; seed++){
        Rng rng(14000 + seed);
        int n = (int)rng.uniform(2, 8), m = (int)rng.uniform(0, 20);
        bool dag = seed % 2 == 1;                                       // odd seeds: acyclic with negative costs
        vector<E> edges;
        for(int i = 0; i < m; i++){
            int u = (int)rng.uniform(0, n - 1), v = (int)rng.uniform(0, n - 1);
            if(dag){ if(u == v) continue; if(u > v) swap(u, v); }
            edges.push_back({u, v, rng.uniform(0, 8), dag ? rng.uniform(-5, 9) : rng.uniform(0, 9)});
        }
        int s = 0, t = n - 1;
        Amount limit = rng.uniform(0, 3) == 0 ? LLONG_MAX : rng.uniform(0, 12);
        MinCostFlow mc(n); vector<int> ids = add_all(mc, edges);
        auto [fl, co] = mc.min_cost_flow(s, t, limit);
        auto r = ref_mcmf(n, edges, s, t, limit);
        CHECK(fl == r.first); CHECK(co == r.second);
        if(fl != r.first || co != r.second) cerr << "  seed " << seed << ": (" << fl << "," << co << ") vs reference (" << r.first << "," << r.second << ")\n";
        check_optimal("random", mc, n, edges, ids, s, t, fl, co);

        // two calls with split limits cost the same as one call with the total
        if(fl >= 2){
            MinCostFlow m2(n); vector<int> ids2 = add_all(m2, edges);
            auto a = m2.min_cost_flow(s, t, fl / 2);
            auto b = m2.min_cost_flow(s, t, fl - fl / 2);
            CHECK(a.first + b.first == fl); CHECK(a.second + b.second == co);
            check_optimal("two calls", m2, n, edges, ids2, s, t, fl, co);
        }
    }
}

static void test_errors(){
    CHECK(throws_invalid([]{ MinCostFlow m(0); }));
    MinCostFlow m(3);
    CHECK(throws_invalid([&]{ m.add_edge(-1, 0, 1, 1); }));
    CHECK(throws_invalid([&]{ m.add_edge(0, 3, 1, 1); }));
    CHECK(throws_invalid([&]{ m.add_edge(0, 1, -1, 1); }));
    CHECK(!throws_invalid([&]{ m.add_edge(0, 1, 0, -4); }));
    CHECK(throws_invalid([&]{ m.min_cost_flow(1, 1); }));
    CHECK(throws_invalid([&]{ m.min_cost_flow(0, 5); }));
    CHECK(throws_invalid([&]{ m.min_cost_flow(0, 2, -1); }));
    CHECK(throws_invalid([&]{ m.flow_on(7); }));
    {   // 1 -> 2 -> 1 costs -2 per round trip and is reachable from the source: the minimum cost is unbounded
        MinCostFlow c(4);
        c.add_edge(0, 1, 5, 0); c.add_edge(1, 2, 1, -3); c.add_edge(2, 1, 1, 1); c.add_edge(1, 3, 5, 0);
        CHECK(throws_invalid([&]{ c.min_cost_flow(0, 3); }));
    }
    {   // the same negative cycle NOT reachable from the source is harmless
        MinCostFlow c(5);
        c.add_edge(0, 1, 5, 2); c.add_edge(1, 2, 5, 2); c.add_edge(3, 4, 1, -3); c.add_edge(4, 3, 1, 1);
        auto r = c.min_cost_flow(0, 2);
        CHECK(r.first == 5); CHECK(r.second == 20);
    }
}

static void test_scale(bool fast){
    using clk = chrono::steady_clock;
    auto ms = [](clk::time_point a, clk::time_point b){ return chrono::duration_cast<chrono::milliseconds>(b - a).count(); };
    {   // assignment problem against the Hungarian algorithm
        int n = fast ? 60 : 120; Rng rng(41);
        vector<vector<Amount>> c(n, vector<Amount>(n));
        for(auto& row : c) for(Amount& x : row) x = rng.uniform(1, 1000);
        int s = 2 * n, t = 2 * n + 1; vector<E> edges;
        for(int i = 0; i < n; i++){ edges.push_back({s, i, 1, 0}); edges.push_back({n + i, t, 1, 0}); for(int j = 0; j < n; j++) edges.push_back({i, n + j, 1, c[i][j]}); }
        MinCostFlow m(2 * n + 2); vector<int> ids = add_all(m, edges);
        auto t0 = clk::now(); auto [fl, co] = m.min_cost_flow(s, t); auto t1 = clk::now();
        CHECK(fl == n); CHECK(co == hungarian(c));
        check_optimal("assignment", m, 2 * n + 2, edges, ids, s, t, fl, co);
        cout << "mcmf assignment " << n << "x" << n << ": cost " << co << " (= Hungarian), " << ms(t0, t1) << " ms\n";
    }
    {   // layered network with random costs, certified by the no-negative-cycle test
        int L = fast ? 12 : 20, W = fast ? 20 : 30; Rng rng(42); int s = 0, t = 1, n = 2 + L * W; vector<E> edges;
        auto id = [&](int l, int j){ return 2 + l * W + j; };
        for(int j = 0; j < W; j++){ edges.push_back({s, id(0, j), 100, 0}); edges.push_back({id(L - 1, j), t, 100, 0}); }
        for(int l = 0; l + 1 < L; l++) for(int j = 0; j < W; j++) for(int k = 0; k < 4; k++)
            edges.push_back({id(l, j), id(l + 1, (int)rng.uniform(0, W - 1)), rng.uniform(1, 20), rng.uniform(1, 50)});
        MinCostFlow m(n); vector<int> ids = add_all(m, edges);
        auto t0 = clk::now(); auto [fl, co] = m.min_cost_flow(s, t); auto t1 = clk::now();
        CHECK(fl > 0);
        check_optimal("layered", m, n, edges, ids, s, t, fl, co);
        cout << "mcmf layered (" << n << " nodes, " << edges.size() << " edges): flow " << fl << ", cost " << co << ", " << ms(t0, t1) << " ms\n";
    }
}

int main(int argc, char** argv){
    bool fast = (argc > 1 && string(argv[1]) == "--fast");
    test_hand_cases(); test_random(); test_errors(); test_scale(fast);
    if(failures){ cerr << failures << " check(s) failed\n"; return 1; }
    cout << "all tests passed\n"; return 0;
}