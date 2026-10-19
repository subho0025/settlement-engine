#pragma once
#include <bits/stdc++.h>
#include "core/types.hpp"
#include "core/instance.hpp"

using namespace std;

class NettingSolver {
public:
    virtual ~NettingSolver() = default;
    virtual string name() const = 0;
    virtual Plan solve(const Instance&) = 0;
};