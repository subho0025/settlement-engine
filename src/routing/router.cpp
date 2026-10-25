#include "routing/router.hpp"
#include "flow/mcmf.hpp"
#include "flow/dinic.hpp"
#include "core/types.hpp"

using namespace std;

Router :: Router(int n, const vector<Channel>& channels) : n_(n) , channels_(channels){
    if(n_<1){
        throw invalid_argument("n should be positive");
    }
    for(Channel& c: channels_){
        if(c.from<0 || c.to<0 || c.from>=n_ || c.to>=n_ || c.from == c.to){
            throw invalid_argument("The endpoints of the channels should be in the range [0," + to_string(n_) + ") and from and to should be distinct" );
        }
        if(c.fee_bps<0 || c.fee_bps>10000){
            throw invalid_argument("The basis points should be in the range [0, 10000]");
        }
        if(c.cap<0){
            throw invalid_argument("The capacity of a channel can not be negative");
        }
    }

    rem_.assign(channels_.size(),0);
    for(int i=0; i<(int)channels_.size(); i++){
        rem_[i] = channels_[i].cap;
    }
}

void check_request(int src, int dst, Amount amt, int n){

    if(src<0 || src>=n){
        throw invalid_argument("The src is out of range [0,"+to_string(n)+")");
    }
    if(dst<0 || dst>=n){
        throw invalid_argument("The dst is out of range [0,"+to_string(n)+")");
    }
    if(amt<0){
        throw invalid_argument("The amount is non-positive");
    }
}

Route Router :: route(int src, int dst, Amount amt){

    check_request(src, dst, amt, n_);

    int m = (int)channels_.size();

    Route r;

    r.flow.assign(m, 0);

    if(src==dst || amt==0){
        r.ok = 1;
        r.amount = amt;
        r.fee =0;

        return r;
    }

    MinCostFlow f(n_);
    
    vector<int> ids(m);
    
    for(int i=0; i<m; i++){
        ids[i] = f.add_edge(channels_[i].from, channels_[i].to, rem_[i], channels_[i].fee_bps);
    }

    auto [flow, cost] = f.min_cost_flow(src, dst, amt);

    if(flow<amt){
        r.ok = 0;
    }else{
        for(int i=0; i<m; i++){
            r.flow[i] = f.flow_on(ids[i]);
            rem_[i] -= r.flow[i];
        }
        r.ok = 1;
        r.amount = amt;
        r.fee = cost;
    }

    return r;

}

Route Router :: route_single_path(int src, int dst, Amount amt){

    check_request(src, dst, amt, n_);

    int m = (int)channels_.size();

    Route r;

    r.flow.assign(m, 0);

    if(src==dst || amt==0){
        r.ok = 1;
        r.amount = amt;
        r.fee =0;

        return r;
    }

    vector<vector<int>> adj(n_);

    for(int i=0; i<m; i++){
        if(rem_[i]>=amt){
            adj[channels_[i].from].push_back(i);
        }
    }

    priority_queue<pair<Amount,int>, vector<pair<Amount,int>>, greater<>> pq;
    vector<Amount> dist(n_, numeric_limits<Amount>::max());
    vector<int> p(n_, -1);

    pq.push({0, src});
    dist[src]=0;

    while(!pq.empty()){

        auto [cost, u] = pq.top();
        pq.pop();

        if(dist[u]<cost){
            continue;
        }

        for(int c: adj[u]){
            int v = channels_[c].to;
            if(dist[v]>dist[u]+channels_[c].fee_bps){
                dist[v] = dist[u]+channels_[c].fee_bps;
                p[v]=c;
                pq.push({dist[v], v});
            }
        }
    }

    if(p[dst]==-1){
        r.ok = 0;
    }else{
        int it = p[dst];
        while(it != -1){
            r.flow[it] = amt;
            rem_[it] -= amt;
            it = p[channels_[it].from];
        }
        r.ok = 1;
        r.fee = amt * dist[dst];
        r.amount = amt;
    }

    return r;
}

Amount Router :: max_routable(int src, int dst) const{

    check_request(src, dst, 0, n_);

    int m = (int)channels_.size();

    if(src==dst){
        return numeric_limits<Amount>::max();
    }

    Dinic d(n_);

    for(int i=0; i<m; i++){
        d.add_edge(channels_[i].from, channels_[i].to, rem_[i]);
    }

    return d.max_flow(src, dst);
}
    
const vector<Amount>& Router :: remaining() const{
    return rem_;
}

int Router :: num_nodes() const{
    return n_;
}