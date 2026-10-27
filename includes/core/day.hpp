#pragma once
#include "core/types.hpp"

using namespace std;

struct DayPayment {
    int from = 0;
    int to = 0;
    Amount amt = 0;
    Time t = 0;
    Time deadline = NO_DEADLINE;
};

struct Day {
    int n = 0;
    vector<Amount> balance;
    vector<Amount> credit_limit;
    vector<DayPayment> payments;
    Time end = 0;
};

void write_day(ostream& out, const Day& d);
Day read_day(istream& in);