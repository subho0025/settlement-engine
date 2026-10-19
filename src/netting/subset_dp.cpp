#include "netting/subset_dp.hpp"
#include "core/net_balance.hpp"

using namespace std;

// size of b is capped at 22 otherwise it may overflow(2^k·k) and crash
static const int MAX_K = 22;

vector<vector<int>> dp_max_groups(const vector<Amount>& b){
    if(b.size()>MAX_K) throw invalid_argument("size of b must be <=22");

    __int64_t total_sum = 0;
    for(Amount val: b){
        total_sum += val;
        if(val==0) throw invalid_argument("Input value cannot be zero");
    }
    if(total_sum!=0) throw invalid_argument("The total doesnot sum to zero");

    int k = (int)b.size();

    if(k==0) return {};

    uint32_t max_mask = (1u<<k)-1;

    vector<uint8_t> dp(max_mask+1,0);
    vector<Amount> subset_sum(max_mask+1,0);
    vector<int> prev(max_mask+1, -1);

    for(uint32_t mask=1; mask<=max_mask; mask++){
        int i = __builtin_ctz(mask);
        uint32_t prev_mask = mask&(mask-1);
        subset_sum[mask] = subset_sum[prev_mask] + b[i];

        int best =-1;
        int maxi = -1;
        for(int j=0; j<k; j++){
            if(mask>>j & 1){
                uint8_t val = dp[mask ^ (1u<<j)];
                if(val>=best){
                    best = val;
                    maxi = j;
                }
            }
        }

        dp[mask] = best + (subset_sum[mask]==0 ? 1: 0);
        prev[mask] = maxi;
    }

    vector<int> order;
    uint32_t mask = max_mask;
    while(mask != 0){
        int i = prev[mask];
        order.push_back(i);
        mask ^= (1u << i);
    }
    reverse(order.begin(), order.end());

    vector<vector<int>> groups;
    int i=0;
    while(i<(int)order.size()){
        vector<int> curr;
        int sum =0;
        while(i<(int)order.size()){
            curr.push_back(order[i]);
            sum += b[order[i]];
            i++;
            if(sum==0) break;
        }
        groups.push_back(curr);
    }

    return groups;
}

string SubsetDpSolver::name() const {return "exact_dp";}

Plan SubsetDpSolver::solve(const Instance& inst){

    vector<Amount> bal = net_balance(inst);

    // The zero balance parties are ignored
    vector<int> parties;
    vector<Amount> b;
    for(int i = 0; i < inst.n; i++){
        if(bal[i] != 0){
            parties.push_back(i);
            b.push_back(bal[i]);
        }
    }

    vector<vector<int>> groups = dp_max_groups(b);

    // The first one in each group is choosen as the hub

    Plan plan;
    for(vector<int>& group: groups){
        int hub = parties[group[0]];
        for(int j=1; j<(int)group.size(); j++){
            int p = parties[group[j]];
            Transfer t;
            if(bal[p]>0){
                t.from = hub;
                t.to = p;
                t.amt = bal[p];
            }else{
                t.from = p;
                t.to = hub;
                t.amt = -bal[p];
            }
            plan.transfers.push_back(t);
        }
    }

    return plan;
}