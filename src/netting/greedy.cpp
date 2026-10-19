#include "netting/greedy.hpp"
#include "core/net_balance.hpp"

using namespace std;

string GreedySolver::name() const { return "greedy"; }

Plan GreedySolver::solve(const Instance& inst){
    vector<Amount> bal = net_balance(inst);

    using Entry = pair<Amount, int>;
    priority_queue<Entry> creditors, debtors;

    for(int i = 0; i < inst.n; i++){
        if(bal[i] > 0) creditors.push({bal[i], -i});
        else if(bal[i] < 0) debtors.push({-bal[i], -i});
    }

    Plan plan;

    while(!creditors.empty() && !debtors.empty()){
        Entry c = creditors.top(); 
        creditors.pop();
        Entry d = debtors.top();
        debtors.pop();

        Amount x = min(c.first, d.first);

        Transfer t;
        t.from = -d.second;
        t.to = -c.second;
        t.amt = x;
        plan.transfers.push_back(t);

        if(c.first > x) creditors.push({c.first - x, c.second});
        if(d.first > x) debtors.push({d.first - x, d.second});
    }

    if(!creditors.empty() || !debtors.empty())
        throw logic_error("greedy: balances do not sum to zero");

    return plan;
}