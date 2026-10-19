#include "netting/group_plan.hpp"

using namespace std;

Plan plan_from_groups(const vector<Amount>& bal, const vector<int>& parties, const vector<vector<int>>& groups){
    Plan plan;
    for(const vector<int>& group: groups){
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