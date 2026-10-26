#include "stream/engine.hpp"
#include "core/instance.hpp"
#include "core/validator.hpp"

using namespace std;

SettlementEngine :: SettlementEngine(const vector<Amount>& balance, const vector<Amount>& credit_limit, GridlockSolver& solver, EngineConfig cfg)
                    : pos_(balance), credit_(credit_limit), solver_(solver), cfg_(cfg) {

    n_ = (int)pos_.size();
    if(n_<1){
        throw invalid_argument("The given number of banks must be atleast 1");
    }
    if(n_ != (int)credit_.size()){
        throw invalid_argument("The given credit limit and balance sizes differ");
    }
    for(int i=0; i<n_; i++){
        if(pos_[i]<0){
            throw invalid_argument("The balance of bank " + to_string(i) + " is negative");
        }
        if(credit_[i]<0){
            throw invalid_argument("The credit limit of bank " + to_string(i) + " is negative");
        }
    }
    if(cfg_.batch_interval<0){
        throw invalid_argument("batch_interval in the config cannot be negative");
    }
    if(cfg_.value_trigger<0){
        throw invalid_argument("value_trigger in the config cannot be negative");
    }
    by_payer_.resize(n_);
    next_tick_ = cfg_.batch_interval;
}


int SettlementEngine :: submit(int from, int to, Amount amt, Time t, Time deadline){

    if(closed_){
        throw logic_error("The engine is closed");
    }
    if(from<0 || from>=n_ || to<0 || to>=n_){
        throw invalid_argument("from / to must be in the range [0, " + to_string(n_) + ")");
    }
    if(from==to){
        throw invalid_argument("A bank cannot pay itself");
    }
    if(amt<=0){
        throw invalid_argument("The amount must be positive");
    }
    if(t<now_){
        throw invalid_argument("Time cannot go backwards: t = " + to_string(t) + " < now = " + to_string(now_));
    }
    if(deadline<t){
        throw invalid_argument("The deadline cannot be before the arrival time");
    }

    advance_to(t);

    PaymentRecord record;
    record.from = from;
    record.to = to;
    record.amt = amt;
    record.arrival = t;
    record.deadline = deadline;

    int id = rec_.size();
    rec_.push_back(record);
    
    if(pos_[from]+credit_[from]>=amt){
        settle(id, How::Arrival);
        recycle(to);
    }else{
        queue_.insert(id);
        by_payer_[from].insert(id);
        deadlines_.insert({deadline, id});
        queued_value_+=amt;
        
        if((cfg_.queue_trigger>0 && queue_size()>=cfg_.queue_trigger) 
            || (cfg_.value_trigger>0 && queued_value_>=cfg_.value_trigger)){
                run_batch();
            }
    }

    return id;
}

void SettlementEngine :: advance_to(Time t){

    if(closed_){
        throw logic_error("The engine is closed");
    }
    if(t<now_){
        throw invalid_argument("Time cannot go backwards: t = " + to_string(t) + " < now = " + to_string(now_));
    }

    while(1){
        if(queue_size()==0){
            if(cfg_.batch_interval>0 && next_tick_<=t){
                next_tick_ = ((t/cfg_.batch_interval)+1) * cfg_.batch_interval;
            }
            break;
        }

        Time nextT;
        if(cfg_.batch_interval>0){
            nextT = next_tick_;
        }else{
            nextT = NO_DEADLINE;
        }

        Time dl = deadlines_.begin()->first;

        Time tt = min(nextT, dl);
        if(tt>t) break;

        now_ = tt;

        if(tt == nextT){
            next_tick_ += cfg_.batch_interval;
        }

        if(tt == nextT || (cfg_.deadline_batch && dl==tt)){
            run_batch();
        }

        while(!deadlines_.empty() && deadlines_.begin()->first<=tt){
            int id = deadlines_.begin()->second;
            dequeue(id);
            rec_[id].status = Status::Expired;
            rec_[id].done_at = tt;
        }
    }

    now_ = t;
}

void SettlementEngine :: close(Time t){

    advance_to(t);

    run_batch();

    vector<int> left(queue_.begin(), queue_.end());
    for(int id : left){
        dequeue(id);
        rec_[id].status = Status::Unsettled;
        rec_[id].done_at = t;
    }

    closed_ = 1;

}

