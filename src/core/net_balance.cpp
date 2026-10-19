#include "core/net_balance.hpp"

using namespace std;

vector<Amount> net_balance(const Instance& inst){

    vector<Amount> balance(inst.n,0);

    for(const Obligation& obs: inst.obs){
        balance[obs.from]-= obs.amt;
        balance[obs.to] += obs.amt;
    }

    return balance;
}