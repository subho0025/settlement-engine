#pragma once
#include "core/types.hpp"

class Dinic {
public:
    Dinic(int n);
    int add_edge(int u, int v, Amount cap);
    Amount max_flow(int s, int t);
    Amount flow_on(int id) const;
    vector<bool> min_cut_side(int s) const;
    int num_nodes() const;

private:
    struct Edge { int to; Amount cap; Amount orig; };
    int n_;
    vector<Edge> e_;
    vector<vector<int>> g_;
    vector<int> level_, it_;
    bool   bfs(int s, int t);
    Amount dfs(int u, int t, Amount pushed);
};