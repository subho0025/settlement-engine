#include "bench/gridlock_bench.hpp"
#include "gen/generators.hpp"
#include "gridlock/fifo_bypass.hpp"
#include "gridlock/drop_violators.hpp"
#include "gridlock/maxflow_relax.hpp"
#include "gridlock/branch_bound.hpp"

using namespace std;

void print_usage(){
    cerr << "usage: bench_gridlock [--quick] [--out FILE] [--node-limit N]\n";
    cerr << "  --quick          small grid (one size, 5 seeds)\n";
    cerr << "  --out FILE       CSV output (default gridlock_bench.csv)\n";
    cerr << "  --node-limit N   branch & bound search limit per instance (default 2000000)\n";
}

Instance make_family(const string& family, int n, int m, uint64_t seed){

    Amount max_amt = 1000000;

    if(family == "random"){
        return gen_random(n, m, max_amt, seed);
    }
    if(family == "hub"){
        return gen_hub(n, m, 1.2, max_amt, seed);
    }
    if(family == "cycles"){
        return gen_cycles(n, n/2, 2, min(n, 6), n, max_amt, seed);
    }
    return gen_clusters(n, n/5, m, max_amt, seed);
}

int main(int argc, char** argv){

    bool quick = 0;
    string out_path = "gridlock_bench.csv";
    long long node_limit = 2000000;

    for(int i=1; i<argc; i++){
        string arg = argv[i];
        if(arg == "--quick"){
            quick = 1;
        }else if(arg == "--out" && i+1 < argc){
            out_path = argv[++i];
        }else if(arg == "--node-limit" && i+1 < argc){
            node_limit = stoll(argv[++i]);
        }else{
            print_usage();
            return 1;
        }
    }

    vector<string> families = {"random", "hub", "cycles", "clusters"};
    vector<int> levels = {0, 100, 200, 300, 500, 750, 1000};

    vector<pair<int,int>> sizes;
    int seeds;
    if(quick){
        sizes = {{10, 30}};
        seeds = 5;
    }else{
        sizes = {{10, 30}, {20, 60}, {40, 120}};
        seeds = 20;
    }

    ofstream csv(out_path);
    if(!csv){
        cerr << "error: cannot write " << out_path << "\n";
        return 1;
    }
    csv << csv_header() << "\n";

    FifoBypassSolver fifo;
    DropViolatorsSolver drop;
    MaxflowRelaxSolver relax;
    vector<GridlockSolver*> heuristics = {&fifo, &drop, &relax};

    vector<BenchRow> rows;

    for(auto [n, m] : sizes){
        for(const string& family : families){
            for(int seed=0; seed<seeds; seed++){

                Instance base = make_family(family, n, m, seed);

                Amount total = 0;
                for(const Obligation& o : base.obs){
                    total += o.amt;
                }

                for(int permille : levels){

                    Instance inst = with_liquidity(base, permille);

                    BranchBoundSolver exact(node_limit);
                    RunResult exact_run = run_gridlock(exact, inst);
                    Amount optimum = -1;
                    if(exact_run.ok && exact_run.valid){
                        optimum = exact_run.value;
                    }

                    vector<pair<string, RunResult>> runs;
                    runs.push_back({exact.name(), exact_run});
                    for(GridlockSolver* h : heuristics){
                        runs.push_back({h->name(), run_gridlock(*h, inst)});
                    }

                    for(auto& [solver_name, run] : runs){
                        BenchRow row;
                        row.solver = solver_name;
                        row.family = family;
                        row.n = n;
                        row.m = inst.obs.size();
                        row.seed = seed;
                        row.permille = permille;
                        row.total = total;
                        row.optimum = optimum;
                        row.run = run;

                        csv << csv_row(row) << "\n";
                        rows.push_back(row);
                    }
                }
            }
        }
    }

    print_gap_table(cout, summarize(rows));
    cerr << rows.size() << " rows written to " << out_path << "\n";

    return 0;
}