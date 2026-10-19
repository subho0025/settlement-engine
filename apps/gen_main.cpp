#include "gen/generators.hpp"
#include "core/io.hpp"

using namespace std;

static void usage(){
    cerr << "usage:\n"
         << "  gen random   n m max_amt seed\n"
         << "  gen hub      n m skew max_amt seed\n"
         << "  gen cycles   n num_cycles min_len max_len noise max_amt seed\n"
         << "  gen clusters n groups m max_amt seed\n";
}

static int as_int(const char* s){
    long long v = stoll(s);
    if(v < INT_MIN || v > INT_MAX) throw invalid_argument(string("value out of int range: ") + s);
    return (int)v;
}
static long long as_ll(const char* s){ return stoll(s); }
static uint64_t as_seed(const char* s){ return (uint64_t)stoull(s); }

int main(int argc, char** argv){
    ios::sync_with_stdio(false);

    if(argc < 2){ usage(); return 1; }
    string kind = argv[1];

    try{
        Instance inst;
        if(kind == "random" && argc == 6){
            inst = gen_random(as_int(argv[2]), as_int(argv[3]), as_ll(argv[4]), as_seed(argv[5]));
        }else if(kind == "hub" && argc == 7){
            inst = gen_hub(as_int(argv[2]), as_int(argv[3]), stod(argv[4]), as_ll(argv[5]), as_seed(argv[6]));
        }else if(kind == "cycles" && argc == 9){
            inst = gen_cycles(as_int(argv[2]), as_int(argv[3]), as_int(argv[4]), as_int(argv[5]),
                              as_int(argv[6]), as_ll(argv[7]), as_seed(argv[8]));
        }else if(kind == "clusters" && argc == 7){
            inst = gen_clusters(as_int(argv[2]), as_int(argv[3]), as_int(argv[4]), as_ll(argv[5]), as_seed(argv[6]));
        }else{
            usage();
            return 1;
        }
        write_instance(cout, inst);
    }catch(const exception& e){
        cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}