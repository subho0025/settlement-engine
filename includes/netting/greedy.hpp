#pragma once
#include "netting/netting_solver.hpp"

using namespace std;

class GreedySolver : public NettingSolver {
public:
    string name() const override;
    Plan solve(const Instance&) override;
};