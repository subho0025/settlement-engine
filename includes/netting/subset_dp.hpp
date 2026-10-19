#pragma once
#include <bits/stdc++.h>
#include "core/instance.hpp"
#include "netting/netting_solver.hpp"

using namespace std;

vector<vector<int>> dp_max_groups(const vector<Amount>& b);

class SubsetDpSolver : public NettingSolver {
public:
    string name() const override;
    Plan solve(const Instance&) override;
};