#include "gen/generators.hpp"
#include "core/rng.hpp"

using namespace std;

static const int64_t MAX_N   = 100000;
static const int64_t MAX_M   = 1000000;
static const int64_t MAX_AMT = 1000000000000LL;

static void need(bool ok, const char* msg){
    if(!ok) throw invalid_argument(msg);
}

static void check_common(int n, Amount max_amt){
    need(n >= 2 && n <= MAX_N, "n must be in [2, 100000]");
    need(max_amt >= 1 && max_amt <= MAX_AMT, "max_amt must be in [1, 1e12]");
}

static Obligation make_ob(int from, int to, Amount amt){
    Obligation o;
    o.from = from;
    o.to = to;
    o.amt = amt;
    return o;
}

static int other_than(Rng& rng, int n, int from){
    int to = (int)rng.uniform(0, n - 2);
    if(to >= from) to++;
    return to;
}

Instance gen_random(int n, int m, Amount max_amt, uint64_t seed){
    check_common(n, max_amt);
    need(m >= 0 && m <= MAX_M, "m must be in [0, 1000000]");

    Rng rng(seed);
    Instance inst;
    inst.n = n;
    inst.obs.reserve(m);

    for(int i = 0; i < m; i++){
        int from = (int)rng.uniform(0, n - 1);
        int to = other_than(rng, n, from);
        inst.obs.push_back(make_ob(from, to, rng.uniform(1, max_amt)));
    }
    return inst;
}

Instance gen_hub(int n, int m, double skew, Amount max_amt, uint64_t seed){
    check_common(n, max_amt);
    need(m >= 0 && m <= MAX_M, "m must be in [0, 1000000]");
    need(skew >= 0.0 && skew <= 3.0, "skew must be in [0, 3]");

    Rng rng(seed);

    vector<double> pref(n);
    double acc = 0;
    for(int i = 0; i < n; i++){
        acc += 1.0 / pow((double)(i + 1), skew);
        pref[i] = acc;
    }
    auto sample = [&]() -> int {
        double x = rng.next_double() * acc;
        int idx = (int)(upper_bound(pref.begin(), pref.end(), x) - pref.begin());
        return idx >= n ? n - 1 : idx;
    };

    Instance inst;
    inst.n = n;
    inst.obs.reserve(m);

    for(int i = 0; i < m; i++){
        int from = sample();
        int to;
        do { to = sample(); } while (to == from);
        inst.obs.push_back(make_ob(from, to, rng.uniform(1, max_amt)));
    }
    return inst;
}

Instance gen_cycles(int n, int num_cycles, int min_len, int max_len, int noise,
                    Amount max_amt, uint64_t seed){
    check_common(n, max_amt);
    need(num_cycles >= 0, "num_cycles must be >= 0");
    need(min_len >= 2 && max_len >= min_len && max_len <= n, "need 2 <= min_len <= max_len <= n");
    need(noise >= 0, "noise must be >= 0");
    need((long long)num_cycles * min_len + noise <= MAX_M, "total obligations would exceed 1000000");

    Rng rng(seed);

    vector<int> lens(num_cycles);
    long long total = noise;
    for(int c = 0; c < num_cycles; c++){
        lens[c] = (int)rng.uniform(min_len, max_len);
        total += lens[c];
    }
    need(total <= MAX_M, "total obligations would exceed 1000000");

    Instance inst;
    inst.n = n;
    inst.obs.reserve(total);

    // persistent permutation: partial Fisher-Yates gives L distinct parties in O(L) per cycle
    vector<int> perm(n);
    iota(perm.begin(), perm.end(), 0);

    for(int c = 0; c < num_cycles; c++){
        int L = lens[c];
        for(int j = 0; j < L; j++){
            int k = j + (int)rng.uniform(0, n - 1 - j);
            swap(perm[j], perm[k]);
        }
        Amount a = rng.uniform(1, max_amt);
        for(int j = 0; j < L; j++){
            inst.obs.push_back(make_ob(perm[j], perm[(j + 1) % L], a));
        }
    }

    for(int i = 0; i < noise; i++){
        int from = (int)rng.uniform(0, n - 1);
        int to = other_than(rng, n, from);
        inst.obs.push_back(make_ob(from, to, rng.uniform(1, max_amt)));
    }

    // Fisher-Yates shuffle so obligation order does not reveal the structure
    for(size_t i = inst.obs.size(); i > 1; i--){
        size_t j = (size_t)rng.uniform(0, (int64_t)i - 1);
        swap(inst.obs[i - 1], inst.obs[j]);
    }
    return inst;
}

Instance gen_clusters(int n, int groups, int m, Amount max_amt, uint64_t seed, vector<int>* group_of){
    check_common(n, max_amt);
    need(groups >= 1 && 2LL * groups <= n, "need groups >= 1 and n >= 2 * groups");
    need(m >= 0 && m <= MAX_M, "m must be in [0, 1000000]");

    Rng rng(seed);

    // random assignment: shuffle parties, then cut the shuffled order into near-equal consecutive blocks
    vector<int> perm(n);
    iota(perm.begin(), perm.end(), 0);
    for(int i = n - 1; i >= 1; i--){
        int j = (int)rng.uniform(0, i);
        swap(perm[i], perm[j]);
    }

    vector<int> start(groups + 1);
    for(int g = 0; g < groups; g++) start[g] = (int)((long long)g * n / groups);
    start[groups] = n;

    vector<int> gid(n), pos(n);
    for(int g = 0; g < groups; g++){
        for(int i = start[g]; i < start[g + 1]; i++){
            gid[perm[i]] = g;
            pos[perm[i]] = i;
        }
    }

    Instance inst;
    inst.n = n;
    inst.obs.reserve(m);

    for(int i = 0; i < m; i++){
        int from = (int)rng.uniform(0, n - 1);
        int g = gid[from];
        int size = start[g + 1] - start[g];
        int cand = start[g] + (int)rng.uniform(0, size - 2);
        if(cand >= pos[from]) cand++;
        inst.obs.push_back(make_ob(from, perm[cand], rng.uniform(1, max_amt)));
    }

    if(group_of) *group_of = gid;
    return inst;
}