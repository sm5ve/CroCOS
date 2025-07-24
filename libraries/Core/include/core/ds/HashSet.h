//
// Created by Spencer Martin on 7/19/25.
//

#ifndef HASHSET_H
#define HASHSET_H
#include <core/Hasher.h>

#include "IndexedHashTable.h"
#include <core/ds/Optional.h>

template<typename T>
struct HashSetKeyExtractor {
    T& operator()(T& entry) const {
        return entry;
    }
};

template <typename T, typename Hasher = DefaultHasher<T>> requires Hashable<T, Hasher>
class ImmutableIndexedHashSet;

template <typename T, typename Hasher = DefaultHasher<T>> requires Hashable<T, Hasher>
class HashSet : public IndexedHashTable<T, T, HashSetKeyExtractor<T>, Hasher>{
private:
    friend ImmutableIndexedHashSet<T, Hasher>;
    using ParentTable = IndexedHashTable<T, T, HashSetKeyExtractor<T>, Hasher>;
    struct CopyInserter {
        static void freshInsert(ParentTable::Entry& entry, const T& key) {
            copy_assign_or_construct(entry.value, key);
        }

        static void overwrite(ParentTable::Entry& entry, const T& key) {
            copy_assign_or_construct(entry.value, key);
        }
    };

    struct MoveInserter {
        static void freshInsert(ParentTable::Entry& entry, const T&& key) {
            move_assign_or_construct(entry.value, move(key));
        }

        static void overwrite(ParentTable::Entry& entry, const T&& key) {
            move_assign_or_construct(entry.value, move(key));
        }
    };
public:
    explicit HashSet(size_t init_capacity = 16) : ParentTable(init_capacity) {}
    //Inserts the desired element, returns if it was already present
    bool insert(const T& element) {
        if (this -> contains(element)) {
            return true;
        }
        this -> template _insert<CopyInserter>(element, element);
        return false;
    }

    bool insert(T&& element) {
        bool toReturn = false;
        if (this -> contains(element)) {
            toReturn = true;
        }
        this -> template _insert<MoveInserter>(element, move(element));
        return toReturn;
    }

    void remove(const T& element) {
        this -> _remove(element);
    }

    struct IdentityTransform {
        T& operator()(T& e) const {
            return e;
        }
    };

    auto begin(){
        return TransformingIterator(this -> entryBuffer, this -> capacity, 0, IdentityTransform{});
    }

    auto end(){
        return TransformingIterator(this -> entryBuffer, this -> capacity, this -> capacity, IdentityTransform{});
    }

    size_t getCapacity() const {
        return this -> capacity;
    }
};

template <typename T, typename Hasher> requires Hashable<T, Hasher>
class ImmutableIndexedHashSet : public IndexedHashTable<T, T, HashSetKeyExtractor<T>, Hasher>{
private:
    using ParentTable = IndexedHashTable<T, T, HashSetKeyExtractor<T>, Hasher>;
public:
    ImmutableIndexedHashSet(HashSet<T, Hasher>&& set) : ParentTable(set.entryBuffer, set.capacity, set.count){
        set.entryBuffer = nullptr;
        set.capacity = 0;
        set.count = 0;
    }

    Optional<size_t> indexOf(const T& element) const {
        auto& entry = this -> probeEntry(element);
        if (entry.state == ParentTable::EntryState::occupied) {
            return this -> getEntryIndex(entry);
        }
        return {};
    }

    T* fromIndex(size_t index) const {
        if (index >= this -> capacity) {
            return nullptr;
        }
        auto& entry = this -> entryBuffer[index];
        if (entry.state == ParentTable::EntryState::occupied) {
            return &entry.value;
        }
        return nullptr;
    }

    struct IdentityTransform {
        T& operator()(T& e) const {
            return e;
        }
    };

    auto begin(){
        using TI = typename ParentTable::TransformingIterator<IdentityTransform>;
        return TI(this -> entryBuffer, this -> capacity, 0, IdentityTransform{});
    }

    auto end(){
        using TI = typename ParentTable::TransformingIterator<IdentityTransform>;
        return TI(this -> entryBuffer, this -> capacity, this -> capacity, IdentityTransform{});
    }

    size_t getCapacity() const {
        return this -> capacity;
    }
};

#endif //HASHSET_H
