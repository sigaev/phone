#pragma once

#include <memory>

namespace common {

// ADL selects the destroy(T*) free function in the opaque type's namespace.
template <typename T>
struct Destroy {
    void operator()(T* object) const noexcept {
        destroy(object);
    }
};

template <typename T>
using Owner = std::unique_ptr<T, Destroy<T>>;

}  // namespace common
