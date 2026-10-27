#include "core/day.hpp"
#include "gen/day_gen.hpp"
#include "stream/sim_runner.hpp"

using namespace std;

static void usage(){
    cerr << "usage: simulate (--day FILE | --gen DAY_CONFIG [--seed N]) [--sim SIM_CONFIG]... [--set key=value]...\n"
            "                [--events FILE] [--csv FILE]\n"
            "  --day FILE         the payment day to replay (written by gen_day)\n"
            "  --gen DAY_CONFIG   generate the day on the fly from a gen_day config instead\n"
            "  --seed N           with --gen: override the day's seed\n"
            "  --sim SIM_CONFIG   a policy to run (repeatable: several = side-by-side comparison);\n"
            "                     without --sim the built-in default policy runs\n"
            "  --set key=value    override a policy setting in every --sim (repeatable)\n"
            "  --events FILE      write the per-payment event log (only with a single policy)\n"
            "  --csv FILE         write one summary row per policy\n";
}

static string pct(Amount part, Amount whole){
    ostringstream s;
    s << fixed << setprecision(2) << (whole > 0 ? 100.0 * (double)part / (double)whole : 0.0) << "%";
    return s.str();
}

static string describe(const SimConfig& c){
    ostringstream s;
    s << "solver " << c.solver;
    if(c.solver == "branch_bound") s << " (node limit " << c.node_limit << ")";
    if(c.engine.batch_interval > 0) s << ", batch every " << c.engine.batch_interval;
    if(c.engine.queue_trigger > 0) s << ", batch at queue size " << c.engine.queue_trigger;
    if(c.engine.value_trigger > 0) s << ", batch at queued value " << c.engine.value_trigger;
    if(c.engine.deadline_batch) s << ", deadline batches";
    if(c.engine.batch_interval == 0 && c.engine.queue_trigger == 0 && c.engine.value_trigger == 0 && !c.engine.deadline_batch)
        s << ", no batches until close (arrival + recycling only)";
    return s.str();
}

static void report(const SimConfig& c, const SimResult& r){
    const EngineStats& s = r.stats;
    size_t n = s.submitted;
    cout << "policy " << c.name << ": " << describe(c) << "\n"
         << "  settled     " << s.settled << " payments (" << pct((Amount)s.settled, (Amount)n) << "), value "
         << s.settled_value << " (" << pct(s.settled_value, r.total_value) << ")\n"
         << "              on arrival " << s.by_arrival << " | by recycling " << s.by_recycle << " | in batches " << s.by_batch << "\n"
         << "  expired     " << s.expired << " payments, value " << s.expired_value << "\n"
         << "  unsettled   " << s.unsettled << " payments, value " << s.unsettled_value << "\n"
         << "  batches     " << s.batches << "\n"
         << "  delay       p50 " << s.delay_p50 << " | p95 " << s.delay_p95 << " | p99 " << s.delay_p99 << " | max " << s.delay_max << "\n"
         << fixed << setprecision(1)
         << "  engine time " << r.wall_ms << " ms (" << (r.wall_ms > 0 ? (double)n / r.wall_ms * 1000.0 : 0.0) << " payments/s)\n";
    cout.unsetf(ios::fixed);
}

static void compare_table(const vector<SimConfig>& cs, const vector<SimResult>& rs){
    vector<vector<string>> rows = {{"policy", "solver", "value settled", "count settled", "expired", "unsettled",
                                    "batches", "p50", "p95", "p99", "max", "ms"}};
    for(size_t i = 0; i < cs.size(); i++){
        const EngineStats& s = rs[i].stats;
        ostringstream ms;
        ms << fixed << setprecision(1) << rs[i].wall_ms;
        rows.push_back({cs[i].name, cs[i].solver, pct(s.settled_value, rs[i].total_value), pct((Amount)s.settled, (Amount)s.submitted),
                        to_string(s.expired), to_string(s.unsettled), to_string(s.batches), to_string(s.delay_p50),
                        to_string(s.delay_p95), to_string(s.delay_p99), to_string(s.delay_max), ms.str()});
    }
    vector<size_t> w(rows[0].size(), 0);
    for(const auto& row : rows) for(size_t k = 0; k < row.size(); k++) w[k] = max(w[k], row[k].size());
    for(size_t r = 0; r < rows.size(); r++){
        for(size_t k = 0; k < rows[r].size(); k++){
            if(k < 2) cout << left << setw((int)w[k]) << rows[r][k];
            else cout << right << setw((int)w[k]) << rows[r][k];
            cout << (k + 1 < rows[r].size() ? "  " : "\n");
        }
        if(r == 0){
            size_t total = 0;
            for(size_t k = 0; k < w.size(); k++) total += w[k] + (k + 1 < w.size() ? 2 : 0);
            cout << string(total, '-') << "\n";
        }
    }
    cout << left;
}

