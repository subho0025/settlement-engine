#include "stream/sim_runner.hpp"
#include "gridlock/fifo_bypass.hpp"
#include "gridlock/drop_violators.hpp"
#include "gridlock/maxflow_relax.hpp"
#include "gridlock/branch_bound.hpp"

using namespace std;

string trim_spaces(const string& s){

    int start = 0;
    int end = (int)s.size() - 1;

    while(start <= end && (s[start]==' ' || s[start]=='\t')){
        start++;
    }
    while(end >= start && (s[end]==' ' || s[end]=='\t')){
        end--;
    }

    return s.substr(start, end - start + 1);
}

long long parse_config_int(const string& value){

    int start = 0;
    if(!value.empty() && value[0] == '-'){
        start = 1;
    }

    int digits = (int)value.size() - start;

    if(digits < 1 || digits > 18){
        throw runtime_error("bad number '" + value + "'");
    }
    
    for(int i=start; i<(int)value.size(); i++){
        if(value[i] < '0' || value[i] > '9'){
            throw runtime_error("bad number '" + value + "'");
        }
    }

    return stoll(value);
}

bool valid_policy_name(const string& name){

    if(name.empty() || name.size() > 40){
        return 0;
    }
    for(char c: name){
        bool ok = isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.';
        if(!ok){
            return 0;
        }
    }

    return 1;
}

void set_sim_param(SimConfig& c, const string& key, const string& value){

    if(value.empty()){
        throw runtime_error("empty value for '" + key + "'");
    }

    if(key == "name"){
        if(!valid_policy_name(value)){
            throw runtime_error("name must be 1-40 characters from A-Z a-z 0-9 _ - .");
        }
        c.name = value;
    }else if(key == "solver"){
        make_solver(value);
        c.solver = value;
    }else if(key == "node_limit"){
        long long x = parse_config_int(value);
        if(x < -1){
            throw runtime_error("node_limit must be >= -1");
        }
        c.node_limit = x;
    }else if(key == "batch_interval"){
        long long x = parse_config_int(value);
        if(x < 0){
            throw runtime_error("batch_interval must be >= 0");
        }
        c.engine.batch_interval = x;
    }else if(key == "queue_trigger"){
        long long x = parse_config_int(value);
        if(x < 0){
            throw runtime_error("queue_trigger must be >= 0");
        }
        c.engine.queue_trigger = (size_t)x;
    }else if(key == "value_trigger"){
        long long x = parse_config_int(value);
        if(x < 0){
            throw runtime_error("value_trigger must be >= 0");
        }
        c.engine.value_trigger = x;
    }else if(key == "deadline_batch"){
        if(value != "0" && value != "1"){
            throw runtime_error("deadline_batch must be 0 or 1");
        }
        c.engine.deadline_batch = (value == "1");
    }else{
        throw runtime_error("unknown key '" + key + "'");
    }
}

SimConfig read_sim_config(istream& in){

    SimConfig c;
    set<string> seen;
    string line;
    int line_no = 0;

    while(getline(in, line)){
        line_no++;

        if(!line.empty() && line.back() == '\r'){
            line.pop_back();
        }
        string t = trim_spaces(line);
        if(t.empty() || t[0] == '#'){
            continue;
        }

        try{
            size_t eq = t.find('=');
            if(eq == string::npos){
                throw runtime_error("expected 'key = value'");
            }
            string key = trim_spaces(t.substr(0, eq));
            string value = trim_spaces(t.substr(eq + 1));

            if(seen.count(key)){
                throw runtime_error("duplicate key '" + key + "'");
            }
            set_sim_param(c, key, value);
            seen.insert(key);
        }catch(const exception& e){
            throw runtime_error("line " + to_string(line_no) + ": " + e.what());
        }
    }

    return c;
}

void write_sim_config(ostream& out, const SimConfig& c){

    out << "# settle simulation config v1\n";
    out << "name = " << c.name << "\n";
    out << "solver = " << c.solver << "\n";
    out << "node_limit = " << c.node_limit << "\n";
    out << "batch_interval = " << c.engine.batch_interval << "\n";
    out << "queue_trigger = " << c.engine.queue_trigger << "\n";
    out << "value_trigger = " << c.engine.value_trigger << "\n";
    out << "deadline_batch = " << (c.engine.deadline_batch ? 1 : 0) << "\n";
}

unique_ptr<GridlockSolver> make_solver(const string& name, long long node_limit){

    if(name == "fifo_bypass"){
        return make_unique<FifoBypassSolver>();
    }
    if(name == "drop_violators"){
        return make_unique<DropViolatorsSolver>();
    }
    if(name == "maxflow_relax"){
        return make_unique<MaxflowRelaxSolver>();
    }
    if(name == "branch_bound"){
        return make_unique<BranchBoundSolver>(node_limit);
    }

    throw invalid_argument("unknown solver '" + name + "' (fifo_bypass, drop_violators, maxflow_relax, branch_bound)");
}

SimResult simulate_day(const Day& d, const SimConfig& c){

    unique_ptr<GridlockSolver> solver = make_solver(c.solver, c.node_limit);

    SimResult result;
    for(const DayPayment& p: d.payments){
        result.total_value += p.amt;
    }

    auto start = chrono::steady_clock::now();

    SettlementEngine engine(d.balance, d.credit_limit, *solver, c.engine);
    for(const DayPayment& p: d.payments){
        engine.submit(p.from, p.to, p.amt, p.t, p.deadline);
    }
    engine.close(d.end);

    auto stop = chrono::steady_clock::now();
    result.wall_ms = chrono::duration<double, milli>(stop - start).count();

    result.stats = engine.stats();
    result.payments = engine.payments();

    return result;
}

string status_to_string(Status s){

    if(s == Status::Queued) return "queued";
    if(s == Status::Settled) return "settled";
    if(s == Status::Expired) return "expired";
    return "unsettled";
}

string how_to_string(How h){

    if(h == How::Arrival) return "arrival";
    if(h == How::Recycle) return "recycle";
    if(h == How::Batch) return "batch";
    return "none";
}

void write_events(ostream& out, const vector<PaymentRecord>& payments){

    out << "id,from,to,amount,arrival,deadline,status,how,done_at\n";

    for(int i=0; i<(int)payments.size(); i++){
        const PaymentRecord& r = payments[i];

        out << i << "," << r.from << "," << r.to << "," << r.amt << "," << r.arrival << ",";
        if(r.deadline != NO_DEADLINE){
            out << r.deadline;
        }
        out << "," << status_to_string(r.status) << "," << how_to_string(r.how) << ",";
        if(r.done_at >= 0){
            out << r.done_at;
        }
        out << "\n";
    }
}