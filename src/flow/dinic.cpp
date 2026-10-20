#include "flow/dinic.hpp"

using namespace std;

Dinic :: Dinic(int n) : n_(n), g_(n) {}

int Dinic :: num_nodes() const{
    return n_;
}

int Dinic :: add_edge(int u, int v, Amount cap){

    if(u>=n_ || v>=n_ || u<0 || v<0){
        throw invalid_argument("Either u or v are out of range(0 to (" + to_string(n_-1) + ")");
    }
    if(cap<0){
        throw invalid_argument("The amount cap is negative");
    }

    int id = (int)e_.size()/2;

    e_.push_back({v, cap, cap});
    g_[u].push_back((int)e_.size()-1);

    e_.push_back({u, 0, 0});
    g_[v].push_back((int)e_.size()-1);
    
    return id;
    
}

bool Dinic :: bfs(int s, int t){
    
    level_.assign(n_, -1);
    level_[s]=0;

    
    queue<int> q;
    q.push(s);

    while(!q.empty()){
        int u = q.front();
        q.pop();
        for(int& idx: g_[u]){
            int v = e_[idx].to;
            if(e_[idx].cap>0 && level_[v]<0){
                q.push(v);
                level_[v]=level_[u]+1;
            }
        }
    }

    return (level_[t]>=0);

}

Amount Dinic :: dfs(int u, int t, Amount pushed){

    if(u==t) return pushed;

    for(int& i = it_[u]; i< (int)g_[u].size(); i++){
        int idx = g_[u][i];
        int to = e_[idx].to;
        if(e_[idx].cap>0 && level_[to] == level_[u]+1){
            Amount f = dfs(to, t, min(pushed, e_[idx].cap));
            if(f>0){
                e_[idx].cap -=f;
                e_[idx^1].cap += f;
                return f;
            }
        }
    }

    return 0;
}


Amount Dinic :: max_flow(int s, int t){

    if(s>=n_ || t>=n_ || s<0 || t<0 || s==t){
        throw invalid_argument("Either s or t is out of range (0 to (" + to_string(n_-1) + ")");
    }

    Amount flow =0;

    while (bfs(s, t)){
        it_.assign(n_,0);
        Amount f = dfs(s, t, numeric_limits<Amount>::max());
        while(f>0){
            flow +=f;
            f = dfs(s, t, numeric_limits<Amount>::max());
        }
    }

    return flow;
}

Amount Dinic :: flow_on(int id) const{
    if(id<0 || id>= (int)e_.size()/2){
        throw out_of_range("the given id is out of range");
    }

    return (e_[2*id].orig - e_[2*id].cap);
}
vector<bool> Dinic :: min_cut_side(int s) const{

    vector<bool> ans(num_nodes(), 0);
    ans[s]=1;

    queue<int> q;
    q.push(s);

    while(!q.empty()){
        int u = q.front();
        q.pop();

        for(int idx : g_[u]){
            int v = e_[idx].to;
            if(e_[idx].cap>0 && !ans[v]){
                ans[v]=1;
                q.push(e_[idx].to);
            }
        }
    }

    return ans;
}
