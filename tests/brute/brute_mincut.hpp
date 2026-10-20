#pragma once
#include <bits/stdc++.h>

using namespace std;

// Exhaustive minimum s-t cut of a small directed graph. By the max-flow / min-cut theorem its value equals the
// maximum s-t flow, so it is an independent oracle for any max-flow implementation.
//
// Self-contained on purpose: it takes a plain edge list, shares no code with Dinic or any solver, and does no
// augmenting at all. It simply tries every set S with s in S and t not in S and sums the capacity of the edges
// that leave S.
struct BruteEdge {
    int u, v;
    long long cap;
};

inline long long brute_min_cut(int n, int s, int t, const vector<BruteEdge>& edges){
    if(n > 20) throw invalid_argument("brute_min_cut: more than 20 nodes");
    if(s == t) throw invalid_argument("brute_min_cut: s == t");

    __int128 best = -1;
    for(uint32_t mask = 0; mask < (1u << n); mask++){
        if(!(mask >> s & 1) || (mask >> t & 1)) continue;      // s inside, t outside
        __int128 cut = 0;
        for(const BruteEdge& e : edges){
            if((mask >> e.u & 1) && !(mask >> e.v & 1)) cut += e.cap;
        }
        if(best < 0 || cut < best) best = cut;
    }
    return (long long)best;
}