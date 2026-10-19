#pragma once
#include <bits/stdc++.h>
#include "gridlock/gridlock.hpp"
#include "core/instance.hpp"
#include "core/types.hpp"

using namespace std;

class BranchBoundSolver : public GridlockSolver {
public:
    explicit BranchBoundSolver(long long node_limit = -1);
    string name() const override;
    vector<int> solve(const Instance&) override;
    long long nodes_explored() const;
private:
    long long node_limit_;
    long long nodes_ = 0;
};