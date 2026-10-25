#pragma once
#include "core/types.hpp"

using namespace std;

class MinCostFlow {
public:
    MinCostFlow(int n);
    int add_edge(int u, int v, Amount cap, Amount cost);
    pair<Amount, Amount> min_cost_flow(int s, int t, Amount limit = numeric_limits<Amount>::max());
    Amount flow_on(int edge_id) const;
private:
    struct Edge {int from; int to; Amount cap; Amount orig; Amount cost; };
    int n_;
    vector<Edge> e_;
    vector<vector<int>> g_;
    vector<Amount> dist_;
    vector<int> prev_;
    vector<Amount> h_;
    void bellmanFord(int s);
    void dijkstra(int s);
};