#include "netting/mitm.hpp"
#include "core/net_balance.hpp"
#include "netting/group_plan.hpp"
#include "netting/match.hpp"

using namespace std;

static const int MAX_S = 40;

struct Entry{
    __int128 sum;
    uint32_t mask;
    int size;
};

vector<int> mitm_zero_subset(const vector<Amount>& v){

    int s = (int)v.size();

    if(s>MAX_S) throw invalid_argument("size of v must be <=40");

    for(Amount val: v){
        if(val==0) throw invalid_argument("Input value cannot be zero");
    }
    if(s<2) return {};

    int ha = s/2;
    int hb = s-ha;

    vector<Entry> halfA(1<<ha);
    vector<Entry> halfB(1<<hb);

    halfA[0] = {0, 0, 0};
    for(int i=1; i<(1<<ha); i++){
        halfA[i].mask = i;
        halfA[i].sum = halfA[i&(i-1)].sum + v[__builtin_ctz(i)];
        halfA[i].size = halfA[i&(i-1)].size +1;
    }

    halfB[0] = {0, 0, 0};
    for(int i=1; i<(1<<hb); i++){
        halfB[i].mask = i;
        halfB[i].sum = halfB[i&(i-1)].sum + v[ha+__builtin_ctz(i)];
        halfB[i].size = halfB[i&(i-1)].size +1;
    }

    sort(halfA.begin(), halfA.end(), [](Entry& x, Entry& y){
        if(x.sum<y.sum) return true;
        else if(x.sum==y.sum && x.size<y.size) return true;
        return false;
    });

    sort(halfB.begin(), halfB.end(), [](Entry& x, Entry& y){
        if(x.sum<y.sum) return true;
        else if(x.sum==y.sum && x.size<y.size) return true;
        return false;
    });

    int minSize = INT_MAX;
    uint32_t bestA=0;
    uint32_t bestB=0;

    //first entry of halfA and halfB with sum 0 is by deafult the empty subset
    vector<int> ans;

    for(Entry& e : halfA){
        if(e.sum == 0 && e.mask != 0){
            if(e.size < minSize){
                minSize = e.size;
                bestA = e.mask;
                bestB = 0;
            }
            break;
        }
    }
    for(Entry& e : halfB){
        if(e.sum == 0 && e.mask != 0){
            if(e.size < minSize){
                minSize = e.size;
                bestA = 0;
                bestB = e.mask;
            }
            break;
        }
    }


    vector<Entry> ua;
    vector<Entry> ub;

    for(Entry& e: halfA){
        if(e.sum !=0 && (ua.empty() || ua.back().sum != e.sum)){
            ua.push_back(e);
        }
    }
    for(Entry& e: halfB){
        if(e.sum !=0 && (ub.empty() || ub.back().sum != e.sum)){
            ub.push_back(e);
        }
    }


    int p1=0;
    int p2=(int)ub.size()-1;

    while(p1<(int)ua.size() && p2>=0){
        __int128_t currSum = ua[p1].sum + ub[p2].sum;
        int currSize = ua[p1].size + ub[p2].size;
        if(currSum==0){
            if(currSize<minSize){
                bestA = ua[p1].mask;
                bestB = ub[p2].mask;
                minSize = currSize;
            }
            p1++;
            p2--;
        }else if(currSum>0){
            p2--;
        }else{
            p1++;
        }
    }

    for(int i=0; i<ha; i++){
        if((1<<i) & bestA){
            ans.push_back(i);
        }
    }
    for(int i=0; i<hb; i++){
        if((1<<i) & bestB){
            ans.push_back(ha+i);
        }
    }

    return ans;
}

vector<vector<int>> refine_groups(const vector<Amount>& b, vector<vector<int>> groups){

    vector<vector<int>> refinedGroups;

    for(vector<int> group: groups){

        int currSize = (int)group.size();

        while(currSize>=4 && currSize<=MAX_S){

            vector<bool> used(currSize, 0);

            vector<Amount> v;
            for(int p: group){
                v.push_back(b[p]);
            }
            vector<int> subgroup = mitm_zero_subset(v);
            if(subgroup.size()==0 || (int)subgroup.size()==currSize) break;

            vector<int> left;
            vector<int> done;

            for(int p: subgroup){
                used[p]=1;
            }

            for(int p=0; p<currSize; p++){
                if(!used[p]){
                    left.push_back(group[p]);
                } else {
                    done.push_back(group[p]);
                }
            }

            refinedGroups.push_back(done);
            group = left;
            currSize = (int)group.size();
        }
        if(group.size()!=0) refinedGroups.push_back(group);
    }

    return refinedGroups;
}

string MitmSolver :: name() const {return "match_mitm";};

Plan MitmSolver:: solve(const Instance& inst){

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

    vector<vector<int>> refinedGroups = refine_groups(b, groups);

    return plan_from_groups(bal, parties, refinedGroups);

}