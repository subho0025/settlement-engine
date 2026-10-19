#include "core/io.hpp"

using namespace std;

static const Amount MAX_AMT = 1000000000000LL;
static const Amount MAX_N_VAL = 100000;
static const Amount MAX_M_VAL = 1000000;
Instance read_instance(istream& s){

    Instance inst;
 
    int64_t n, m;
 
    if(!(s >> n)) throw runtime_error("missing or non-numeric n");
    if(!(s >> m)) throw runtime_error("missing or non-numeric m");
 
    if(n < 1 || n > MAX_N_VAL) throw runtime_error("n out of range [1, 100000]");
    if(m < 0 || m > MAX_M_VAL) throw runtime_error("m out of range [0, 1000000]");
 
    inst.n = (int)n;
    inst.obs.resize((int)m);

    for(int i=0; i<m; i++){
        int64_t from, to, amt;

        if(!(s >> from >> to >> amt)) throw runtime_error("obligation " + to_string(i) + ": missing or non-numeric field");

        if(from < 0 || from >= n) throw runtime_error("obligation " + to_string(i) + ": 'from' out of range");
        if(to < 0 || to >= n) throw runtime_error("obligation " + to_string(i) + ": 'to' out of range");
        if(amt < 1 || amt > MAX_AMT) throw runtime_error("obligation " + to_string(i) + ": amt out of range [1, 1e12]");
 
        Obligation o;
        o.from = from;
        o.to = to;
        o.amt = amt;

        inst.obs[i] = o;
    }

    s >> ws;
    if(!s.eof()) throw runtime_error("Trailing data given in input");

    return inst;
}

void write_instance(ostream& s, const Instance& inst){

    s << inst.n << " " << inst.obs.size() << "\n";

    for(int i=0; i<(int)inst.obs.size(); i++){
        s << inst.obs[i].from << " " << inst.obs[i].to << " " << inst.obs[i].amt << "\n";
    }

}

Instance read_gridlock_instance(istream& s){

    Instance inst;
    int64_t n, m;
 
    if(!(s >> n)) throw runtime_error("missing or non-numeric n");
    if(!(s >> m)) throw runtime_error("missing or non-numeric m");
 
    if(n < 1 || n > MAX_N_VAL) throw runtime_error("n out of range [1, 100000]");
    if(m < 0 || m > MAX_M_VAL) throw runtime_error("m out of range [0, 1000000]");
 
    inst.n = (int)n;
    inst.obs.resize((int)m);
    inst.balance.resize((int)n);
    inst.credit_limit.resize((int)n);

    for(int i=0; i<n; i++){
        int64_t bal, limit;

        if(!(s >> bal >> limit)) throw runtime_error("bank " + to_string(i) + ": missing or non-numeric field");

        if(bal < 0 || bal > MAX_AMT) throw runtime_error("bank " + to_string(i) + ": balance out of range [0, 1e12]");
        if(limit < 0 || limit > MAX_AMT) throw runtime_error("bank " + to_string(i) + ": amt out of range [0, 1e12]");

        inst.balance[i] = bal;
        inst.credit_limit[i] = limit;

    }

    for(int i=0; i<m; i++){
        int64_t from, to, amt;

        if(!(s >> from >> to >> amt)) throw runtime_error("payment " + to_string(i) + ": missing or non-numeric field");

        if(from < 0 || from >= n) throw runtime_error("payment " + to_string(i) + ": 'from' out of range");
        if(to < 0 || to >= n) throw runtime_error("payment " + to_string(i) + ": 'to' out of range");
        if(amt < 1 || amt > 1e12) throw runtime_error("payment  " + to_string(i) + ": amt out of range [1, 1e12]");
        if(from == to) throw runtime_error("payment " + to_string(i) + ": payer and payee are the same");
 
        Obligation o;
        o.from = from;
        o.to = to;
        o.amt = amt;

        inst.obs[i] = o;
    }

    s >> ws;
    if(!s.eof()) throw runtime_error("Trailing data given in input");

    return inst;
}

void write_gridlock_instance(ostream& s, const Instance& inst){

    s << inst.n << " " << inst.obs.size() << "\n";

    for(int i=0; i<inst.n; i++){
        s << inst.balance[i] << " " << inst.credit_limit[i] << "\n";
    }

    for(int i=0; i<(int)inst.obs.size(); i++){
        s << inst.obs[i].from << " " << inst.obs[i].to << " " << inst.obs[i].amt << "\n";
    }
    
}