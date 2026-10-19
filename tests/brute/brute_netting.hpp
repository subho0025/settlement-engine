#pragma once
#include <bits/stdc++.h>
#include "core/types.hpp"

using namespace std;

// Brute-force method for exact netting: the maximum number of disjoint zero-sum groups that the nonzero balances can be split into

inline void brute_rec(const vector<Amount>& b, int i, vector<Amount>& sums, int& best){
    if(i == (int)b.size()){
        for(Amount s : sums) if(s != 0) return;
        best = max(best, (int)sums.size());
        return;
    }

    for(int j = 0; j < (int)sums.size(); j++){
        sums[j] += b[i];
        brute_rec(b, i + 1, sums, best);
        sums[j] -= b[i];
    }

    sums.push_back(b[i]);
    brute_rec(b, i + 1, sums, best);
    sums.pop_back();
}

inline int brute_max_groups(const vector<Amount>& b){
    if(b.empty()) return 0;
    vector<Amount> sums;
    int best = 0;
    brute_rec(b, 0, sums, best);
    return best;
}