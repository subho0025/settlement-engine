#pragma once
#include <bits/stdc++.h>
#include "gridlock.hpp"

using namespace std;

class DropViolatorsSolver : public GridlockSolver {
public:
    string name() const override;
    vector<int> solve(const Instance&) override;
};