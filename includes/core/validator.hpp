#pragma once
#include <bits/stdc++.h>
#include "core/types.hpp"
#include "core/instance.hpp"

using namespace std;

struct ValidationResult {
    bool ok = false;
    string error;
    size_t transfers = 0;
    Amount total_moved = 0;
};

ValidationResult validate_netting(const Instance&, const Plan&);

struct GridlockCheck {
    bool ok = false;
    string error;
    size_t settled_count = 0;
    Amount settled_value = 0;
};
GridlockCheck validate_gridlock(const Instance&, const vector<int>& selected);