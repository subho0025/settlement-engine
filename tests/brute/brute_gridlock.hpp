#pragma once
#include <bits/stdc++.h>
#include "core/types.hpp"
#include "core/instance.hpp"

using namespace std;

// Exhaustive optimum for gridlock resolution: tries every subset of the payments and keeps the one with the largest
// settled value that leaves every bank with  balance + credit_limit + incoming - outgoing >= 0.
//
// Self-contained on purpose: it uses only Instance (n, obs, balance, credit_limit) and Amount. It does NOT include
// gridlock.hpp / validator.hpp / io.hpp and shares no code with the validator or any solver, so it can act as an
// independent oracle.
struct BruteGridlock {
    Amount best_value = 0;
    vector<int> best_set;       // indices ascending (one of the optimal sets)
};

inline BruteGridlock brute_gridlock(const Instance& inst){
    int m = (int)inst.obs.size();
    if(m > 24) throw invalid_argument("brute_gridlock: more than 24 payments");
    if((int)inst.balance.size() != inst.n || (int)inst.credit_limit.size() != inst.n)
        throw invalid_argument("brute_gridlock: instance has no balances / credit limits");

    BruteGridlock best;
    for(uint32_t mask = 0; mask < (1u << m); mask++){
        vector<__int128> pos(inst.n);
        for(int i = 0; i < inst.n; i++) pos[i] = (__int128)inst.balance[i] + inst.credit_limit[i];

        Amount value = 0;
        for(int p = 0; p < m; p++){
            if(mask >> p & 1){
                pos[inst.obs[p].to]   += inst.obs[p].amt;
                pos[inst.obs[p].from] -= inst.obs[p].amt;
                value += inst.obs[p].amt;
            }
        }

        bool ok = true;
        for(int i = 0; i < inst.n && ok; i++) if(pos[i] < 0) ok = false;

        if(ok && value > best.best_value){
            best.best_value = value;
            best.best_set.clear();
            for(int p = 0; p < m; p++) if(mask >> p & 1) best.best_set.push_back(p);
        }
    }
    return best;
}