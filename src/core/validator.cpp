#include "core/validator.hpp"
#include "core/net_balance.hpp"

static const Amount MAX_TRANSFER_AMT = 1000000000000000000LL;

ValidationResult validate_netting(const Instance& inst, const Plan& plan){

    ValidationResult res;

    res.transfers = plan.transfers.size();

    auto fail = [&](const string& msg){
    res.ok = false;
    res.error = msg;
    return res;
    };

    vector<__int128> effect(inst.n, 0);

    for(const Transfer& transfer: plan.transfers){

        if(transfer.to<0 || transfer.to>=inst.n) return fail("'to' out of range [0, n)");

        if(transfer.from<0 || transfer.from>=inst.n) return fail("'from' out of range [0, n)");

        if(transfer.from == transfer.to) return fail("'from' and 'to' must differ");

        if(transfer.amt<1 || transfer.amt >MAX_TRANSFER_AMT) return fail("amt out of range [1, 1e18]");

        Amount next;
        if(__builtin_add_overflow(res.total_moved, transfer.amt, &next))  return fail("total moved overflows int64");

        res.total_moved = next;
        effect[transfer.to]   += transfer.amt;
        effect[transfer.from] -= transfer.amt;
    }

    vector<Amount> bal = net_balance(inst);
    for(int i = 0; i < inst.n; i++){
        if(effect[i] != (__int128)bal[i]){
            return fail("party " + to_string(i) + ": plan effect differs from expected net balance "+ to_string(bal[i]));
        }
    }


    res.ok=1;

    return res;
    
}

GridlockCheck validate_gridlock(const Instance& inst, const vector<int>& selected){

    GridlockCheck res;

    auto fail = [&](const string& msg){
    res.ok = false;
    res.error = msg;
    return res;
    };

    if((int)inst.balance.size()!=inst.n) return fail("'balance' is not of size n");
    if((int)inst.credit_limit.size()!=inst.n) return fail("'credit limit' is not of size n");

    vector<__int128_t> pos(inst.n);
    for(int i = 0; i < inst.n; i++){
        pos[i] = (__int128_t)inst.balance[i] + inst.credit_limit[i];
    }

    long long prev = -1;
    for(size_t k = 0; k < selected.size(); k++){
        long long idx = selected[k];
        if(idx < 0 || idx >= (long long)inst.obs.size()){
            return fail("selected[" + to_string(k) + "]: index out of range");
        }
        if(idx <= prev){
            return fail("selected[" + to_string(k) + "]: indices must be strictly increasing");
        }
        prev = idx;

        const Obligation& o = inst.obs[idx];
        pos[o.to]   += o.amt;
        pos[o.from] -= o.amt;
        res.settled_value += o.amt;
        res.settled_count++;
    }

    for(int i = 0; i < inst.n; i++){
        if(pos[i] < 0){
            return fail("bank " + to_string(i) + ": ends " + to_string((long long)(-pos[i])) + " below its credit limit");
        }
    }


    res.ok=1;

    return res;
    
}