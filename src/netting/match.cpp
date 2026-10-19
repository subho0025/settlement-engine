#include "netting/match.hpp"
#include "core/net_balance.hpp"
#include "netting/group_plan.hpp"

using namespace std;

static const int TRIPLE_LIMIT = 3000;

vector<vector<int>> match_groups(const vector<Amount>& b){

    __int128_t total_sum = 0;
    for(Amount val: b){
        total_sum += val;
        if(val==0) throw invalid_argument("Input value cannot be zero");
    }
    if(total_sum!=0) throw invalid_argument("The total doesnot sum to zero");

    int k = (int)b.size();

    if(k==0) return {};

    vector<pair<Amount,int>> pos;
    vector<pair<Amount,int>> neg;
    vector<pair<Amount,int>> left;
    vector<vector<int>> groups;


    for(int i=0; i<k; i++){
        if(b[i]>0){
            pos.push_back({b[i], i});
        }else{
            neg.push_back({b[i], i});
        }
    }

    sort(pos.begin(), pos.end(), [](pair<Amount,int> x, pair<Amount,int> y){
        if(x.first<y.first){
            return true;
        }else if(x.first == y.first && x.second<y.second){
            return true;
        }
        return false;
    });

    sort(neg.begin(), neg.end(), [](pair<Amount,int> x, pair<Amount,int> y){
        if(x.first>y.first){
            return true;
        }else if(x.first == y.first && x.second<y.second){
            return true;
        }
        return false;
    });

    int p1=0;
    int p2=0;

    while(p1<(int)pos.size() && p2<(int)neg.size()){
        if(pos[p1].first==-neg[p2].first){
            groups.push_back({pos[p1].second, neg[p2].second});
            p1++;
            p2++;
        }else if(pos[p1].first<-neg[p2].first){
            left.push_back(pos[p1]);
            p1++;
        }else{
            left.push_back(neg[p2]);
            p2++;
        }
    }
    while(p1<(int)pos.size()){
        left.push_back(pos[p1]);
        p1++;
    }
    while(p2<(int)neg.size()){
        left.push_back(neg[p2]);
        p2++;
    }

    sort(left.begin(), left.end());

    int r = left.size();
    vector<bool> used(r,0);

    if(r<=TRIPLE_LIMIT){
        for(int i=0; i<r; i++){
            if(left[i].first>0) break;
            if(used[i]) continue;
            int low = i+1;
            int high = r-1;

            while(low<high){
                if(used[low]){
                    low++;
                }else if(used[high]){
                    high--;
                }else{
                    __int128_t triple_sum = (__int128_t)left[i].first+left[low].first+left[high].first;
                    if(triple_sum==0){
                        groups.push_back({left[i].second, left[low].second, left[high].second});
                        used[i]=1;
                        used[low]=1;
                        used[high]=1;
                        break;
                    }else if(triple_sum<0){
                        low++;
                    }else{
                        high--;
                    }
                }
            }
        }
    }

    vector<int> rest;
    for(int i=0; i<r; i++){
        if(!used[i]){
            rest.push_back(left[i].second);
        }
    }
    if(!rest.empty()){
        groups.push_back(rest);
    }

    return groups;

}

string MatchSolver::name() const {return "match";}

Plan MatchSolver::solve(const Instance& inst){

    vector<Amount> bal = net_balance(inst);

    vector<int> parties;
    vector<Amount> b;
    for(int i = 0; i < inst.n; i++){
        if(bal[i] != 0){
            parties.push_back(i);
            b.push_back(bal[i]);
        }
    }

    vector<vector<int>> groups = match_groups(b);

    return plan_from_groups(bal, parties, groups);

}