void SettlementEngine :: settle(int id, How how){
    
    pos_[rec_[id].from] -= rec_[id].amt;
    pos_[rec_[id].to] += rec_[id].amt;
    rec_[id].status = Status::Settled;
    rec_[id].how = how;
    rec_[id].done_at = now_;

}

void SettlementEngine :: dequeue(int id){

    queue_.erase(id);
    by_payer_[rec_[id].from].erase(id);
    deadlines_.erase({rec_[id].deadline, id});

    queued_value_-= rec_[id].amt;
}

void SettlementEngine :: recycle(int start){

    queue<int> worklist;
    vector<bool> waiting(n_,0);
    worklist.push(start);
    waiting[start]=1;

    while(!worklist.empty()){
        int b = worklist.front();
        worklist.pop();
        waiting[b]=0;
        vector<int> ids(by_payer_[b].begin(), by_payer_[b].end());
        for(int id : ids){
            if(pos_[b]+credit_[b]>=rec_[id].amt){
                dequeue(id);
                settle(id, How::Recycle);
                int to = rec_[id].to;
                if(!waiting[to]){
                    waiting[to]=1;
                    worklist.push(to);
                }
            }
        }
    }

}

void SettlementEngine :: run_batch(){

    if(queue_size()==0) return;

    Instance inst;
    inst.n = n_;
    inst.balance.assign(n_,0);
    inst.credit_limit.assign(n_,0);
    inst.obs.clear();

    for(int i=0; i<n_; i++){
        inst.balance[i] = pos_[i]+credit_[i];
        inst.credit_limit[i] = 0;
    }

    vector<int> ids;

    for(int id: queue_){
        Obligation o;
        o.from = rec_[id].from;
        o.to = rec_[id].to;
        o.amt = rec_[id].amt;
        o.deadline = rec_[id].deadline;

        inst.obs.push_back(o);
        ids.push_back(id);
    }

    vector<int> sel = solver_.solve(inst);

    if(!validate_gridlock(inst, sel).ok){
        throw logic_error("Logic error in solver");
    }

    for(int k : sel){
        dequeue(ids[k]);
        settle(ids[k], How::Batch);
    }

    batches_++;

}


Time SettlementEngine :: now() const{
    return now_;
}

bool SettlementEngine :: closed() const{
    return closed_;
}

const vector<Amount>& SettlementEngine :: positions() const{
    return pos_;
}

const vector<PaymentRecord>& SettlementEngine :: payments() const{
    return rec_;
}

size_t SettlementEngine :: queue_size() const{
    return queue_.size();
}

Amount SettlementEngine :: queued_value() const{
    return queued_value_;
}

int SettlementEngine :: batches_run() const{
    return batches_;
}

EngineStats SettlementEngine :: stats() const{

    EngineStats s;
    s.submitted = rec_.size();
    s.batches = batches_;

    vector<Time> delays;

    for(const PaymentRecord& r: rec_){
        if(r.status == Status::Settled){
            s.settled++;
            s.settled_value += r.amt;
            delays.push_back(r.done_at - r.arrival);
            if(r.how == How::Arrival){
                s.by_arrival++;
            }else if(r.how == How::Recycle){
                s.by_recycle++;
            }else{
                s.by_batch++;
            }
        }else if(r.status == Status::Expired){
            s.expired++;
            s.expired_value += r.amt;
        }else if(r.status == Status::Unsettled){
            s.unsettled++;
            s.unsettled_value += r.amt;
        }else{
            s.queued++;
            s.queued_value += r.amt;
        }
    }

    if(!delays.empty()){
        sort(delays.begin(), delays.end());
        size_t N = delays.size();
        // nearest rank: the p-th percentile is the k-th smallest, k = ceil(p * N / 100)
        auto pct = [&](size_t p){
            size_t k = (p * N + 99) / 100;
            return delays[k - 1];
        };
        s.delay_p50 = pct(50);
        s.delay_p95 = pct(95);
        s.delay_p99 = pct(99);
        s.delay_max = delays.back();
    }

    return s;
}