int main(int argc, char** argv){
    string day_path, gen_path, events_path, csv_path, seed;
    vector<string> sim_paths;
    vector<pair<string,string>> overrides;

    for(int i = 1; i < argc; i++){
        string a = argv[i];
        auto next = [&]() -> string {
            if(i + 1 >= argc){ usage(); exit(1); }
            return argv[++i];
        };
        if(a == "--day") day_path = next();
        else if(a == "--gen") gen_path = next();
        else if(a == "--seed") seed = next();
        else if(a == "--sim") sim_paths.push_back(next());
        else if(a == "--events") events_path = next();
        else if(a == "--csv") csv_path = next();
        else if(a == "--set"){
            string kv = next();
            size_t eq = kv.find('=');
            if(eq == string::npos){ cerr << "error: --set needs key=value\n"; return 1; }
            overrides.push_back({kv.substr(0, eq), kv.substr(eq + 1)});
        }
        else if(a == "-h" || a == "--help"){ usage(); return 0; }
        else{ usage(); return 1; }
    }
    if(day_path.empty() == gen_path.empty()){
        cerr << "error: give exactly one of --day and --gen\n";
        usage();
        return 1;
    }

    try{
        // the day
        Day d;
        if(!day_path.empty()){
            if(!seed.empty()) throw runtime_error("--seed only works with --gen");
            ifstream in(day_path);
            if(!in) throw runtime_error("cannot open " + day_path);
            d = read_day(in);
        }else{
            ifstream in(gen_path);
            if(!in) throw runtime_error("cannot open " + gen_path);
            DayParams p = read_day_params(in);
            if(!seed.empty()) set_day_param(p, "seed", seed);
            d = generate_day(p);
        }

        // the policies
        vector<SimConfig> configs;
        if(sim_paths.empty()) configs.push_back(SimConfig{});
        for(const string& path : sim_paths){
            ifstream in(path);
            if(!in) throw runtime_error("cannot open " + path);
            try{
                configs.push_back(read_sim_config(in));
            }catch(const exception& e){
                throw runtime_error(path + ": " + e.what());
            }
        }
        for(SimConfig& c : configs) for(const auto& [k, v] : overrides) set_sim_param(c, k, v);
        if(!events_path.empty() && configs.size() != 1) throw runtime_error("--events needs exactly one policy");

        Amount total = 0;
        for(const DayPayment& x : d.payments) total += x.amt;
        cout << "day: " << d.n << " banks, " << d.payments.size() << " payments, total value " << total
             << ", times 0.." << d.end << "\n\n";

        vector<SimResult> results;
        for(const SimConfig& c : configs){
            results.push_back(simulate_day(d, c));
            report(c, results.back());
            cout << "\n";
        }
        if(configs.size() > 1) compare_table(configs, results);

        if(!events_path.empty()){
            ofstream out(events_path);
            if(!out) throw runtime_error("cannot write " + events_path);
            write_events(out, results[0].payments);
            cerr << "events: " << results[0].payments.size() << " payments -> " << events_path << "\n";
        }
        if(!csv_path.empty()){
            ofstream out(csv_path);
            if(!out) throw runtime_error("cannot write " + csv_path);
            out << "policy,solver,batch_interval,queue_trigger,value_trigger,deadline_batch,payments,total_value,"
                   "settled,settled_value,by_arrival,by_recycle,by_batch,expired,expired_value,unsettled,unsettled_value,"
                   "batches,delay_p50,delay_p95,delay_p99,delay_max,wall_ms\n";
            for(size_t i = 0; i < configs.size(); i++){
                const SimConfig& c = configs[i];
                const EngineStats& s = results[i].stats;
                out << c.name << "," << c.solver << "," << c.engine.batch_interval << "," << c.engine.queue_trigger << ","
                    << c.engine.value_trigger << "," << (c.engine.deadline_batch ? 1 : 0) << "," << s.submitted << ","
                    << results[i].total_value << "," << s.settled << "," << s.settled_value << "," << s.by_arrival << ","
                    << s.by_recycle << "," << s.by_batch << "," << s.expired << "," << s.expired_value << "," << s.unsettled << ","
                    << s.unsettled_value << "," << s.batches << "," << s.delay_p50 << "," << s.delay_p95 << "," << s.delay_p99
                    << "," << s.delay_max << "," << fixed << setprecision(3) << results[i].wall_ms << "\n";
                out.unsetf(ios::fixed);
            }
            cerr << "summary: " << configs.size() << " rows -> " << csv_path << "\n";
        }
    }catch(const exception& e){
        cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}