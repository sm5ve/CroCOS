//
// Created by Spencer Martin on 7/18/25.
//

#ifndef HASHER_H
#define HASHER_H

template<typename K>
struct DefaultHasher {
    size_t operator()(const K& key) const {
        static_assert(is_integral_v<K>, "No default hash for this type");
        return static_cast<size_t>(key);
    }
};

template<>
struct DefaultHasher<const char*> {
    const uint64_t modulus = static_cast<uint64_t>(-59l);
    const uint64_t prime = 37;
    size_t operator()(const char* str) const {
        size_t hash = 0;
        size_t x = 1;
        while (*str != 0) {
            hash = (hash + x * *str) % modulus;
            x = (x * prime) % modulus;
            str++;
        }
        return hash;
    }
};

template <typename Type, typename Hasher>
concept Hashable = requires(const Type& t, Hasher h)
{
    {h(t)} -> convertible_to<size_t>;
};

#endif //HASHER_H
