#pragma once
#include "core/types.hpp"

using namespace std;

struct Channel {
    int from = 0;
    int to = 0;
    Amount cap = 0;
    int fee_bps = 0; //basis points
};

struct Route {
    bool ok = false;
    Amount amount = 0;
    Amount fee = 0;
    vector<Amount> flow;
};

class Router {
public:
    Router(int n, const vector<Channel>& channels);
    Route route(int src, int dst, Amount amt);
    Route route_single_path(int src, int dst, Amount amt);
    Amount max_routable(int src, int dst) const;
    const vector<Amount>& remaining() const;
    int num_nodes() const;
private:
    int n_;
    vector<Channel> channels_;
    vector<Amount> rem_;
};