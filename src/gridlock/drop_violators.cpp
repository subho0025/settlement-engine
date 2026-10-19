#include "gridlock/drop_violators.hpp"

using namespace std;


string DropViolatorsSolver :: name() const {return "drop_violators";}

vector<int> DropViolatorsSolver :: solve(const Instance& inst){

    int m = (int)inst.obs.size();

    vector<int> valid_payments(m, 1);
    vector<vector<int>> outgoing(inst.n);
    vector<Amount> slack(inst.n,0);

    for(int i=0; i<inst.n; i++){
        slack[i] = inst.balance[i] + inst.credit_limit[i];
    }

    for(int i=0; i<m; i++){
        slack[inst.obs[i].from] -= inst.obs[i].amt;
        slack[inst.obs[i].to] += inst.obs[i].amt;
        outgoing[inst.obs[i].from].push_back(i);
    }

    queue<int> worklist;
    for(int i=0; i<inst.n; i++){
        worklist.push(i);
    }
    vector<bool> queued(inst.n, 1);

    while(!worklist.empty()){
        int v = worklist.front();
        worklist.pop();
        queued[v]=0;
        while(slack[v]<0){
            int idx = outgoing[v].back();
            int u  = inst.obs[idx].to;
            Amount amt = inst.obs[idx].amt;
            outgoing[v].pop_back();
            valid_payments[idx]=0;
            slack[v] += amt;
            slack[u] -= amt;
            if(!queued[u]){
                worklist.push(u);
                queued[u]=1;
            }
        }
    }

    bool progress =1;
    while(progress){
        progress=0;
        for(int i=0; i<m; i++){
            if(valid_payments[i]) continue;
            int from = inst.obs[i].from;
            int to = inst.obs[i].to;
            Amount amt = inst.obs[i].amt;
            if(slack[from]>=amt){
                slack[from] -= amt;
                slack[to] += amt;
                progress=1;
                valid_payments[i]=1;
            }
        }
    }
    
    vector<int> indices;
    for(int i=0; i<m; i++){
        if(valid_payments[i]){
            indices.push_back(i);
        }
    }

    return indices;
}
