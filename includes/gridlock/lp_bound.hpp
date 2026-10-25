#pragma once
#include "core/instance.hpp"

struct LpBound {
    Amount value = 0;
    Amount total = 0;
    vector<Amount> unsettled;
};

LpBound gridlock_lp_bound(const Instance&);