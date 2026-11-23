#pragma once
#include "model.h"
#include <memory>

namespace pulse {
std::optional<double> read_cpu_temperature();
class Sampler {
public:
    Sampler();
    ~Sampler();
    Hardware sample(unsigned foreground_pid, bool read_nvidia);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
