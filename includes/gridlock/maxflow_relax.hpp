#pragma once
#include "gridlock/gridlock.hpp"
#include "flow/dinic.hpp"

using namespace std;

class MaxflowRelaxSolver : public GridlockSolver {
public:
    string name() const override;
    vector<int> solve(const Instance&) override;

    Amount flow_value() const;
    Amount relaxed_value() const;
    const vector<Amount>& unsettled() const;

private:
    Amount flow_ = 0;
    Amount relaxed_ = 0;
    vector<Amount> y_;
};