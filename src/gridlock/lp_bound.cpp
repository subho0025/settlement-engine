#include "gridlock/lp_bound.hpp"
#include "flow/mcmf.hpp"
#include "core/components.hpp"

using namespace std;

LpBound gridlock_lp_bound(const Instance& inst){

    vector<Amount> need(inst.n, 0);

    int m = (int)inst.obs.size();

    for(int i=0; i<inst.n; i++){
        need[i] =-(inst.balance[i]+inst.credit_limit[i]);
    }

    for(const Obligation& o: inst.obs){
        need[o.from] += o.amt;
        need[o.to] -= o.amt;
    }

    LpBound result;

    for(const Obligation& o: inst.obs){
        result.total += o.amt;
    }

    result.unsettled.assign(m, 0);
    Amount cost_total = 0;

    vector<int> local(inst.n, -1);

    for(const vector<int>& pays: payment_components(inst)){

        vector<int> banks;
        for(int p: pays){
            int from = inst.obs[p].from;
            int to = inst.obs[p].to;
            if(local[from]<0){
                local[from]=(int)banks.size();
                banks.push_back(from);
            }
            if(local[to]<0){
                local[to]=(int)banks.size();
                banks.push_back(to);
            }
        }

        int k = (int)banks.size();
        int s = k;
        int t = k+1;
        MinCostFlow mcmf(k+2);

        Amount totalPosNeed = 0;
        for(int j=0; j<k; j++){
            Amount nd = need[banks[j]];
            if(nd>0){
                mcmf.add_edge(s, j, nd, 0);
                totalPosNeed += nd;
            }else if(nd<0){
                mcmf.add_edge(j, t, -nd, 0);
            }
        }

        vector<int> ids;
        for(int p: pays){
            ids.push_back(mcmf.add_edge(local[inst.obs[p].from], local[inst.obs[p].to], inst.obs[p].amt, 1));
        }
        
        auto [flow, cost] = mcmf.min_cost_flow(s, t);
 
        if(flow!=totalPosNeed){
            throw logic_error("There is some error in the implementation of min cost flow");
        }
 
        for(int q=0; q<(int)pays.size(); q++){
            result.unsettled[pays[q]] = mcmf.flow_on(ids[q]);
        }
        cost_total += cost;
 
        for(int v: banks){
            local[v] = -1;
        }
    }
 
    result.value = result.total - cost_total;
 
    return result;

}