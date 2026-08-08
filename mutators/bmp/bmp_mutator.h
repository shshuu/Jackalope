#pragma once
#include "mutator.h"

class BmpMutator : public Mutator {
public:
    bool Mutate(Sample* sample, PRNG* prng,
        std::vector<Sample*>& all_samples) override;
};