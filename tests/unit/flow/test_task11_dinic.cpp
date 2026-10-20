#include "flow/dinic.hpp"
#include "brute/brute_mincut.hpp"
#include "core/rng.hpp"

using namespace std;

// Contract used by this file:
//
//   class Dinic                                     (includes/flow/dinic.hpp, src/flow/dinic.cpp)
//       explicit Dinic(int n)                       nodes are 0..n-1
//       int    add_edge(int u, int v, Amount cap)   returns edge ids 0, 1, 2, ... in call order (ids of the edges YOU add;
//                                                   the hidden reverse edges get no id). u == v is allowed and harmless.
//                                                   Throws std::invalid_argument if u/v is out of range or cap < 0.
//       Amount max_flow(int s, int t)               maximum s-t flow. Calling it again with the same s, t returns 0
//                                                   (the residual graph has no augmenting path left).
//                                                   Throws std::invalid_argument if s == t or s/t is out of range.
//       Amount flow_on(int id) const                flow on edge `id` (0 before max_flow). Throws std::out_of_range on a bad id.
//       vector<bool> min_cut_side(int s) const      after max_flow: true for every node reachable from s in the residual graph
//       int    num_nodes() const

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
        return false;      // wrong exception type
    }
    return false;
}

// Checks a Dinic result: capacities, conservation, and that the residual cut has exactly the flow's capacity.
static bool flow_ok(const Dinic& g, int n, int s, int t, const vector<BruteEdge>& edges, const vector<int>& ids,
                    Amount flow, string& why){
    vector<__int128> net(n, 0);       // outflow - inflow
    for(size_t k = 0; k < edges.size(); k++){
        Amount f = g.flow_on(ids[k]);
        if(f < 0 || f > edges[k].cap){ why = "edge " + to_string(k) + " carries flow outside [0, cap]"; return false; }
        net[edges[k].u] += f;
        net[edges[k].v] -= f;
    }
    for(int v = 0; v < n; v++){
        if(v == s || v == t) continue;
        if(net[v] != 0){ why = "conservation broken at node " + to_string(v); return false; }
    }
    if(net[s] != flow || net[t] != -flow){ why = "flow leaving s / entering t != max_flow()"; return false; }

    vector<bool> side = g.min_cut_side(s);
    if((int)side.size() != n || !side[s] || side[t]){ why = "min_cut_side() must contain s and not t"; return false; }
    __int128 cut = 0;
    for(const BruteEdge& e : edges) if(side[e.u] && !side[e.v]) cut += e.cap;
    if(cut != flow){ why = "residual cut capacity != flow"; return false; }
    return true;
}

static Amount run_dinic(int n, int s, int t, const vector<BruteEdge>& edges, bool check, const char* label){
    Dinic g(n);
    vector<int> ids;
    for(const BruteEdge& e : edges) ids.push_back(g.add_edge(e.u, e.v, e.cap));
    Amount f = g.max_flow(s, t);
    if(check){
        string why;
        bool ok = flow_ok(g, n, s, t, edges, ids, f, why);
        CHECK(ok);
        if(!ok) cerr << "  [" << label << "] " << why << "\n";
    }
    return f;
}

