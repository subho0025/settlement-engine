#include "stream/replay.hpp"

using namespace std;

EngineStats run_day(const Day& d, GridlockSolver& solver, EngineConfig cfg){
    SettlementEngine e(d.balance, d.credit_limit, solver, cfg);
    for(const DayPayment& x : d.payments){
        e.submit(x.from, x.to, x.amt, x.t, x.deadline);
    }
    e.close(d.end);
    return e.stats();
}