#include "gridlock/branch_bound.hpp"
#include "core/components.hpp"

using namespace std;

BranchBoundSolver :: BranchBoundSolver(long long node_limit) : node_limit_(node_limit) {}

string BranchBoundSolver :: name() const {return "branch_bound";}

long long BranchBoundSolver :: nodes_explored() const{
    return nodes_;
}

struct Bank{
    Amount cap = 0;
    Amount curr_in = 0;
    Amount curr_out = 0;
    Amount rem_in = 0;
    Amount rem_out = 0;
    Amount ub = 0;
};

Amount bank_bound(const Bank& b){
    return min(b.curr_out + b.rem_out, b.cap + b.curr_in + b.rem_in);
}

void refresh(int i, Amount& ub_total, vector<Bank>& banks){
    ub_total -= banks[i].ub;
    banks[i].ub = bank_bound(banks[i]);
    ub_total += banks[i].ub;
}

void dfs(long long& nodes, const long long& node_limit, const Instance& inst, const vector<int>& pays, int pos, Amount curr_value, vector<bool>& curr_choices, 
    Amount& best_value, vector<bool>& best_choices, vector<Bank>& banks, Amount& ub_total){

    nodes++;
    if(node_limit>=0 && nodes> node_limit){
        throw runtime_error("Nodes limit exceeded");
    }

    if(ub_total<=best_value) return;

    int k = (int)pays.size();

    if(pos == k){
        if(curr_value>best_value){
            best_value = curr_value;
            best_choices = curr_choices;
        }
        return;
    }

    int from = inst.obs[pays[pos]].from;
    int to = inst.obs[pays[pos]].to;
    Amount amt = inst.obs[pays[pos]].amt;


    banks[from].rem_out-=amt;
    banks[to].rem_in-=amt;
    refresh(from, ub_total, banks);
    refresh(to, ub_total, banks);

    if(banks[from].curr_out+amt <= banks[from].cap + banks[from].curr_in + banks[from].rem_in){
        curr_choices[pos]=1;
        banks[from].curr_out+=amt;
        banks[to].curr_in+=amt;
        refresh(from, ub_total, banks);
        refresh(to, ub_total, banks);
        dfs(nodes, node_limit, inst,pays, pos+1, curr_value+amt, curr_choices, best_value, best_choices, banks, ub_total);
        curr_choices[pos]=0;
        banks[from].curr_out-=amt;
        banks[to].curr_in-=amt;
        refresh(from, ub_total, banks);
        refresh(to, ub_total, banks);
    }

    if(banks[to].curr_out <= banks[to].cap+banks[to].curr_in+banks[to].rem_in){
        dfs(nodes, node_limit, inst, pays, pos+1, curr_value, curr_choices, best_value, best_choices, banks, ub_total);
    }

    banks[from].rem_out+=amt;
    banks[to].rem_in+=amt;
    refresh(from, ub_total, banks);
    refresh(to, ub_total, banks);
}

void solve_comp(long long& nodes, const long long& node_limit, const Instance& inst, const vector<int>& pays,
    vector<bool>& chosen, vector<Bank>& banks){

    int k = (int)pays.size();

    vector<bool> curr_choices(k, 0);
    vector<bool> best_choices(k,0);
    Amount best_value = 0;

    Amount ub_total =0;
    for(int p: pays){
        int from = inst.obs[p].from;
        int to = inst.obs[p].to;

        refresh(from, ub_total, banks);
        refresh(to, ub_total, banks);
    }

    dfs(nodes, node_limit, inst, pays, 0, 0, curr_choices, best_value, best_choices, banks, ub_total);

    for(int i=0; i<k; i++){
        if(best_choices[i]){
            chosen[pays[i]]=1;
        }
    }
}

vector<int> BranchBoundSolver :: solve(const Instance& inst){

    nodes_ =0;

    int m = (int)inst.obs.size();

    vector<Bank> banks(inst.n);

    for(int i=0; i<inst.n; i++){
        banks[i].cap = inst.balance[i] + inst.credit_limit[i];
    }

    for(const Obligation& o: inst.obs){
        banks[o.from].rem_out += o.amt;
        banks[o.to].rem_in += o.amt;
    }

    vector<bool> chosen(m, 0);

    for(vector<int>& pays: payment_components(inst)){
        stable_sort(pays.begin(), pays.end(), [&](const int x, const int y){
            return inst.obs[x].amt > inst.obs[y].amt;
        });
        solve_comp(nodes_, node_limit_, inst, pays, chosen, banks);
    }

    vector<int> ans;
    for(int i=0; i<m; i++){
        if(chosen[i]){
            ans.push_back(i);
        }
    }

    return ans;

}