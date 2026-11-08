// A part of a composition: a device and the instance path the composition gave it.
//
// The SoC and the ECU build their parts in code and keep each one in a Part, so a part is found by the member that
// holds it and is also known by a path ("soc.qspi", "board.asic"). The path is what a scenario's fault names; the
// fault loader looks it up among the composition's parts. A Part is a pointer with a name, not a registry.
#pragma once

#include <memory>
#include <string>
#include <utility>

namespace sim::sys {

template <class T>
class Part {
   public:
    // Builds the device from its constructor arguments and names it.
    template <class... Args>
    T& create(std::string path, Args&&... args) {
        path_ = std::move(path);
        device_ = std::make_shared<T>(std::forward<Args>(args)...);
        return *device_;
    }

    const std::string& path() const {
        return path_;
    }

    T* operator->() const {
        return device_.get();
    }

    T& operator*() const {
        return *device_;
    }

    T* get() const {
        return device_.get();
    }

    const std::shared_ptr<T>& shared() const {
        return device_;
    }

   private:
    std::string path_;
    std::shared_ptr<T> device_;
};

}  // namespace sim::sys
