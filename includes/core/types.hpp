#pragma once
#include <bits/stdc++.h>

using namespace std;

using Amount = int64_t;
using PartyId = int64_t;
using Time = int64_t;

struct Obligation{
    PartyId from;
    PartyId to;
    Amount amt;
    int prio = 0;
    Time deadline = 0;
};

struct Transfer{
    PartyId from;
    PartyId to;
    Amount amt;
};

struct Plan{
    vector<Transfer> transfers;
};