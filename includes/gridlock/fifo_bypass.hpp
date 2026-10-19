#pragma once
#include <bits/stdc++.h>
#include "core/types.hpp"
#include "core/instance.hpp"
#include "gridlock/gridlock.hpp"

using namespace std;

class FifoBypassSolver: public GridlockSolver{
public:
    string name() const override;
    vector<int> solve(const Instance&) override;
};