// The one exception the JIT layer throws: a failure that is a bug or an unusable host, never an ordinary condition.
// Ordinary conditions (the code memory being full) are returned as values instead.
#pragma once

#include <stdexcept>
#include <string>

namespace sim::jit {

class JitError : public std::runtime_error {
   public:
    explicit JitError(const std::string& message) : std::runtime_error(message) {
    }
};

}  // namespace sim::jit
