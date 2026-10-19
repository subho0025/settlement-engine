#pragma once
#include <bits/stdc++.h>
#include "core/types.hpp"
#include "core/instance.hpp"

using namespace std;

Instance gen_random(int n, int m, Amount max_amt, uint64_t seed);

Instance gen_hub(int n, int m, double skew, Amount max_amt, uint64_t seed);

Instance gen_cycles(int n, int num_cycles, int min_len, int max_len, int noise, Amount max_amt, uint64_t seed);

Instance gen_clusters(int n, int groups, int m, Amount max_amt, uint64_t seed, vector<int>* group_of = nullptr);