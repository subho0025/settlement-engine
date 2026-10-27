#pragma once
#include "core/day.hpp"

using namespace std;

struct DayParams {
    uint64_t seed = 1;
    int n = 100;
    int m = 50000;
    Time length = 36000;
    int concentration = 1;
    Amount amount_min = 2000;
    Amount amount_max = 5000000;
    Amount large_threshold = 100000;
    int decade_permille = 400;
    int round_permille = 300;
    vector<int> volume_profile = {5, 10, 12, 10, 7, 8, 10, 12, 9, 6};
    vector<int> value_profile  = {2, 4, 6, 8, 7, 9, 12, 15, 17, 14};
    int deadline_permille = 200;
    Time deadline_min = 900;
    Time deadline_max = 7200;
    int reciprocal_permille = 300;
    Time reciprocal_lag = 1800;
    int liquidity_permille = 50;
    int credit_permille = 0;
};

Day generate_day(const DayParams& p);

void set_day_param(DayParams& p, const string& key, const string& value);
DayParams read_day_params(istream& in);
void write_day_params(ostream& out, const DayParams& p);