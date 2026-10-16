// Trace loading shared by harbinger_trace_sim and harbinger_replay, so both read a trace identically.
#pragma once

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace harbinger::trace_io {

struct Invocation { double arrival_ms; uint32_t key; double duration_ms; };

struct Trace {
    std::vector<Invocation> rows;
    std::vector<std::string> keys;
    /// Per-row size hints in row order, NaN when unknown; empty when the trace has no size column.
    std::vector<double> sizes;
};

/// harbinger-trace-v1: arrival_ms,key,duration_ms[,size], sorted by arrival (ties keep file order).
inline Trace load_common(std::ifstream& in, bool sized) {
    Trace t;
    std::unordered_map<std::string, uint32_t> ids;
    std::string line;
    double previous = -INFINITY;
    for (std::size_t number = 2; std::getline(in, line); ++number) {
        const auto fail = [&](const char* what) {
            return std::runtime_error("line " + std::to_string(number) + ": " + what);
        };
        const auto a = line.find(','), b = line.find(',', a + 1), c = line.find(',', b + 1);
        if (b == std::string::npos || (sized ? c == std::string::npos || line.find(',', c + 1) != std::string::npos
                                             : c != std::string::npos))
            throw fail("wrong number of fields");
        const auto key = line.substr(a + 1, b - a - 1);
        if (key.empty() || key.find('"') != std::string::npos) throw fail("bad key");
        const double arrival = std::stod(line.substr(0, a));
        const double duration = std::stod(line.substr(b + 1, sized ? c - b - 1 : std::string::npos));
        if (!std::isfinite(arrival) || arrival < previous) throw fail("rows must be sorted by arrival_ms");
        if (!std::isfinite(duration) || duration < 0) throw fail("bad duration_ms");
        previous = arrival;
        if (sized) {
            const auto field = line.substr(c + 1);
            const double size = field.empty() ? NAN : std::stod(field);
            if (!field.empty() && (!std::isfinite(size) || size < 0)) throw fail("bad size");
            t.sizes.push_back(size);
        }
        const auto [it, fresh] = ids.emplace(key, static_cast<uint32_t>(t.keys.size()));
        if (fresh) t.keys.push_back(key);
        t.rows.push_back({arrival, it->second, duration});
    }
    return t;
}

inline Trace load(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::string line;
    if (!std::getline(in, line)) throw std::runtime_error("empty trace");
    if (line == "arrival_ms,key,duration_ms") return load_common(in, false);
    if (line == "arrival_ms,key,duration_ms,size") return load_common(in, true);
    if (line != "app,func,end_timestamp,duration") throw std::runtime_error("unexpected header");
    // Azure Functions 2021, read as for the published results: arrival reconstructed as end - duration.
    Trace t;
    std::unordered_map<std::string, uint32_t> ids;
    while (std::getline(in, line)) {
        const auto a = line.find(','), b = line.find(',', a + 1), c = line.find(',', b + 1);
        if (c == std::string::npos) throw std::runtime_error("bad row");
        const auto key = line.substr(0, a) + "/" + line.substr(a + 1, b - a - 1);
        const double end = std::stod(line.substr(b + 1, c - b - 1)), duration = std::stod(line.substr(c + 1));
        const auto [it, fresh] = ids.emplace(key, static_cast<uint32_t>(t.keys.size()));
        if (fresh) t.keys.push_back(key);
        t.rows.push_back({(end - duration) * 1000.0, it->second, duration * 1000.0});
    }
    std::sort(t.rows.begin(), t.rows.end(), [](const Invocation& x, const Invocation& y) { return x.arrival_ms < y.arrival_ms; });
    return t;
}

inline double quantile(std::vector<double> v, double q) {
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<std::size_t>(std::max(0.0, std::ceil(v.size() * q) - 1)))];
}

} // namespace harbinger::trace_io
