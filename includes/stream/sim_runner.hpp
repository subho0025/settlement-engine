#pragma once
#include "core/day.hpp"
#include "stream/engine.hpp"

using namespace std;

struct SimConfig {
    string name = "default";
    string solver = "maxflow_relax";     // fifo_bypass | drop_violators | maxflow_relax | branch_bound
    long long node_limit = 1000000;
    EngineConfig engine;
};

void set_sim_param(SimConfig& c, const string& key, const string& value);
SimConfig read_sim_config(istream& in);
void write_sim_config(ostream& out, const SimConfig& c);

unique_ptr<GridlockSolver> make_solver(const string& name, long long node_limit = -1);

struct SimResult {
    EngineStats stats;
    vector<PaymentRecord> payments;
    Amount total_value = 0;
    double wall_ms = 0;
};

SimResult simulate_day(const Day& d, const SimConfig& c);

string trim_spaces(const string& s);
long long parse_config_int(const string& value);
bool valid_policy_name(const string& name);
string status_to_string(Status s);
string how_to_string(How h);

void write_events(ostream& out, const vector<PaymentRecord>& payments);