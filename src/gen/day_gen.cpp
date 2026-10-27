#include "gen/day_gen.hpp"
#include "core/rng.hpp"

using namespace std;

static const Amount MAX_PAY = 1000000000000LL;

static void need(bool ok, const string& msg){
    if(!ok) throw invalid_argument(msg);
}


static int pick(Rng& rng, const vector<long long>& cum){
    long long r = rng.uniform(0, cum.back() - 1);
    return (int)(upper_bound(cum.begin(), cum.end(), r) - cum.begin());
}

static vector<long long> prefix_sums(const vector<long long>& w){
    vector<long long> c;
    long long acc = 0;
    for(long long x : w){
        acc += x;
        c.push_back(acc);
    }
    return c;
}

static Amount pow10(int e){
    Amount r = 1;
    while(e-- > 0) r *= 10;
    return r;
}

static int digits_minus_one(Amount x){
    int d = 0;
    while(x >= 10){
        x /= 10;
        d++;
    }
    return d;
}

static void check_params(const DayParams& p){

    need(p.n >= 2 && p.n <= 100000, "banks must be in [2, 100000]");
    need(p.m >= 0, "payments must be >= 0");
    need(p.length >= 1, "length must be >= 1");
    need(p.concentration >= 0 && p.concentration <= 3, "concentration must be in [0, 3]");
    need(p.amount_min >= 1, "amount_min must be >= 1");
    need(p.amount_min <= p.amount_max, "amount_min must be <= amount_max");
    need(p.amount_max <= MAX_PAY, "amount_max must be <= 1000000000000");
    need(p.large_threshold >= 1, "large_threshold must be >= 1");
    need(p.decade_permille >= 0 && p.decade_permille <= 1000, "decade_permille must be in [0, 1000]");
    need(p.round_permille >= 0 && p.round_permille <= 1000, "round_permille must be in [0, 1000]");
    need(p.deadline_permille >= 0 && p.deadline_permille <= 1000, "deadline_permille must be in [0, 1000]");
    need(p.reciprocal_permille >= 0 && p.reciprocal_permille <= 1000, "reciprocal_permille must be in [0, 1000]");
    need(p.liquidity_permille >= 0, "liquidity_permille must be >= 0");
    need(p.credit_permille >= 0, "credit_permille must be >= 0");

    for(const auto& [name, pr] : {pair<const char*, const vector<int>*>{"volume_profile", &p.volume_profile},
                                  pair<const char*, const vector<int>*>{"value_profile", &p.value_profile}}){

        need(!pr->empty(), string(name) + " must not be empty");
        need((long long)pr->size() <= p.length, string(name) + " has more slices than length");
        long long sum = 0;
        for(int w : *pr){
            need(w >= 0, string(name) + " weights must be >= 0");
            sum += w;
        }

        need(sum > 0, string(name) + " weights must not all be 0");
    }

    need(p.deadline_min >= 0, "deadline_min must be >= 0");
    need(p.deadline_min <= p.deadline_max, "deadline_min must be <= deadline_max");
    need(p.reciprocal_lag >= 0, "reciprocal_lag must be >= 0");
}

