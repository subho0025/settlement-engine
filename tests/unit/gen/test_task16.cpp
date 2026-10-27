#include "core/day.hpp"
#include "gen/day_gen.hpp"
#include "stream/replay.hpp"
#include "gridlock/drop_violators.hpp"
#include "gridlock/maxflow_relax.hpp"

using namespace std;

// Contract used by this file. Amounts, times and bank ids are plain integers.
//
//   includes/core/day.hpp, src/core/day.cpp        Day, DayPayment, write_day, read_day
//   includes/gen/day_gen.hpp, src/gen/day_gen.cpp  DayParams, generate_day, the generator config file
//   includes/stream/replay.hpp, src/stream/replay.cpp   run_day
//
//   Day generate_day(const DayParams& p)       deterministic in p (p.seed included); see the task for the exact steps.
//       Throws invalid_argument on bad parameters (see section B).
//
//   void write_day(ostream&, const Day&)       CSV, one record per line:
//       # settle day file v1
//       day,<end>
//       bank,<id>,<balance>,<credit>                         ids 0, 1, 2, ... in order
//       pay,<t>,<from>,<to>,<amount>,<deadline or empty>
//   Day read_day(istream&)                     runtime_error("line N: ...") on anything malformed; ignores blank lines,
//       lines starting with '#', and a trailing '\r'. Every number is plain digits (no sign, no spaces, at most 18).
//       Rules: one day line first; then banks (ids in order, at most 100000, balance/credit <= 1e15); then payments
//       (t <= end and never decreasing, from/to existing and different, amount in [1, 1e12], deadline empty or >= t).
//       At least one bank.
//
//   Generator config ("key = value" per line; '#' comments, blank lines, spaces around '=' and a trailing '\r' are fine):
//   void set_day_param(DayParams&, key, value)  one setting; runtime_error for an unknown key, an empty or malformed value
//       keys: seed banks payments length concentration amount_min amount_max large_threshold decade_permille
//             round_permille volume_profile value_profile deadline_permille deadline_min deadline_max
//             reciprocal_permille reciprocal_lag liquidity_permille credit_permille
//       values: integers (an optional '-' and 1-18 digits; seed >= 0; int-sized keys range-checked), profiles as
//               comma-separated integers. Ranges are NOT checked here (generate_day does that).
//   DayParams read_day_params(istream&)         starts from the defaults; runtime_error("line N: ...") on a line without
//       '=', an unknown key, a duplicate key, or a bad value
//   void write_day_params(ostream&, const DayParams&)   every key, one per line, in the order listed above, after the
//       header line "# settle day generator config v1"; read_day_params gives the same DayParams back
//
//   EngineStats run_day(const Day&, GridlockSolver&, EngineConfig cfg = {})
//       a fresh SettlementEngine over the day's balances, every payment submitted in order, close(end); stats()

static int failures = 0;

