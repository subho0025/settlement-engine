#include "netting/naive.hpp"
#include "core/net_balance.hpp"

using namespace std;

string GrossSolver::name() const { return "gross"; }

Plan GrossSolver::solve(const Instance& inst){
    Plan plan;
    plan.transfers.reserve(inst.obs.size());

    for(const Obligation& o : inst.obs){
        if(o.from == o.to) continue;
        Transfer t;
        t.from = o.from;
        t.to = o.to;
        t.amt = o.amt;
        plan.transfers.push_back(t);
    }
    return plan;
}

string HubSolver::name() const { return "hub"; }

Plan HubSolver::solve(const Instance& inst){
    vector<Amount> bal = net_balance(inst);
    Plan plan;

    int hub = -1;
    for(int i = 0; i < inst.n; i++){
        if(bal[i] != 0){
            hub = i;
            break;
        }
    }
    if(hub < 0) return plan;

    for(int i = hub + 1; i < inst.n; i++){
        if(bal[i] < 0){
            Transfer t;
            t.from = i;
            t.to = hub;
            t.amt = -bal[i];
            plan.transfers.push_back(t);
        }else if(bal[i] > 0){
            Transfer t;
            t.from = hub;
            t.to = i;
            t.amt = bal[i];
            plan.transfers.push_back(t);
        }
    }
    return plan;
}