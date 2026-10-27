#include "stream/sim_runner.hpp"
#include "gen/day_gen.hpp"
#include "gridlock/maxflow_relax.hpp"
#include "gridlock/drop_violators.hpp"

using namespace std;

// Contract used by this file (includes/stream/sim_runner.hpp, src/stream/sim_runner.cpp):
//
//   struct SimConfig { string name; string solver; long long node_limit; EngineConfig engine; };
//       defaults: name "default", solver "maxflow_relax", node_limit 1000000, EngineConfig{} (no batches until close)
//
//   set_sim_param(c, key, value)       runtime_error for an unknown key, an empty value, a bad number, a negative
//       batch_interval / queue_trigger / value_trigger, node_limit < -1, deadline_batch other than 0/1, a name that is not
//       1-40 characters from A-Z a-z 0-9 _ - . ; an unknown solver throws invalid_argument (from make_solver)
//       keys: name solver node_limit batch_interval queue_trigger value_trigger deadline_batch
//   read_sim_config(in)                defaults + one 'key = value' per line ('#' comments, blank lines, spaces, '\r' ok);
//       errors become runtime_error("line N: ..."); a duplicate key is an error
//   write_sim_config(out, c)           "# settle simulation config v1" then every key in the order above
//
//   make_solver(name, node_limit)      fifo_bypass | drop_violators | maxflow_relax | branch_bound, else invalid_argument
//   simulate_day(day, c)               fresh engine + the named solver, submit all payments, close(end);
//                                      stats, every PaymentRecord, total value of the day, wall time
//   write_events(out, payments)        header "id,from,to,amount,arrival,deadline,status,how,done_at", one row per payment;
//                                      deadline / done_at empty when there is none

static int failures = 0;