// ------------------------------------------------------------------ A. Dinic: hand cases
static void test_dinic_hand(){
    {   // CLRS 26.1: answer 23
        vector<BruteEdge> E = {{0,1,16},{0,2,13},{1,2,10},{2,1,4},{1,3,12},{3,2,9},{2,4,14},{4,3,7},{3,5,20},{4,5,4}};
        CHECK(run_dinic(6, 0, 5, E, true, "clrs") == 23);
    }
    {   // t unreachable
        vector<BruteEdge> E = {{0,1,5},{2,3,5}};
        CHECK(run_dinic(4, 0, 3, E, true, "no path") == 0);
    }
    {   // parallel edges add up; the bottleneck is downstream
        vector<BruteEdge> E = {{0,1,5},{0,1,7},{1,2,100}};
        CHECK(run_dinic(3, 0, 2, E, true, "parallel") == 12);
    }
    {   // a plain DFS that walks 0-1-2-3 first needs the residual back edge 2->1 to reach 2; Dinic avoids 1->2 via levels
        vector<BruteEdge> E = {{0,1,1},{0,2,1},{1,2,1},{1,3,1},{2,3,1}};
        CHECK(run_dinic(4, 0, 3, E, true, "diamond") == 2);
    }
    {   // zero-capacity edges and a self-loop are harmless
        vector<BruteEdge> E = {{0,1,0},{0,2,4},{2,2,9},{2,1,3},{1,3,8}};
        CHECK(run_dinic(4, 0, 3, E, true, "zero cap + self-loop") == 3);
    }
    {   // amounts close to the int64 limit: two parallel 4e18 edges -> 8e18 (no overflow anywhere)
        vector<BruteEdge> E = {{0,1,4000000000000000000LL},{0,1,4000000000000000000LL},{1,2,9000000000000000000LL}};
        CHECK(run_dinic(3, 0, 2, E, true, "huge") == 8000000000000000000LL);
    }
    {   // edge ids, flow before solving, and a second max_flow call
        Dinic g(3);
        CHECK(g.num_nodes() == 3);
        CHECK(g.add_edge(0, 1, 5) == 0);
        CHECK(g.add_edge(1, 2, 3) == 1);
        CHECK(g.add_edge(0, 2, 2) == 2);
        CHECK(g.flow_on(0) == 0 && g.flow_on(1) == 0 && g.flow_on(2) == 0);
        CHECK(g.max_flow(0, 2) == 5);
        CHECK(g.flow_on(0) == 3 && g.flow_on(1) == 3 && g.flow_on(2) == 2);
        CHECK(g.max_flow(0, 2) == 0);      // nothing left to push
        CHECK(g.flow_on(0) == 3);          // and nothing changed
    }
    {   // bad input
        CHECK(throws<invalid_argument>([]{ Dinic g(2); g.add_edge(0, 2, 1); }));
        CHECK(throws<invalid_argument>([]{ Dinic g(2); g.add_edge(-1, 1, 1); }));
        CHECK(throws<invalid_argument>([]{ Dinic g(2); g.add_edge(0, 1, -1); }));
        CHECK(throws<invalid_argument>([]{ Dinic g(2); g.max_flow(1, 1); }));
        CHECK(throws<invalid_argument>([]{ Dinic g(2); g.max_flow(0, 2); }));
        CHECK(throws<out_of_range>([]{ Dinic g(2); g.add_edge(0, 1, 1); g.flow_on(1); }));
    }
}

// ------------------------------------------------------------------ B. Dinic: against exhaustive min cut
static void test_dinic_vs_brute(bool fast){
    int seeds = fast ? 500 : 2000;
    int bad = 0;
    for(int seed = 0; seed < seeds; seed++){
        Rng rng(11000 + seed);
        int n = (int)rng.uniform(2, 9);
        int m = (int)rng.uniform(0, 3 * n);
        int s = (int)rng.uniform(0, n - 1);
        int t = (int)rng.uniform(0, n - 2);
        if(t >= s) t++;
        long long maxc = rng.uniform(0, 1) ? 20 : 1000000000000LL;
        vector<BruteEdge> E;
        for(int k = 0; k < m; k++) E.push_back({(int)rng.uniform(0, n - 1), (int)rng.uniform(0, n - 1), rng.uniform(0, maxc)});

        Amount got = run_dinic(n, s, t, E, true, "random");
        Amount want = brute_min_cut(n, s, t, E);
        if(got != want){
            if(bad++ < 5) cerr << "  seed " << seed << ": max_flow " << got << ", min cut " << want << "\n";
        }
    }
    CHECK(bad == 0);
    cout << "dinic on " << seeds << " random graphs: max flow == exhaustive min cut on all of them\n";
}

// ------------------------------------------------------------------ C. Dinic: scale
static void test_dinic_scale(bool fast){
    using clk = chrono::steady_clock;
    // layered graph: s -> L layers of W nodes -> t, every node linked to 4 random nodes of the next layer
    int L = fast ? 20 : 50, W = fast ? 200 : 1000;
    int n = L * W + 2, s = n - 2, t = n - 1;
    Rng rng(77);
    vector<BruteEdge> E;
    for(int w = 0; w < W; w++) E.push_back({s, w, rng.uniform(1, 1000000)});
    for(int l = 0; l + 1 < L; l++)
        for(int w = 0; w < W; w++)
            for(int k = 0; k < 4; k++) E.push_back({l * W + w, (l + 1) * W + (int)rng.uniform(0, W - 1), rng.uniform(1, 1000000)});
    for(int w = 0; w < W; w++) E.push_back({(L - 1) * W + w, t, rng.uniform(1, 1000000)});

    auto t0 = clk::now();
    Amount f = run_dinic(n, s, t, E, true, "scale");
    auto ms = chrono::duration_cast<chrono::milliseconds>(clk::now() - t0).count();
    CHECK(f > 0);
    cout << "dinic on a layered graph, " << n << " nodes, " << E.size() << " edges: flow " << f << ", " << ms << " ms\n";
}

int main(int argc, char** argv){
    bool fast = (argc > 1 && string(argv[1]) == "--fast");

    test_dinic_hand();
    test_dinic_vs_brute(fast);
    test_dinic_scale(fast);

    if(failures){
        cerr << failures << " check(s) failed\n";
        return 1;
    }
    cout << "all tests passed\n";
    return 0;
}