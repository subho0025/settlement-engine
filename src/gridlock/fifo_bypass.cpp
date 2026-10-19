#include "gridlock/fifo_bypass.hpp"

using namespace std;

string FifoBypassSolver :: name() const {return "fifo_bypass";}

vector<int> FifoBypassSolver :: solve(const Instance& inst){

    int m = (int)inst.obs.size();

    vector<Amount> liquidity(inst.n,0);
    vector<bool> settled(m, 0);

    for(int i=0; i<inst.n; i++){
        liquidity[i] = inst.balance[i] + inst.credit_limit[i];
    }

    bool progress = 1;

    while(progress){
        progress =0;

        for(int i=0; i<m; i++){
            if(settled[i]) continue;

            if(liquidity[inst.obs[i].from]>=inst.obs[i].amt){
                liquidity[inst.obs[i].from] -= inst.obs[i].amt;
                liquidity[inst.obs[i].to] += inst.obs[i].amt;
                settled[i]=1;
                progress=1;
            }
        }
    }

    vector<int> settled_indices;
    for(int i=0; i<m; i++){
        if(settled[i]){
            settled_indices.push_back(i);
        }
    }

    return settled_indices;
}