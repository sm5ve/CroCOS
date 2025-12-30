//
// Created by Spencer Martin on 10/2/25.
//

#ifndef CROCOS_COMPARE_H
#define CROCOS_COMPARE_H
namespace std {
    class strong_ordering {
        signed char value;

        constexpr explicit strong_ordering(signed char v) : value(v) {}
    public:
        static const strong_ordering less;
        static const strong_ordering equal;
        static const strong_ordering greater;

        constexpr bool operator==(const strong_ordering& rhs) const = default;
        constexpr bool operator<(const strong_ordering& rhs) const { return value < rhs.value; }
        constexpr bool operator<=(const strong_ordering& rhs) const { return value <= rhs.value; }
        constexpr bool operator!=(const strong_ordering& rhs) const = default;
        constexpr bool operator>(const strong_ordering& rhs) const { return value > rhs.value; }
        constexpr bool operator>=(const strong_ordering& rhs) const { return value >= rhs.value; }
    };

    inline constexpr strong_ordering strong_ordering::less{-1};
    inline constexpr strong_ordering strong_ordering::equal{0};
    inline constexpr strong_ordering strong_ordering::greater{1};
}
#endif //CROCOS_COMPARE_H