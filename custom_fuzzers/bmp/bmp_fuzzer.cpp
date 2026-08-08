#include "common.h"
#include "fuzzer.h"
#include "mutators/bmp/bmp_mutator.h"

class BmpFuzzer : public Fuzzer {
protected:
    Mutator* CreateMutator(int argc, char** argv,
        ThreadContext* tc) override {
        int nrounds = GetIntOption(
            "-iterations_per_round", argc, argv, 1000);

        return new NRoundMutator(new BmpMutator(), nrounds);
    }
};

int main(int argc, char** argv) {
    BmpFuzzer fuzzer;
    fuzzer.Run(argc, argv);
}