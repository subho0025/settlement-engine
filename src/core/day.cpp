#include "core/day.hpp"

using namespace std;

static const Amount MAX_PAY = 1000000000000LL;
static const Amount MAX_BAL = 1000000000000000LL;

void write_day(ostream& out, const Day& d){

    out << "# settle day file v1\n";
    out << "day," << d.end << "\n";

    for(int i = 0; i < d.n; i++){
        out << "bank," << i << "," << d.balance[i] << "," << d.credit_limit[i] << "\n";
    }

    for(const auto& x : d.payments){
        out << "pay," << x.t << "," << x.from << "," << x.to << "," << x.amt << ",";
        if(x.deadline != NO_DEADLINE){
            out << x.deadline;
        }
        out << "\n";
    }

}

static vector<string> split(const string& s){

    vector<string> f; string cur;

    for(char c : s){
        if(c == ','){ 
            f.push_back(cur);
            cur.clear();
        }else{
            cur += c;
        }
    }

    f.push_back(cur);
    return f;

}

static long long num(const string& s){

    if(s.empty() || s.size() > 18 || !all_of(s.begin(), s.end(), [](char c){
        return c >= '0' && c <= '9';
    })){
        throw runtime_error("bad number '" + s + "'");
    }

    return stoll(s);
}

Day read_day(istream& in){

    Day d;
    bool have_day = false, have_pay = false;
    Time last = 0;
    string line;
    int ln = 0;

    while(getline(in, line)){
        ln++;
        if(!line.empty() && line.back() == '\r'){
            line.pop_back();
        }
        if(line.empty() || line[0] == '#'){
            continue;
        }

        try{
            auto bad = [](const string& m){
                throw runtime_error(m);
            };

            vector<string> f = split(line);

            if(f[0] == "day"){
                if(have_day) bad("second day line");
                if(f.size() != 2) bad("day needs 1 field");
                d.end = num(f[1]); have_day = true;
            }else if(f[0] == "bank"){
                if(!have_day) bad("bank before day");
                if(have_pay) bad("bank after payments");
                if(f.size() != 4) bad("bank needs 3 fields");
                if(num(f[1]) != d.n) bad("bank ids must be 0, 1, 2, ... in order");
                if(d.n >= 100000) bad("too many banks");
                Amount b = num(f[2]), c = num(f[3]);
                if(b > MAX_BAL || c > MAX_BAL) bad("balance/credit too large");
                d.n++; d.balance.push_back(b); d.credit_limit.push_back(c);
            }else if(f[0] == "pay"){
                if(d.n == 0) bad("payment before banks");
                if(f.size() != 6) bad("pay needs 5 fields");
                Time t = num(f[1]);
                if(t > d.end) bad("time after the end of the day");
                if(have_pay && t < last) bad("time goes backwards");
                long long a = num(f[2]), b = num(f[3]);
                if(a >= d.n || b >= d.n) bad("unknown bank");
                if(a == b) bad("bank pays itself");
                Amount amt = num(f[4]);
                if(amt < 1 || amt > MAX_PAY) bad("amount out of range");
                DayPayment x; x.from = (int)a; x.to = (int)b; x.amt = amt; x.t = t;
                if(!f[5].empty()){ Time dl = num(f[5]); if(dl < t) bad("deadline before time"); x.deadline = dl; }
                d.payments.push_back(x); last = t; have_pay = true;
            }else{
                bad("unknown record '" + f[0] + "'");
            }
        }catch(const runtime_error& e){
            throw runtime_error("line " + to_string(ln) + ": " + e.what());
        }
    }

    if(!have_day) throw runtime_error("no day line");
    if(d.n == 0) throw runtime_error("no banks");
    return d;
    
}