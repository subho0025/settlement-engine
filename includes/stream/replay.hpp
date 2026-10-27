#pragma once
#include "core/day.hpp"
#include "stream/engine.hpp"

using namespace std;

EngineStats run_day(const Day& d, GridlockSolver& solver, EngineConfig cfg = {});