Day generate_day(const DayParams& p){

    check_params(p);

    Rng rng(p.seed);
    const Time length = p.length;

    vector<long long> bank_w(p.n);
    for(int i = 0; i < p.n; i++){
        long long w = 1000000000000LL;
        for(int k = 0; k < p.concentration; k++) w /= (i + 1);
        bank_w[i] = max<long long>(1, w);
    }
    vector<long long> bank_cum = prefix_sums(bank_w);

    int dlo = digits_minus_one(p.amount_min);
    int dhi = digits_minus_one(p.amount_max);
    vector<long long> dec_w;
    long long w = 1000000000LL;

    for(int dd = dlo; dd <= dhi; dd++){
        dec_w.push_back(w);
        w = w * p.decade_permille / 1000;
    }
    vector<long long> dec_cum = prefix_sums(dec_w);

    vector<long long> vol_cum = prefix_sums(vector<long long>(p.volume_profile.begin(), p.volume_profile.end()));
    vector<long long> val_cum = prefix_sums(vector<long long>(p.value_profile.begin(), p.value_profile.end()));

    auto draw_amount = [&]() -> Amount {
        int dd = dlo + pick(rng, dec_cum);
        Amount lo = max(p.amount_min, pow10(dd));
        Amount hi = min(p.amount_max, dd >= 12 ? MAX_PAY : pow10(dd + 1) - 1);
        Amount x = rng.uniform(lo, hi);
        if(rng.uniform(0, 999) < p.round_permille){
            Amount unit = pow10(max(0, dd - 1));
            x = x / unit * unit;
            if(x < p.amount_min) x += unit;
            x = min(x, p.amount_max);
        }

        return x;
    };

    auto draw_time = [&](Amount amt) -> Time {
        bool large = amt >= p.large_threshold;
        const vector<long long>& cum = large ? val_cum : vol_cum;
        int k = (int)cum.size();
        int s = pick(rng, cum);
        Time a = length * s / k, b = length * (s + 1) / k;
        return rng.uniform(a, b - 1);
    };

    auto draw_deadline = [&](Time t) -> Time {
        if(rng.uniform(0, 999) >= p.deadline_permille) return NO_DEADLINE;
        return t + rng.uniform(p.deadline_min, p.deadline_max);
    };

    vector<DayPayment> pays;
    while((int)pays.size() < p.m){
        DayPayment x;
        x.amt = draw_amount();
        x.t = draw_time(x.amt);
        x.from = pick(rng, bank_cum);
        do{
            x.to = pick(rng, bank_cum);
        }while(x.to == x.from);
        x.deadline = draw_deadline(x.t);
        pays.push_back(x);

        if((int)pays.size() < p.m && rng.uniform(0, 999) < p.reciprocal_permille){
            DayPayment y;
            y.from = x.to;
            y.to = x.from;
            y.t = min<Time>(length, x.t + rng.uniform(0, p.reciprocal_lag));
            y.amt = rng.uniform(x.amt - x.amt / 10, x.amt + x.amt / 10);
            y.amt = max(p.amount_min, min(p.amount_max, y.amt));
            y.deadline = draw_deadline(y.t);
            pays.push_back(y);
        }
    }
    stable_sort(pays.begin(), pays.end(), [](const DayPayment& a, const DayPayment& b){ return a.t < b.t; });

    Day d;
    d.n = p.n;
    d.end = length;
    d.payments = pays;

    vector<__int128> out(p.n, 0);
    for(const DayPayment& x : d.payments) out[x.from] += x.amt;
    for(int i = 0; i < p.n; i++){
        d.balance.push_back((Amount)(out[i] * p.liquidity_permille / 1000));
        d.credit_limit.push_back((Amount)(out[i] * p.credit_permille / 1000));
    }
    return d;
}

static string trim(const string& s){
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == string::npos ? "" : s.substr(a, b - a + 1);
}

static long long parse_int(const string& v){
    size_t i = (v.size() > 0 && v[0] == '-') ? 1 : 0;
    if(i == v.size() || v.size() - i > 18) throw runtime_error("bad number '" + v + "'");
    for(size_t k = i; k < v.size(); k++) if(v[k] < '0' || v[k] > '9') throw runtime_error("bad number '" + v + "'");
    return stoll(v);
}

static vector<int> parse_list(const string& v){
    vector<int> out;
    string cur;
    for(size_t i = 0; i <= v.size(); i++){
        if(i == v.size() || v[i] == ','){
            long long x = parse_int(trim(cur));
            if(x < INT_MIN || x > INT_MAX) throw runtime_error("number out of range '" + cur + "'");
            out.push_back((int)x);
            cur.clear();
        }else cur += v[i];
    }
    return out;
}