#define CHECK(cond) \
    do { if(!(cond)) { cerr << __FILE__ << ":" << __LINE__ << "  CHECK failed: " #cond "\n"; failures++; } } while(0)

// ------------------------------------------------------------------ helpers
template <class E, class F>
static bool throws(F f){
    try{
        f();
    }catch(const E&){
        return true;
    }catch(...){
        return false;
    }
    return false;
}

static bool same_day(const Day& a, const Day& b){
    if(a.n != b.n || a.end != b.end) return false;
    if(a.balance != b.balance || a.credit_limit != b.credit_limit || a.payments.size() != b.payments.size()) return false;
    for(size_t i = 0; i < a.payments.size(); i++){
        const DayPayment& x = a.payments[i];
        const DayPayment& y = b.payments[i];
        if(x.from != y.from || x.to != y.to || x.amt != y.amt || x.t != y.t || x.deadline != y.deadline) return false;
    }
    return true;
}

static bool read_throws(const string& text){
    istringstream in(text);
    return throws<runtime_error>([&]{ read_day(in); });
}

static Day read_text(const string& text){
    istringstream in(text);
    return read_day(in);
}

static bool stats_equal(const EngineStats& a, const EngineStats& b){
    return a.submitted == b.submitted && a.settled == b.settled && a.by_arrival == b.by_arrival && a.by_recycle == b.by_recycle &&
           a.by_batch == b.by_batch && a.expired == b.expired && a.unsettled == b.unsettled && a.queued == b.queued &&
           a.settled_value == b.settled_value && a.expired_value == b.expired_value && a.unsettled_value == b.unsettled_value &&
           a.batches == b.batches && a.delay_p50 == b.delay_p50 && a.delay_p95 == b.delay_p95 && a.delay_p99 == b.delay_p99 &&
           a.delay_max == b.delay_max;
}

static int digits_of(Amount x){
    int d = 0;
    while(x > 0){ x /= 10; d++; }
    return d;
}

// ------------------------------------------------------------------ A. file format
static void test_format(){
    Day d;
    d.n = 3;
    d.end = 28800;
    d.balance = {5000000, 0, 75};
    d.credit_limit = {0, 2000000, 0};
    DayPayment a, b;
    a.from = 0; a.to = 1; a.amt = 3000000; a.t = 5;
    b.from = 2; b.to = 0; b.amt = 1000000000000LL; b.t = 5; b.deadline = 3600;
    d.payments = {a, b};

    ostringstream out;
    write_day(out, d);
    CHECK(out.str() ==
          "# settle day file v1\n"
          "day,28800\n"
          "bank,0,5000000,0\n"
          "bank,1,0,2000000\n"
          "bank,2,75,0\n"
          "pay,5,0,1,3000000,\n"
          "pay,5,2,0,1000000000000,3600\n");
    CHECK(same_day(read_text(out.str()), d));

    {   // comments, blank lines, Windows line endings, deadline == t, t == end, t == 0
        Day z = read_text("# hello\r\n\r\nday,10\r\n# banks\nbank,0,1,0\nbank,1,0,0\n\npay,0,1,0,7,\npay,10,0,1,1,10\n");
        CHECK(z.n == 2 && z.end == 10 && z.payments.size() == 2);
        CHECK(z.payments[1].t == 10 && z.payments[1].deadline == 10 && z.payments[1].amt == 1);
        CHECK(z.payments[0].deadline == NO_DEADLINE);
        Day w = read_text("day,0\nbank,0,0,0\n");
        CHECK(w.n == 1 && w.payments.empty() && w.end == 0);
    }
    {   // the error says which line
        string msg;
        try{ read_text("day,100\nbank,0,1,0\nbank,5,1,0\n"); }catch(const runtime_error& e){ msg = e.what(); }
        CHECK(msg.rfind("line 3", 0) == 0);
    }

    const string H = "day,1000\nbank,0,100,0\nbank,1,0,0\n";
    vector<pair<const char*, string>> bad = {
        {"empty file",              ""},
        {"no banks",                "day,1000\n"},
        {"bank before day",         "bank,0,1,0\nday,1000\n"},
        {"two day lines",           "day,1000\nday,1000\nbank,0,1,0\n"},
        {"negative end",            "day,-1\nbank,0,1,0\n"},
        {"day field count",         "day,1000,2000\nbank,0,1,0\n"},
        {"bad end",                 "day,10x\nbank,0,1,0\n"},
        {"bank ids out of order",   "day,1000\nbank,1,1,0\n"},
        {"bank field count",        "day,1000\nbank,0,1\n"},
        {"negative balance",        "day,1000\nbank,0,-5,0\n"},
        {"decimal balance",         "day,1000\nbank,0,5.5,0\n"},
        {"balance too large",       "day,1000\nbank,0,1000000000000001,0\n"},
        {"credit too large",        "day,1000\nbank,0,0,1000000000000001\n"},
        {"bank after payments",     H + "pay,9,0,1,1,\nbank,2,0,0\n"},
        {"payment before banks",    "day,1000\npay,9,0,1,1,\n"},
        {"unknown bank",            H + "pay,9,0,2,1,\n"},
        {"pays itself",             H + "pay,9,0,0,1,\n"},
        {"amount zero",             H + "pay,9,0,1,0,\n"},
        {"amount too large",        H + "pay,9,0,1,1000000000001,\n"},
        {"negative amount",         H + "pay,9,0,1,-3,\n"},
        {"spaces in a number",      H + "pay,9,0,1, 5,\n"},
        {"negative time",           H + "pay,-1,0,1,1,\n"},
        {"after the end",           H + "pay,1001,0,1,1,\n"},
        {"time goes backwards",     H + "pay,9,0,1,1,\npay,8,0,1,1,\n"},
        {"deadline before time",    H + "pay,9,0,1,1,8\n"},
        {"pay field count",         H + "pay,9,0,1,1\n"},
        {"unknown record",          H + "foo,1,2\n"},
    };
    for(const auto& [label, text] : bad){
        bool t = read_throws(text);
        CHECK(t);
        if(!t) cerr << "  read_day accepted: " << label << "\n";
    }
}

// ------------------------------------------------------------------ B. generator
static void test_generator(bool fast){
    DayParams p;
    p.seed = 7;
    p.n = 200;
    p.m = fast ? 30000 : 80000;
    p.concentration = 1;
    p.amount_min = 1000;                                        // 1000 .. 99999999: exactly 5 decades
    p.amount_max = 99999999;
    p.decade_permille = 400;
    p.round_permille = 0;
    p.large_threshold = 1000000;
    p.deadline_permille = 250;
    p.reciprocal_permille = 0;
    p.liquidity_permille = 70;
    p.credit_permille = 30;

    const Time length = p.length;
    Day d = generate_day(p);
    CHECK(d.n == p.n && d.end == length && (int)d.payments.size() == p.m);

    bool shape = true;
    long long top_banks = 0, with_deadline = 0, dl_ok = 0, two_sig = 0;
    vector<long long> per_decade(5, 0);
    vector<__int128> out(p.n, 0);
    vector<Amount> amounts;
    vector<long long> small_slice(10, 0);
    long long n_small = 0, n_large = 0;
    __int128 t_small = 0, t_large = 0;
    for(size_t i = 0; i < d.payments.size(); i++){
        const DayPayment& x = d.payments[i];
        if(x.from < 0 || x.from >= p.n || x.to < 0 || x.to >= p.n || x.from == x.to) shape = false;
        if(x.t < 0 || x.t > length || (i > 0 && x.t < d.payments[i - 1].t)) shape = false;
        if(x.amt < p.amount_min || x.amt > p.amount_max) shape = false;
        int dg = digits_of(x.amt);
        if(dg >= 4 && dg <= 8) per_decade[dg - 4]++;
        Amount unit = 1;
        for(int k = 0; k < dg - 2; k++) unit *= 10;
        if(x.amt % unit == 0) two_sig++;
        if(x.from < p.n / 10) top_banks++;
        if(x.deadline != NO_DEADLINE){
            with_deadline++;
            if(x.deadline >= x.t + p.deadline_min && x.deadline <= x.t + p.deadline_max) dl_ok++;
        }
        out[x.from] += x.amt;
        amounts.push_back(x.amt);
        if(x.amt >= p.large_threshold){ n_large++; t_large += x.t; }
        else{ n_small++; t_small += x.t; small_slice[x.t * 10 / length]++; }
    }
    CHECK(shape);
    double M = (double)p.m;

    // big banks: with concentration 1 the top 10% of banks send about 60% of payments
    CHECK(top_banks / M > 0.48 && top_banks / M < 0.64);
    // heavy tail: each decade is about 40% as frequent as the one below, and the top 1% of payments carry a big share
    for(int k = 0; k + 1 < 5; k++){
        double r = (double)per_decade[k + 1] / (double)per_decade[k];
        CHECK(r > 0.34 && r < 0.46);
    }
    sort(amounts.rbegin(), amounts.rend());
    __int128 top = 0, all = 0;
    for(size_t i = 0; i < amounts.size(); i++){ all += amounts[i]; if(i < amounts.size() / 100) top += amounts[i]; }
    CHECK((double)top / (double)all > 0.15);
    CHECK(two_sig / M < 0.05);                                  // round_permille = 0: almost nothing is round
    // intraday profile: ordinary payments follow volume_profile slice by slice; large ones come later in the day
    long long wsum = 0;
    for(int w : p.volume_profile) wsum += w;
    for(int s = 0; s < 10; s++){
        double expect = (double)n_small * p.volume_profile[s] / (double)wsum;
        CHECK(fabs((double)small_slice[s] - expect) < 0.12 * expect + 50);
    }
    CHECK(n_large > 0 && (double)t_large / (double)n_large > (double)t_small / (double)n_small + 0.05 * length);
    // deadlines
    CHECK(fabs(with_deadline / M - 0.25) < 0.02);
    CHECK(dl_ok == with_deadline);
    // liquidity and credit, exactly
    for(int i = 0; i < p.n; i++){
        CHECK(d.balance[i] == (Amount)(out[i] * 70 / 1000));
        CHECK(d.credit_limit[i] == (Amount)(out[i] * 30 / 1000));
    }
    // deterministic per seed
    CHECK(same_day(generate_day(p), d));
    DayParams p8 = p;
    p8.seed = 8;
    CHECK(!same_day(generate_day(p8), d));

    {   // equal banks: the top 10% send about 10%
        DayParams q = p;
        q.concentration = 0;
        q.m = 20000;
        q.seed = 3;
        Day e = generate_day(q);
        long long tb = 0;
        for(const DayPayment& x : e.payments) if(x.from < q.n / 10) tb++;
        CHECK(tb / 20000.0 > 0.07 && tb / 20000.0 < 0.13);
    }
    {   // round amounts: everything has at most two significant digits
        DayParams q = p;
        q.round_permille = 1000;
        q.m = 5000;
        q.seed = 4;
        Day e = generate_day(q);
        bool round = true;
        for(const DayPayment& x : e.payments){
            Amount unit = 1;
            for(int k = 0; k < digits_of(x.amt) - 2; k++) unit *= 10;
            if(x.amt % unit != 0 || x.amt < q.amount_min || x.amt > q.amount_max) round = false;
        }
        CHECK(round);
    }
    {   // reciprocal flows: with every payment answered, at least half of all payments are replies
        DayParams q = p;
        q.m = 4000;
        q.concentration = 0;
        q.reciprocal_lag = 900;
        auto replies = [&](const Day& day){
            map<pair<int,int>, vector<const DayPayment*>> by_pair;
            for(const DayPayment& x : day.payments) by_pair[{x.from, x.to}].push_back(&x);
            long long cnt = 0;
            for(const DayPayment& y : day.payments){
                auto it = by_pair.find({y.to, y.from});
                if(it == by_pair.end()) continue;
                for(const DayPayment* x : it->second){
                    Amount lo = max(q.amount_min, x->amt - x->amt / 10), hi = min(q.amount_max, x->amt + x->amt / 10);
                    if(x->t <= y.t && y.t <= min<Time>(q.length, x->t + q.reciprocal_lag) && y.amt >= lo && y.amt <= hi){ cnt++; break; }
                }
            }
            return cnt;
        };
        q.reciprocal_permille = 1000;
        q.seed = 11;
        Day full = generate_day(q);
        CHECK((int)full.payments.size() == q.m);
        CHECK(replies(full) >= q.m / 2);
        q.reciprocal_permille = 0;
        CHECK(replies(generate_day(q)) < q.m / 20);
    }
    auto bad = [&](auto edit){ DayParams z = p; z.m = 100; edit(z); return throws<invalid_argument>([&]{ generate_day(z); }); };
    CHECK(bad([](DayParams& z){ z.n = 1; }));
    CHECK(bad([](DayParams& z){ z.n = 100001; }));
    CHECK(bad([](DayParams& z){ z.m = -1; }));
    CHECK(bad([](DayParams& z){ z.length = 0; }));
    CHECK(bad([](DayParams& z){ z.concentration = -1; }));
    CHECK(bad([](DayParams& z){ z.concentration = 4; }));
    CHECK(bad([](DayParams& z){ z.amount_min = 0; }));
    CHECK(bad([](DayParams& z){ z.amount_min = z.amount_max + 1; }));
    CHECK(bad([](DayParams& z){ z.amount_max = 1000000000001LL; }));
    CHECK(bad([](DayParams& z){ z.decade_permille = 1001; }));
    CHECK(bad([](DayParams& z){ z.round_permille = -1; }));
    CHECK(bad([](DayParams& z){ z.deadline_permille = 2000; }));
    CHECK(bad([](DayParams& z){ z.reciprocal_permille = 1001; }));
    CHECK(bad([](DayParams& z){ z.liquidity_permille = -1; }));
    CHECK(bad([](DayParams& z){ z.credit_permille = -1; }));
    CHECK(bad([](DayParams& z){ z.large_threshold = 0; }));
    CHECK(bad([](DayParams& z){ z.volume_profile = {}; }));
    CHECK(bad([](DayParams& z){ z.value_profile = {0, 0, 0}; }));
    CHECK(bad([](DayParams& z){ z.volume_profile = {3, -1, 2}; }));
    CHECK(bad([](DayParams& z){ z.deadline_min = -1; }));
    CHECK(bad([](DayParams& z){ z.deadline_min = z.deadline_max + 1; }));
    CHECK(bad([](DayParams& z){ z.reciprocal_lag = -1; }));
    CHECK(!bad([](DayParams& z){ z.concentration = 0; z.volume_profile = {1}; }));
    CHECK(!bad([](DayParams& z){ z.n = 100000; }));
    {   // an empty day
        DayParams q = p;
        q.m = 0;
        CHECK(generate_day(q).payments.empty());
    }
}


// ------------------------------------------------------------------ C. generator config files
static bool same_params(const DayParams& a, const DayParams& b){
    return a.seed == b.seed && a.n == b.n && a.m == b.m && a.length == b.length &&
           a.concentration == b.concentration && a.amount_min == b.amount_min && a.amount_max == b.amount_max &&
           a.large_threshold == b.large_threshold && a.decade_permille == b.decade_permille &&
           a.round_permille == b.round_permille && a.volume_profile == b.volume_profile && a.value_profile == b.value_profile &&
           a.deadline_permille == b.deadline_permille && a.deadline_min == b.deadline_min && a.deadline_max == b.deadline_max &&
           a.reciprocal_permille == b.reciprocal_permille && a.reciprocal_lag == b.reciprocal_lag &&
           a.liquidity_permille == b.liquidity_permille && a.credit_permille == b.credit_permille;
}

static DayParams params_from(const string& text){
    istringstream in(text);
    return read_day_params(in);
}

static bool params_throw(const string& text){
    istringstream in(text);
    return throws<runtime_error>([&]{ read_day_params(in); });
}

static void test_config(){
    {   // the defaults, written out exactly
        ostringstream out;
        write_day_params(out, DayParams{});
        CHECK(out.str() ==
              "# settle day generator config v1\n"
              "seed = 1\n"
              "banks = 100\n"
              "payments = 50000\n"
              "length = 36000\n"
              "concentration = 1\n"
              "amount_min = 2000\n"
              "amount_max = 5000000\n"
              "large_threshold = 100000\n"
              "decade_permille = 400\n"
              "round_permille = 300\n"
              "volume_profile = 5,10,12,10,7,8,10,12,9,6\n"
              "value_profile = 2,4,6,8,7,9,12,15,17,14\n"
              "deadline_permille = 200\n"
              "deadline_min = 900\n"
              "deadline_max = 7200\n"
              "reciprocal_permille = 300\n"
              "reciprocal_lag = 1800\n"
              "liquidity_permille = 50\n"
              "credit_permille = 0\n");
        CHECK(same_params(params_from(out.str()), DayParams{}));
    }
    {   // a partial file: given keys change, the rest stay default; comments, blank lines, spacing, CRLF
        DayParams p = params_from("# my day\r\n\n  banks=12\r\nlength = 5000\n\tamount_max =  100050 \n"
                                  "volume_profile = 1, 2 ,3\nseed = 18446744073709551\n");
        DayParams want;
        want.n = 12;
        want.length = 5000;
        want.amount_max = 100050;
        want.volume_profile = {1, 2, 3};
        want.seed = 18446744073709551ULL;
        CHECK(same_params(p, want));
        CHECK(same_params(params_from(""), DayParams{}));
    }
    {   // round trip of something unusual
        DayParams p;
        p.seed = 99;
        p.n = 7;
        p.m = 0;
        p.length = 1;
        p.concentration = 3;
        p.amount_min = 1;
        p.amount_max = 1000000000000LL;
        p.large_threshold = 5;
        p.volume_profile = {1};
        p.value_profile = {0, 4};
        p.deadline_min = 0;
        p.deadline_max = 0;
        p.liquidity_permille = 2500;
        p.credit_permille = 7;
        ostringstream out;
        write_day_params(out, p);
        CHECK(same_params(params_from(out.str()), p));
    }
    {   // set_day_param: one setting at a time (what gen_day --set uses); ranges are generate_day's job
        DayParams p;
        set_day_param(p, "liquidity_permille", "10");
        set_day_param(p, "length", "61200");
        set_day_param(p, "large_threshold", "250000");
        set_day_param(p, "value_profile", "9,8");
        set_day_param(p, "concentration", "-4");
        CHECK(p.liquidity_permille == 10 && p.length == 61200 && p.large_threshold == 250000);
        CHECK(p.value_profile == vector<int>({9, 8}) && p.concentration == -4);
        CHECK(throws<invalid_argument>([&]{ generate_day(p); }));
        CHECK(throws<runtime_error>([&]{ set_day_param(p, "bogus", "1"); }));
        CHECK(throws<runtime_error>([&]{ set_day_param(p, "banks", ""); }));
        CHECK(throws<runtime_error>([&]{ set_day_param(p, "banks", "12x"); }));
        CHECK(throws<runtime_error>([&]{ set_day_param(p, "seed", "-1"); }));
    }
    {   // the error says which line
        string msg;
        try{ params_from("banks = 10\n\nlength = 8am\n"); }catch(const runtime_error& e){ msg = e.what(); }
        CHECK(msg.rfind("line 3", 0) == 0);
    }
    vector<pair<const char*, string>> bad = {
        {"no '='",               "banks 10\n"},
        {"unknown key",          "bankz = 10\n"},
        {"duplicate key",        "banks = 10\nbanks = 20\n"},
        {"empty value",          "banks =\n"},
        {"empty key",            "= 10\n"},
        {"not a number",         "payments = many\n"},
        {"decimal number",       "payments = 1.5\n"},
        {"number too long",      "payments = 1234567890123456789012\n"},
        {"int overflow",         "banks = 3000000000\n"},
        {"decimal amount",       "amount_min = 1.5\n"},
        {"amount with a sign",   "amount_min = +5\n"},
        {"bad list",             "volume_profile = 1,,2\n"},
        {"list trailing comma",  "volume_profile = 1,2,\n"},
        {"negative seed",        "seed = -3\n"},
    };
    for(const auto& [label, text] : bad){
        bool t = params_throw(text);
        CHECK(t);
        if(!t) cerr << "  read_day_params accepted: " << label << "\n";
    }
}

// ------------------------------------------------------------------ D. round trip + replay
static void test_roundtrip_and_replay(bool fast){
    DropViolatorsSolver dv; MaxflowRelaxSolver rx;
    int seeds = fast ? 15 : 40;
    for(int seed = 0; seed < seeds; seed++){
        DayParams p;
        p.n = 30;
        p.m = 2000;
        p.length = 10800;
        p.seed = 500 + seed;
        p.concentration = seed % 3;
        p.reciprocal_permille = 100 * (seed % 8);
        p.round_permille = 500;
        p.liquidity_permille = 5 + 10 * seed;
        p.credit_permille = seed % 3 == 0 ? 20 : 0;
        Day d = generate_day(p);

        stringstream ss;
        write_day(ss, d);
        Day back = read_day(ss);
        CHECK(same_day(back, d));

        EngineConfig c;
        c.batch_interval = 120 + 60 * (seed % 4);
        c.deadline_batch = seed % 2 == 0;
        GridlockSolver& s = seed % 2 ? (GridlockSolver&)dv : (GridlockSolver&)rx;

        SettlementEngine e(d.balance, d.credit_limit, s, c);
        for(const DayPayment& x : d.payments) e.submit(x.from, x.to, x.amt, x.t, x.deadline);
        e.close(d.end);
        EngineStats want = e.stats();
        CHECK(stats_equal(run_day(d, s, c), want));
        CHECK(stats_equal(run_day(back, s, c), want));
    }

    Day broken = read_text("day,36000\nbank,0,10,0\nbank,1,0,0\npay,3600,0,1,1,\n");
    broken.end = 10;                                            // closes before its payment
    CHECK(throws<invalid_argument>([&]{ run_day(broken, dv); }));
    EngineStats z = run_day(read_text("day,36000\nbank,0,0,0\n"), dv);
    CHECK(z.submitted == 0 && z.batches == 0);
}

// ------------------------------------------------------------------ E. a realistic day at scale
static void test_scale(bool fast){
    using clk = chrono::steady_clock;
    auto ms = [](clk::time_point x, clk::time_point y){ return chrono::duration<double, milli>(y - x).count(); };

    DayParams p;                                                // the defaults: 08:00-18:00, Zipf banks, heavy tail
    p.n = 1000;
    p.m = fast ? 50000 : 200000;
    p.seed = 2026;
    auto t0 = clk::now();
    Day d = generate_day(p);
    auto t1 = clk::now();
    stringstream ss;
    write_day(ss, d);
    size_t bytes = ss.str().size();
    Day back = read_day(ss);
    auto t2 = clk::now();
    CHECK(same_day(back, d));

    Amount total = 0;
    vector<Amount> a;
    for(const DayPayment& x : d.payments){ total += x.amt; a.push_back(x.amt); }
    sort(a.begin(), a.end());
    __int128 top = 0;
    for(size_t i = a.size() - a.size() / 100; i < a.size(); i++) top += a[i];

    cout << fixed << setprecision(1);
    cout << "day: " << p.n << " banks, " << p.m << " payments worth " << total << " (median "
         << a[a.size() / 2] << ", top 1% of payments = " << 100.0 * (double)top / (double)total
         << "% of value); generated in " << ms(t0, t1) << " ms, written + read back (" << bytes / 1024 << " KiB) in "
         << ms(t1, t2) << " ms\n";

    MaxflowRelaxSolver rx;
    EngineConfig lsm;
    lsm.batch_interval = 300;
    lsm.deadline_batch = true;
    auto show = [&](const char* label, const Day& day){
        Amount tot = 0;
        for(const DayPayment& x : day.payments) tot += x.amt;
        EngineStats g = run_day(day, rx, EngineConfig{});
        EngineStats b = run_day(day, rx, lsm);
        cout << "  " << label << ": recycling only settles " << 100.0 * (double)g.settled_value / (double)tot << "% of value (p99 "
             << g.delay_p99 << "); with maxflow_relax batches every 300 " << 100.0 * (double)b.settled_value / (double)tot
             << "% (p99 " << b.delay_p99 << ")\n";
    };
    show("default day (liquidity 5%)", d);
    DayParams st = p;                                           // stressed: tight liquidity, peers paying each other
    st.concentration = 0;
    st.reciprocal_permille = 700;
    st.liquidity_permille = 10;
    show("stressed day (liquidity 1%, peers, 70% reciprocal)", generate_day(st));
    cout.unsetf(ios::fixed);
}

int main(int argc, char** argv){
    bool fast = (argc > 1 && string(argv[1]) == "--fast");

    test_format();
    test_generator(fast);
    test_config();
    test_roundtrip_and_replay(fast);
    test_scale(fast);

    if(failures){
        cerr << failures << " check(s) failed\n";
        return 1;
    }
    cout << "all tests passed\n";
    return 0;
}