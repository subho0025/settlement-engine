#pragma once
#include <bits/stdc++.h>
#include "core/types.hpp"
#include "netting/netting_solver.hpp"

using namespace std;

vector<vector<int>> match_groups(const vector<Amount>& b);

class MatchSolver : public NettingSolver{
public:
    string name() const override;
    Plan solve(const Instance&) override;
};