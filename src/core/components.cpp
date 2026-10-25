#include "core/components.hpp"

using namespace std;

int find(int v, vector<int>& parent){
    if(parent[v]!=v){
        return parent[v] = find(parent[v], parent);
    }
    return v;
}

void unite(int u, int v, vector<int>& parent, vector<int>& sze){
    int pu = find(u, parent);
    int pv = find(v, parent);
    if(pu==pv) return;
    if(sze[pu]>=sze[pv]){
        parent[pv]=pu;
        sze[pu]+=sze[pv];
    }else{
        parent[pu]=pv;
        sze[pv]+=sze[pu];
    }
}

vector<vector<int>> payment_components(const Instance& inst){

    int m = (int)inst.obs.size();

    vector<int> parent(inst.n);
    vector<int> sze(inst.n, 1);
    for(int i=0; i<inst.n; i++){
        parent[i]=i;
    }

    for(const Obligation& o: inst.obs){
        unite(o.from, o.to, parent, sze);
    }

    vector<vector<int>> by_root(inst.n);
    for(int p=0; p<m; p++){
        by_root[find(inst.obs[p].from, parent)].push_back(p);
    }

    vector<vector<int>> groups;
    for(int root=0; root<inst.n; root++){
        if(!by_root[root].empty()){
            groups.push_back(move(by_root[root]));
        }
    }

    return groups;
}