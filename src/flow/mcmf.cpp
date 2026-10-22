#include "flow/mcmf.hpp"

using namespace std;

MinCostFlow :: MinCostFlow(int n) : n_(n), g_(n) {
    if(n <= 0) {
        throw invalid_argument("Number of nodes must be positive");
    }
}

int MinCostFlow :: add_edge(int u, int v, Amount cap, Amount cost){

    if(u>=n_ || v>=n_ || u<0 || v<0){
        throw invalid_argument("Either u or v are out of range(0 to (" + to_string(n_-1) + ")");
    }
    if(cap<0){
        throw invalid_argument("The amount cap is negative");
    }

    int id = (int)e_.size()/2;

    e_.push_back({u, v, cap, cap, cost});
    g_[u].push_back((int)e_.size()-1);

    e_.push_back({v, u, 0, 0, -cost});
    g_[v].push_back((int)e_.size()-1);
    
    return id;
}

void MinCostFlow :: bellmanFord(int s){

    Amount inf = numeric_limits<Amount>::max();
    dist_.assign(n_, inf);
    prev_.assign(n_, -1);
    dist_[s]=0;

    for(int k=0; k<n_; k++){
        bool changed =0;
        for(int i=0; i<(int)e_.size(); i++){
            int from = e_[i].from;
            int to = e_[i].to;
            Amount cost = e_[i].cost;
            Amount cap = e_[i].cap;
            if(dist_[from]<inf && dist_[from]+cost<dist_[to] && cap>0){
                dist_[to] = dist_[from]+cost;
                prev_[to] = i;
                changed =1;
            }
        }
        if(!changed) break;
    }

    for(Edge& edge: e_){
        if(dist_[edge.from]<inf && dist_[edge.from]+edge.cost<dist_[edge.to] && edge.cap>0){
            throw invalid_argument("There exists a negative cycle of costs reachable from s");
        }
    }

}

void MinCostFlow :: dijkstra(int s){

    Amount inf = numeric_limits<Amount>::max();
    dist_.assign(n_, inf);
    prev_.assign(n_, -1);
    dist_[s]=0;

    priority_queue<pair<Amount,int>, vector<pair<Amount,int>>, greater<>> pq;
    pq.push({0, s});

    while(!pq.empty()){
        auto [d, u] = pq.top();
        pq.pop();

        if(d>dist_[u]) continue;

        for(int idx : g_[u]){
            if(e_[idx].cap>0){
                int to = e_[idx].to;
                Amount cost = e_[idx].cost;
                if(d+cost+h_[u]-h_[to]<dist_[to]){
                    dist_[to] = d+cost+h_[u]-h_[to];
                    prev_[to] = idx;
                    pq.push({dist_[to], to});
                }
            }
        }
    }

}

pair<Amount, Amount> MinCostFlow :: min_cost_flow(int s, int t, Amount limit){

    if(s < 0 || s >= n_ || t < 0 || t >= n_) {
        throw invalid_argument("Source or sink out of range");
    }
    if(s == t) {
        throw invalid_argument("Source and sink cannot be the same");
    }
    if(limit < 0) {
        throw invalid_argument("Limit cannot be negative");
    }

    Amount flow =0;
    Amount cost =0;

    Amount inf = numeric_limits<Amount>::max();

    bellmanFord(s);
    h_ = dist_;
    for(int i=0; i<n_; i++){
        if(dist_[i]==inf){
            h_[i]=0;
        }
    }
    
    while(flow<limit){
        dijkstra(s);
        if(dist_[t]==inf) break;

        for(int i=0; i<n_; i++){
            if(dist_[i]<inf){
                h_[i] += dist_[i];
            }
        }

        int it = t;
        Amount pushed = inf;
        while(it!=s){
            pushed = min(pushed, e_[prev_[it]].cap);
            it = e_[prev_[it]].from;
        }
        pushed = min(pushed, limit-flow);

        it = t;
        while(it!=s){
            e_[prev_[it]].cap -= pushed;
            e_[prev_[it]^1].cap += pushed;
            it = e_[prev_[it]].from;
        }

        flow += pushed;
        cost += pushed*(h_[t]-h_[s]);
    }

    return {flow, cost};

}

Amount MinCostFlow :: flow_on(int edge_id) const{

    if (edge_id < 0 || edge_id >= (int)e_.size()/2) {
        throw invalid_argument("Invalid edge ID");
    }

    return e_[2*edge_id+1].cap;
}