#define CHECK(cond) \
    do { if(!(cond)) { cerr << __FILE__ << ":" << __LINE__ << "  CHECK failed: " #cond "\n"; failures++; } } while(0)

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

static SimConfig from_text(const string& text){
    istringstream in(text);
    return read_sim_config(in);
}

static bool same_config(const SimConfig& a, const SimConfig& b){
    return a.name == b.name && a.solver == b.solver && a.node_limit == b.node_limit &&
           a.engine.batch_interval == b.engine.batch_interval && a.engine.queue_trigger == b.engine.queue_trigger &&
           a.engine.value_trigger == b.engine.value_trigger && a.engine.deadline_batch == b.engine.deadline_batch;
}

static bool same_records(const vector<PaymentRecord>& a, const vector<PaymentRecord>& b){
    if(a.size() != b.size()) return false;
    for(size_t i = 0; i < a.size(); i++){
        if(a[i].from != b[i].from || a[i].to != b[i].to || a[i].amt != b[i].amt || a[i].arrival != b[i].arrival ||
           a[i].deadline != b[i].deadline || a[i].status != b[i].status || a[i].how != b[i].how || a[i].done_at != b[i].done_at)
            return false;
    }
    return true;
}

// ------------------------------------------------------------------ A. config
static void test_config(){
    {
        ostringstream out;
        write_sim_config(out, SimConfig{});
        CHECK(out.str() ==
              "# settle simulation config v1\n"
              "name = default\n"
              "solver = maxflow_relax\n"
              "node_limit = 1000000\n"
              "batch_interval = 0\n"
              "queue_trigger = 0\n"
              "value_trigger = 0\n"
              "deadline_batch = 0\n");
        CHECK(same_config(from_text(out.str()), SimConfig{}));
    }
    {
        SimConfig c = from_text("# relax\r\n\nname = relax_300\nsolver=maxflow_relax\n\tbatch_interval =  300 \ndeadline_batch = 1\r\n");
        SimConfig want;
        want.name = "relax_300";
        want.engine.batch_interval = 300;
        want.engine.deadline_batch = true;
        CHECK(same_config(c, want));

        SimConfig d;
        d.name = "bb.v2-x";
        d.solver = "branch_bound";
        d.node_limit = -1;
        d.engine.batch_interval = 7;
        d.engine.queue_trigger = 50;
        d.engine.value_trigger = 123456789;
        d.engine.deadline_batch = true;
        ostringstream out;
        write_sim_config(out, d);
        CHECK(same_config(from_text(out.str()), d));
        CHECK(same_config(from_text(""), SimConfig{}));
    }
    {
        string msg;
        try{ from_text("name = a\n\nsolver = magic\n"); }catch(const runtime_error& e){ msg = e.what(); }
        CHECK(msg.rfind("line 3", 0) == 0);
    }
    vector<pair<const char*, string>> bad = {
        {"no '='",              "batch_interval 300\n"},
        {"unknown key",         "batch_interval = 300\ncolour = red\n"},
        {"duplicate key",       "name = a\nname = b\n"},
        {"empty value",         "name =\n"},
        {"unknown solver",      "solver = magic\n"},
        {"bad name",            "name = has space\n"},
        {"long name",           "name = " + string(41, 'x') + "\n"},
        {"negative interval",   "batch_interval = -5\n"},
        {"negative trigger",    "queue_trigger = -1\n"},
        {"negative value",      "value_trigger = -1\n"},
        {"node limit < -1",     "node_limit = -2\n"},
        {"bad number",          "batch_interval = 3e2\n"},
        {"deadline_batch 2",    "deadline_batch = 2\n"},
        {"deadline_batch yes",  "deadline_batch = yes\n"},
    };
    for(const auto& [label, text] : bad){
        bool t = throws<runtime_error>([&]{ from_text(text); });
        CHECK(t);
        if(!t) cerr << "  read_sim_config accepted: " << label << "\n";
    }
    SimConfig c;
    CHECK(throws<invalid_argument>([&]{ set_sim_param(c, "solver", "magic"); }));
    CHECK(c.solver == "maxflow_relax");
}

// ------------------------------------------------------------------ B. solvers by name
static void test_make_solver(){
    for(const char* s : {"fifo_bypass", "drop_violators", "maxflow_relax", "branch_bound"}){
        unique_ptr<GridlockSolver> p = make_solver(s, 100);
        CHECK(p && p->name() == s);
    }
    CHECK(throws<invalid_argument>([]{ make_solver("dijkstra"); }));
    CHECK(throws<invalid_argument>([]{ make_solver(""); }));
}

// ------------------------------------------------------------------ C. simulate_day == a manual engine run
static void test_simulate(bool fast){
    vector<SimConfig> cs(4);
    cs[0].name = "recycle_only";
    cs[1].name = "relax_300";
    cs[1].engine.batch_interval = 300;
    cs[1].engine.deadline_batch = true;
    cs[2].name = "drop_q";
    cs[2].solver = "drop_violators";
    cs[2].engine.queue_trigger = 40;
    cs[3].name = "bb";
    cs[3].solver = "branch_bound";
    cs[3].node_limit = 5000000;
    cs[3].engine.batch_interval = 500;

    int seeds = fast ? 4 : 10;
    for(int seed = 0; seed < seeds; seed++){
        DayParams p;
        p.seed = 100 + seed;
        p.n = 20;
        p.m = 1500;
        p.length = 7200;
        p.reciprocal_permille = 500;
        p.liquidity_permille = 20;
        Day d = generate_day(p);
        Amount total = 0;
        for(const DayPayment& x : d.payments) total += x.amt;

        for(const SimConfig& c : cs){
            if(c.solver == "branch_bound" && seed % 3) continue;            // keep the exact solver cheap
            SimResult r = simulate_day(d, c);
            unique_ptr<GridlockSolver> s = make_solver(c.solver, c.node_limit);
            SettlementEngine e(d.balance, d.credit_limit, *s, c.engine);
            for(const DayPayment& x : d.payments) e.submit(x.from, x.to, x.amt, x.t, x.deadline);
            e.close(d.end);
            CHECK(same_records(r.payments, e.payments()));
            CHECK(r.stats.settled_value == e.stats().settled_value && r.stats.batches == e.stats().batches);
            CHECK(r.total_value == total);
            CHECK(r.wall_ms >= 0);
            CHECK(r.stats.submitted == d.payments.size());
        }
    }
    SimConfig bad;
    bad.solver = "magic";
    Day d;
    d.n = 1;
    d.balance = {0};
    d.credit_limit = {0};
    CHECK(throws<invalid_argument>([&]{ simulate_day(d, bad); }));
}

// ------------------------------------------------------------------ D. event log
static void test_events(){
    MaxflowRelaxSolver rx;
    EngineConfig c;
    c.batch_interval = 10;
    SettlementEngine e({100, 0, 0}, {0, 0, 0}, rx, c);
    e.submit(0, 1, 60, 1);                 // arrival
    e.submit(1, 2, 70, 2, 30);             // queued, released by batch? no: by recycling at 3
    e.submit(0, 1, 30, 3);                 // arrival, recycles bank 1 -> payment 1 settles
    e.submit(2, 0, 500, 4, 6);             // expires at 6
    e.submit(2, 1, 999, 5);                // never affordable: unsettled at close
    e.close(20);

    ostringstream out;
    write_events(out, e.payments());
    CHECK(out.str() ==
          "id,from,to,amount,arrival,deadline,status,how,done_at\n"
          "0,0,1,60,1,,settled,arrival,1\n"
          "1,1,2,70,2,30,settled,recycle,3\n"
          "2,0,1,30,3,,settled,arrival,3\n"
          "3,2,0,500,4,6,expired,none,6\n"
          "4,2,1,999,5,,unsettled,none,20\n");

    ostringstream empty;
    write_events(empty, {});
    CHECK(empty.str() == "id,from,to,amount,arrival,deadline,status,how,done_at\n");

    PaymentRecord q;                        // a payment still queued has no done_at
    q.from = 3; q.to = 4; q.amt = 5; q.arrival = 6;
    ostringstream one;
    write_events(one, {q});
    CHECK(one.str() == "id,from,to,amount,arrival,deadline,status,how,done_at\n0,3,4,5,6,,queued,none,\n");
}

int main(int argc, char** argv){
    bool fast = (argc > 1 && string(argv[1]) == "--fast");

    test_config();
    test_make_solver();
    test_simulate(fast);
    test_events();

    if(failures){
        cerr << failures << " check(s) failed\n";
        return 1;
    }
    cout << "all tests passed\n";
    return 0;
}