//
// Created by Spencer Martin on 8/27/25.
//

#ifndef CROCOS_BIMAP_H
#define CROCOS_BIMAP_H

#include <core/ds/HashMap.h>
#include <core/ds/SmartPointer.h>

template <typename L, typename R, typename LeftHasher = DefaultHasher<L>, typename RightHasher = DefaultHasher<R>>
requires (Hashable<L, LeftHasher> && Hashable<R, RightHasher>)
class Bimap {
    SharedPtr<HashMap<L, R, LeftHasher>> leftMap;
    SharedPtr<HashMap<R, L, RightHasher>> rightMap;
    Bimap(SharedPtr<HashMap<L, R, LeftHasher>> lm, SharedPtr<HashMap<R, L, RightHasher>> rm) : leftMap(lm), rightMap(rm) {}
public:
    Bimap() : leftMap(make_shared<HashMap<L, R, LeftHasher>>()), rightMap(make_shared<HashMap<R, L, RightHasher>>()) {}
    Bimap(const Bimap& toCopy) : leftMap(toCopy.leftMap), rightMap(toCopy.rightMap) {}
    Bimap(Bimap&& toMove) noexcept : leftMap(move(toMove.leftMap)), rightMap(move(toMove.rightMap)) {}
    Bimap& operator=(const Bimap& toCopy) = default;
    Bimap& operator=(Bimap&& toMove) noexcept = default;

    Bimap<R, L, RightHasher, LeftHasher> inverse() const {
        return Bimap(rightMap, leftMap);
    }

    bool remove(const L& left) {
        if (leftMap->contains(left)) {
            const auto& right = leftMap->at(left);
            rightMap->remove(right);
            leftMap->remove(left);
            return true;
        }
        return false;
    }

    bool removeRight(const R& right) {
        if (rightMap->contains(right)) {
            const auto& left = rightMap->at(right);
            leftMap->remove(left);
            rightMap->remove(right);
            return true;
        }
        return false;
    }

    bool insert(const L& left, const R& right) {
        if (leftMap->contains(left)) {return false;}
        if (rightMap->contains(right)) {return false;}
        leftMap->insert(left, right);
        rightMap->insert(right, left);
        return true;
    }

    bool insert(const Tuple<L, R>& tup) {
        return insert(tup.first(), tup.second());
    }

    bool contains(const L& left) const {
        return leftMap->contains(left);
    }

    bool containsRight(const R& right) const {
        return rightMap->contains(right);
    }

    const R& at(const L& left) const {
        return leftMap->at(left);
    }

    const L& atRight(const R& right) const {
        return rightMap->at(right);
    }

    auto begin() const {
        return leftMap->begin();
    }

    auto end() const {
        return leftMap->end();
    }

    auto entries() const {
        return leftMap->entries();
    }

    auto leftValues() const {
        return leftMap->keys();
    }

    auto rightValues() const {
        return leftMap->values();
    }
};

#endif //CROCOS_BIMAP_H