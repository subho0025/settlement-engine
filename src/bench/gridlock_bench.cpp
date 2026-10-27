#include "bench/gridlock_bench.hpp"
#include "core/validator.hpp"

using namespace std;

string fixed3(double x){
    stringstream out;
    out << fixed << setprecision(3) << x;
    return out.str();
}

Instance with_liquidity(const Instance& base, int permille){

    if(permille < 0){
        throw invalid_argument("permille must be >= 0");
    }

    Instance inst;
    inst.n = base.n;
    inst.obs = base.obs;
    inst.balance.assign(inst.n, 0);
    inst.credit_limit.assign(inst.n, 0);

    vector<__int128_t> out(inst.n,0);

    for(const Obligation& o: inst.obs){
        out[o.from] += o.amt;
    }

    for(int i=0; i<inst.n; i++){
        inst.balance[i] = (out[i]*permille/1000);
        inst.credit_limit[i] =0;
    }

    return inst;
}

RunResult run_gridlock(GridlockSolver& solver, const Instance& inst){

    RunResult r;

    vector<int> res;
 
    auto start = chrono::steady_clock::now();

    try{
        res = solver.solve(inst);
        r.ok = 1;
    }catch(const exception& e){
        r.ok = 0;
        r.error = e.what();
    }

    auto end = chrono::steady_clock::now();
    r.time_ms = chrono::duration<double, milli>(end - start).count();
 
    if(!r.ok){
        r.valid = 0;
        return r;
    }
 
    GridlockCheck ch = validate_gridlock(inst, res);
    if(!ch.ok){
        r.valid = 0;
        r.error = ch.error;
    }else{
        r.valid = 1;
        r.value = ch.settled_value;
        r.count = ch.settled_count;
    }

    return r;
}

string csv_header(){
    return "solver,family,n,m,seed,liquidity_permille,total,optimum,settled_value,settled_count,"
           "settled_pct,gap_pct,time_ms,ok,valid";
}

string csv_row(const BenchRow& r){

    stringstream out;
    out << r.solver << "," 
    << r.family << "," 
    << r.n << "," 
    << r.m << "," 
    << r.seed << ","
    << r.permille << ","
    << r.total << ",";

    if(r.optimum!=-1){
        out << r.optimum;
    }
    out << ",";

    out << r.run.value << ","
    << r.run.count << ",";

    if(r.run.ok && r.run.valid){
        if(r.total==0){
            out << fixed3(100);
        }else{
            out << fixed3((double)r.run.value/(double)r.total *100);
        }
    }
    out << ",";

    if(r.run.ok && r.run.valid && r.optimum!=-1){
        if(r.optimum==0){
            out << fixed3(0);
        }else{
            out << fixed3((double)(r.optimum-r.run.value)/(double)r.optimum *100);
        }
    }
    out << ",";

    out << fixed3(r.run.time_ms) << ",";
    out << r.run.ok << "," << r.run.valid;

    return out.str();
}

map<pair<string,int>, GapStats> summarize(const vector<BenchRow>& rows){

    map<pair<string,int>, GapStats> stats;

    map<pair<string,int>, int> good_rows;
 
    for(const BenchRow& r: rows){
        pair<string,int> key = {r.solver, r.permille};
        GapStats& g = stats[key];
 
        g.runs++;
        g.mean_time_ms += r.run.time_ms;
 
        if(!(r.run.ok && r.run.valid)){
            g.failed++;
            continue;
        }
 
        good_rows[key]++;
        if(r.total == 0){
            g.mean_settled_pct += 100;
        }else{
            g.mean_settled_pct += ((double)r.run.value / (double)r.total * 100);
        }
 
        if(r.optimum != -1){
            g.with_opt++;
            double gap = 0;
            if(r.optimum != 0){
                gap = ((double)(r.optimum - r.run.value)/(double)r.optimum * 100);
            }
            g.mean_gap_pct += gap;
            g.max_gap_pct = max(g.max_gap_pct, gap);
            if(r.run.value == r.optimum){
                g.optimal++;
            }
        }
    }
 
    for(auto& [key, g]: stats){
        g.mean_time_ms /= g.runs;
        if(good_rows[key] > 0){
            g.mean_settled_pct /= good_rows[key];
        }
        if(g.with_opt > 0){
            g.mean_gap_pct /= g.with_opt;
        }
    }

    return stats;
}

void print_gap_table(ostream& out, const map<pair<string,int>, GapStats>& stats){

    out << "| solver | liquidity | runs | optimal | mean gap % | max gap % | settled % | mean ms |\n";
    out << "|---|---:|---:|---:|---:|---:|---:|---:|\n";

    for(const auto& [key, g] : stats){
        const string& solver = key.first;
        int permille = key.second;

        ostringstream liquidity;
        liquidity << fixed << setprecision(1) << permille / 10.0 << "%";

        out << "| " << solver
            << " | " << liquidity.str()
            << " | " << g.runs
            << " | " << g.optimal << "/" << g.with_opt
            << " | " << fixed3(g.mean_gap_pct)
            << " | " << fixed3(g.max_gap_pct)
            << " | " << fixed3(g.mean_settled_pct)
            << " | " << fixed3(g.mean_time_ms)
            << " |\n";
    }
}