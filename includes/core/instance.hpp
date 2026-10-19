#pragma once
#include "core/types.hpp"

using namespace std;

struct Instance {
  int n = 0;
  vector<Obligation> obs;
  vector<Amount> balance;
  vector<Amount> credit_limit;
};