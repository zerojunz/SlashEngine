#pragma once

#include <cstring>
#include <type_traits>

namespace slash {

// A value's bits as another type of the same size, as std::bit_cast. Use this one: the glibc 2.31
// sysroot that Linux exports build against has libstdc++ 10, which lacks std::bit_cast.
template <typename To, typename From>
    requires(sizeof(To) == sizeof(From) && std::is_trivially_copyable_v<To> &&
             std::is_trivially_copyable_v<From>)
[[nodiscard]] To bit_cast(const From& value) noexcept {
    To out{};
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

} // namespace slash
