#pragma once
#include <cstdint>

struct Rng {
    uint64_t state;

    explicit Rng(uint64_t seed) : state(seed) {}

    uint64_t next(){
        state += 0x9E3779B97F4A7C15ULL;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    int64_t uniform(int64_t lo, int64_t hi){
        uint64_t span = (uint64_t)hi - (uint64_t)lo + 1;
        uint64_t threshold = (0ULL - span) % span;
        uint64_t x;
        do {
            x = next();
        } while (x < threshold);
        return (int64_t)((uint64_t)lo + x % span);
    }

    double next_double(){
        return (double)(next() >> 11) * (1.0 / 9007199254740992.0);
    }
};