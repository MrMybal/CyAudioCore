#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace path_measurement {
inline volatile double sink = 0;
inline void require(bool value) { if (!value) throw std::runtime_error("path measurement validation"); }
template <class F, class Check>
void bench(std::string_view name, std::size_t frames, std::size_t instances,
           std::string_view backend, F process, Check check) {
    constexpr unsigned batch = 32, batches = 31, warmup = 64;
    for (unsigned i = 0; i < warmup; ++i) process();
    std::array<double,batches> costs{};
    for (auto& value : costs) {
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < batch; ++i) process();
        value = std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/batch;
    }
    const auto signature = check(); // Validate/summarize all output outside the timed region.
    require(std::isfinite(signature));
    std::sort(costs.begin(),costs.end());
    std::cout << std::setprecision(15) << "{\"case\":\"" << name << "\",\"frames\":" << frames
              << ",\"instances\":" << instances << ",\"backend\":\"" << backend
              << "\",\"medianBatchMeanNs\":" << costs[15] << ",\"p95BatchMeanNs\":" << costs[29]
              << ",\"projectedCorePercent48000\":" << costs[15] * 48000 / frames / 1e7
              << ",\"signature\":" << signature << ",\"calls\":" << batch*batches
              << ",\"warmup\":" << warmup << ",\"batchSize\":" << batch << "}\n" << std::flush;
}
} // namespace path_measurement