void set_day_param(DayParams& p, const string& key, const string& value){
    auto as_int = [&]() -> int {
        long long x = parse_int(value);
        if(x < INT_MIN || x > INT_MAX) throw runtime_error("number out of range '" + value + "'");
        return (int)x;
    };
    if(value.empty()) throw runtime_error("empty value for '" + key + "'");
    if(key == "seed"){
        if(value[0] == '-') throw runtime_error("seed must be >= 0");
        p.seed = (uint64_t)parse_int(value);
    }
    else if(key == "banks") p.n = as_int();
    else if(key == "payments") p.m = as_int();
    else if(key == "length") p.length = parse_int(value);
    else if(key == "concentration") p.concentration = as_int();
    else if(key == "amount_min") p.amount_min = parse_int(value);
    else if(key == "amount_max") p.amount_max = parse_int(value);
    else if(key == "large_threshold") p.large_threshold = parse_int(value);
    else if(key == "decade_permille") p.decade_permille = as_int();
    else if(key == "round_permille") p.round_permille = as_int();
    else if(key == "volume_profile") p.volume_profile = parse_list(value);
    else if(key == "value_profile") p.value_profile = parse_list(value);
    else if(key == "deadline_permille") p.deadline_permille = as_int();
    else if(key == "deadline_min") p.deadline_min = parse_int(value);
    else if(key == "deadline_max") p.deadline_max = parse_int(value);
    else if(key == "reciprocal_permille") p.reciprocal_permille = as_int();
    else if(key == "reciprocal_lag") p.reciprocal_lag = parse_int(value);
    else if(key == "liquidity_permille") p.liquidity_permille = as_int();
    else if(key == "credit_permille") p.credit_permille = as_int();
    else throw runtime_error("unknown key '" + key + "'");
}

DayParams read_day_params(istream& in){
    DayParams p;
    set<string> seen;
    string line;
    int ln = 0;
    while(getline(in, line)){
        ln++;
        if(!line.empty() && line.back() == '\r') line.pop_back();
        string t = trim(line);
        if(t.empty() || t[0] == '#') continue;
        try{
            size_t eq = t.find('=');
            if(eq == string::npos) throw runtime_error("expected 'key = value'");
            string key = trim(t.substr(0, eq)), value = trim(t.substr(eq + 1));
            if(seen.count(key)) throw runtime_error("duplicate key '" + key + "'");
            set_day_param(p, key, value);
            seen.insert(key);
        }catch(const runtime_error& e){
            throw runtime_error("line " + to_string(ln) + ": " + e.what());
        }
    }
    return p;
}

void write_day_params(ostream& out, const DayParams& p){
    auto list = [](const vector<int>& v){ string s; for(size_t i = 0; i < v.size(); i++){ if(i) s += ","; s += to_string(v[i]); } return s; };
    out << "# settle day generator config v1\n"
        << "seed = " << p.seed << "\n"
        << "banks = " << p.n << "\n"
        << "payments = " << p.m << "\n"
        << "length = " << p.length << "\n"
        << "concentration = " << p.concentration << "\n"
        << "amount_min = " << p.amount_min << "\n"
        << "amount_max = " << p.amount_max << "\n"
        << "large_threshold = " << p.large_threshold << "\n"
        << "decade_permille = " << p.decade_permille << "\n"
        << "round_permille = " << p.round_permille << "\n"
        << "volume_profile = " << list(p.volume_profile) << "\n"
        << "value_profile = " << list(p.value_profile) << "\n"
        << "deadline_permille = " << p.deadline_permille << "\n"
        << "deadline_min = " << p.deadline_min << "\n"
        << "deadline_max = " << p.deadline_max << "\n"
        << "reciprocal_permille = " << p.reciprocal_permille << "\n"
        << "reciprocal_lag = " << p.reciprocal_lag << "\n"
        << "liquidity_permille = " << p.liquidity_permille << "\n"
        << "credit_permille = " << p.credit_permille << "\n";
}