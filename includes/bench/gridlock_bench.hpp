#pragma once
#include "core/instance.hpp"
#include "gridlock/gridlock.hpp"

using namespace std;

Instance with_liquidity(const Instance& base, int permille);

struct RunResult {
    bool ok = false;
    bool valid = false;
    Amount value = 0;
    size_t count = 0;
    double time_ms = 0;
    string error;
};
RunResult run_gridlock(GridlockSolver& solver, const Instance& inst);

struct BenchRow {
    string solver, family;
    int n = 0;
    size_t m = 0;
    uint64_t seed = 0;
    int permille = 0;
    Amount total = 0;
    Amount optimum = -1;
    RunResult run;
};

string csv_header();
string csv_row(const BenchRow& r);

struct GapStats {
    int runs = 0;
    int failed = 0;
    int with_opt = 0;
    int optimal = 0;
    double mean_gap_pct = 0;
    double max_gap_pct = 0;
    double mean_settled_pct = 0;
    double mean_time_ms = 0;
};

map<pair<string,int>, GapStats> summarize(const vector<BenchRow>& rows);
void print_gap_table(ostream& out, const map<pair<string,int>, GapStats>& stats);