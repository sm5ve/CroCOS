//
// Created by Spencer Martin on 4/13/26.
//

#ifndef CROCOS_FLAGS_H
#define CROCOS_FLAGS_H

#include <core/TypeTraits.h>

// Opt-in trait: specialize this for your enum class to enable Flags<E>.
// Example:
//   template<> struct is_flags_enum<MyEnum> { static constexpr bool value = true; };
template<typename E>
struct is_flags_enum { static constexpr bool value = false; };

// Type-safe bitmask wrapper for enum class types.
// Construct from one or more enum values using operator|.
// Test for presence with operator& and operator bool.
template<typename E>
    requires (is_enum_v<E> && is_flags_enum<E>::value)
class Flags {
    using U = underlying_type_t<E>;
    U bits;

    explicit constexpr Flags(U raw) : bits(raw) {}

public:
    constexpr Flags() : bits(0) {}
    // Implicit construction from a single enum value.
    // NOLINTNEXTLINE(google-explicit-constructor)
    constexpr Flags(E e) : bits(static_cast<U>(e)) {}

    // Bitwise OR
    [[nodiscard]] constexpr Flags operator|(Flags other) const { return Flags(bits | other.bits); }
    [[nodiscard]] constexpr Flags operator|(E e)         const { return Flags(bits | static_cast<U>(e)); }
    constexpr Flags& operator|=(Flags other) { bits |= other.bits; return *this; }
    constexpr Flags& operator|=(E e)         { bits |= static_cast<U>(e); return *this; }

    // Bitwise AND (used for flag testing)
    [[nodiscard]] constexpr Flags operator&(Flags other) const { return Flags(bits & other.bits); }
    [[nodiscard]] constexpr Flags operator&(E e)         const { return Flags(bits & static_cast<U>(e)); }
    constexpr Flags& operator&=(Flags other) { bits &= other.bits; return *this; }
    constexpr Flags& operator&=(E e)         { bits &= static_cast<U>(e); return *this; }

    // Bitwise complement
    [[nodiscard]] constexpr Flags operator~() const { return Flags(~bits); }

    // Contextual bool: true if any flag is set
    [[nodiscard]] constexpr explicit operator bool() const { return bits != 0; }

    [[nodiscard]] constexpr bool operator==(Flags other) const { return bits == other.bits; }
    [[nodiscard]] constexpr bool operator!=(Flags other) const { return bits != other.bits; }

    // Check whether a specific flag (or combination) is fully set
    [[nodiscard]] constexpr bool has(E e) const {
        U mask = static_cast<U>(e);
        return (bits & mask) == mask;
    }
    [[nodiscard]] constexpr bool has(Flags f) const {
        return (bits & f.bits) == f.bits;
    }

    // Raw underlying value (escape hatch — prefer the typed API)
    [[nodiscard]] constexpr U raw() const { return bits; }
};

// E | E → Flags<E> so that two raw enum values can be OR-ed without an explicit cast.
template<typename E>
    requires (is_enum_v<E> && is_flags_enum<E>::value)
[[nodiscard]] constexpr Flags<E> operator|(E a, E b) {
    return Flags<E>(a) | b;
}

#endif //CROCOS_FLAGS_H
