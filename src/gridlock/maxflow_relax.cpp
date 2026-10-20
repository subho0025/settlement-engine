#include "gridlock/maxflow_relax.hpp"

using namespace std;

string MaxflowRelaxSolver  :: name() const {return "maxflow_relax";}

vector<int> MaxflowRelaxSolver :: solve(const Instance& inst){

    flow_ =0;
    relaxed_ =0;
    y_.clear();

    vector<Amount> cap(inst.n,0);
    vector<Amount> d(inst.n,0); //ideal net flow(out - in)
    vector<Amount> need(inst.n,0);

    for(int i=0; i<inst.n; i++){
        cap[i] = inst.balance[i] + inst.credit_limit[i];
    }
    for(const Obligation& o: inst.obs){
        d[o.from] += o.amt;
        d[o.to] -= o.amt;
    }
    for(int i=0; i<inst.n; i++){
        need[i] = d[i]-cap[i];
    }

    Dinic dinic = Dinic(inst.n+2);
    int s = inst.n;
    int t = inst.n+1;

    Amount need_pos = 0;

    for(int i=0; i<inst.n; i++){
        if(need[i]>0){
            dinic.add_edge(s, i, need[i]);
            need_pos += need[i];
        }else if(need[i]<0){
            dinic.add_edge(i, t, -need[i]);
        }
    }

    int m = (int)inst.obs.size();

    vector<int> id(m);
    for(int i=0; i<m; i++){
        id[i] = dinic.add_edge(inst.obs[i].from, inst.obs[i].to, inst.obs[i].amt);
    }

    flow_ = dinic.max_flow(s, t);
    if(flow_ != need_pos){
        throw logic_error("There is somethig wrong with the Dinic algorithm");
    }
    
    y_.resize(m);
    vector<Amount> x(m);

    for(int i=0; i<m; i++){
        y_[i] = dinic.flow_on(id[i]);
        x[i] = inst.obs[i].amt - y_[i];
        relaxed_ += x[i];
    }

    vector<bool> in_S(m, 0);
    for(int i=0; i<m; i++){
        if(x[i]>0) in_S[i]=1;
    }

    vector<Amount> pos = cap;
    for(int i=0; i<m; i++){
        if(in_S[i]){
            pos[inst.obs[i].from] -= inst.obs[i].amt;
            pos[inst.obs[i].to] += inst.obs[i].amt;
        }
    }
    
    vector<vector<int>> outgoing(inst.n);
    for(int i=0; i<m; i++){
        if(in_S[i]){
            outgoing[inst.obs[i].from].push_back(i);
        }
    }

    queue<int> worklist;

    for(int i=0; i<inst.n; i++){
        if(pos[i]<0){
            worklist.push(i);
        }
    }

    while(!worklist.empty()){
        int u = worklist.front();
        worklist.pop();

        while(pos[u]<0){
            __int128_t minNum =1;
            __int128_t minDen =1;
            int mini =-1;
            for(int p: outgoing[u]){
                if(in_S[p] && (minDen*x[p] <= minNum * (x[p]+y_[p]))){
                    minNum = x[p];
                    minDen = x[p]+y_[p];
                    mini = p;
                }
            }
            pos[u] += x[mini]+y_[mini];
            int v = inst.obs[mini].to;
            pos[v] -= x[mini]+y_[mini];
            in_S[mini] =0;
            if(pos[v]<0){
                worklist.push(v);
            }
            
        }
    }

    vector<int> repair;
    for(int i=0; i<m; i++){
        if(!in_S[i]){
            repair.push_back(i);
        }
    }

    stable_sort(repair.begin(), repair.end(), [&](const int a, const int b){
        return inst.obs[a].amt>inst.obs[b].amt;
    });

    int changed =1;
    while(changed){
        changed=0;
        for(int i: repair){
            if(!in_S[i] && pos[inst.obs[i].from]>=inst.obs[i].amt){
                pos[inst.obs[i].from] -= inst.obs[i].amt;
                pos[inst.obs[i].to] += inst.obs[i].amt;
                in_S[i]=1;
                changed =1;
            }
        }
    }

    vector<int> ans;

    for(int i=0; i<m; i++){
        if(in_S[i]){
            ans.push_back(i);
        }
    }

    return ans;

}

Amount MaxflowRelaxSolver :: flow_value() const{
    return flow_;
}

Amount MaxflowRelaxSolver :: relaxed_value() const{
    return relaxed_;
}
const vector<Amount>& MaxflowRelaxSolver :: unsettled() const{
    return y_;
}
