//
// Created by Spencer Martin on 4/11/25.
//

#ifndef CROCOS_HASHMAP_H
#define CROCOS_HASHMAP_H

#include "stddef.h"
#include <core/ds/IndexedHashTable.h>
#include <core/utility.h>
#include <core/ds/Tuple.h>

template<typename K, typename V>
struct HashMapKeyExtractor {
    K operator()(Tuple<K, V>& entry) const {
        return entry.first();
    }
};

template<typename K, typename V, typename Hasher = DefaultHasher<K>>
class HashMap : public IndexedHashTable<K, Tuple<K, V>, HashMapKeyExtractor<K, V>, Hasher>{
private:
    using ParentTable = IndexedHashTable<K, Tuple<K, V>, HashMapKeyExtractor<K, V>, Hasher>;

    struct CopyInserter {
        static void freshInsert(ParentTable::Entry& entry, const K& key, const V& value) {
            copy_assign_or_construct(entry.value.first(), key);
            copy_assign_or_construct(entry.value.second(), value);
        }

        static void overwrite(ParentTable::Entry& entry, const K&, const V& value) {
            copy_assign_or_construct(entry.value.second(), value);
        }
    };

    struct MoveInserter {
        static void freshInsert(ParentTable::Entry& entry, const K& key, V&& value) {
            copy_assign_or_construct(entry.value.first(), key);
            move_assign_or_construct(entry.value.second(), move(value));
        }

        static void overwrite(ParentTable::Entry& entry, const K&, V&& value) {
            move_assign_or_construct(entry.value.second(), move(value));
        }
    };

public:
    explicit HashMap(size_t init_capacity = 16) : ParentTable(init_capacity){}

    ~HashMap() = default;

    void insert(const K& key, const V& value){
        this -> template _insert<CopyInserter>(key, key, value);
    }

    void insert(const K& key, V&& value){
        this -> template _insert<MoveInserter>(key, key, move(value));
    }

    bool get(const K& key, V& outValue) const{
        auto& entry = this -> probeEntry(key);
        if(entry.state == ParentTable::EntryState::occupied){
            outValue = entry.value.second();
            return true;
        }
        return false;
    }

    bool take(const K& key, V& outValue){
        auto& entry = this -> probeEntry(key);
        if(entry.state == ParentTable::EntryState::occupied){
            outValue = move(entry.value);
            entry.value.~EntryType();
            entry.state = ParentTable::EntryState::tombstone;
            --(this -> count);
            this -> resizeIfNecessary();
            return true;
        }
        return false;
    }

    bool remove(const K& key){
        auto& entry = this -> probeEntry(key);
        if(entry.state == ParentTable::EntryState::occupied){
            using EntryType = typename ParentTable::InternalEntryType;
            entry.value.~EntryType();
            entry.state = ParentTable::EntryState::tombstone;
            --(this -> count);
            this -> resizeIfNecessary();
            return true;
        }
        return false;
    }

    V& at(const K& key) const {
        auto& entry = this -> probeEntry(key);
        if (entry.state != ParentTable::EntryState::occupied) {
            assertNotReached("Key not found in HashMap::at()");
        }
        return entry.value.second();
    }

    V& operator[](const K& key) {
        auto& entry = this -> probeEntry(key);
        if (entry.state != ParentTable::EntryState::occupied) {
            // Inserting a new entry
            new(&entry.value.first()) K(key);
            new(&entry.value.second()) V(); // Default-construct the value
            entry.state = ParentTable::EntryState::occupied;
            ++(this -> count);
            this -> resizeIfNecessary();
        }
        return entry.value.second();
    }

    struct KVPTransform {
        Tuple<const K&, V&> operator()(typename ParentTable::InternalEntryType& e) const {
            return makeTuple(static_cast<const K &>(e.first()), e.second());
        }
    };

    struct KeyTransform {
        const K& operator()(typename ParentTable::InternalEntryType& e) const {
            return e.first();
        }
    };

    struct ValueTransform {
        V& operator()(typename ParentTable::InternalEntryType& e) const {
            return e.second();
        }
    };

    auto begin(){
        using It = typename ParentTable::template TransformingIterator<KVPTransform>;
        return It(this -> entryBuffer, this -> capacity, 0, KVPTransform{});
    }

    auto end(){
        using It = typename ParentTable::template TransformingIterator<KVPTransform>;
        return It(this -> entryBuffer, this -> capacity, this -> capacity, KVPTransform{});
    }

    auto entries() {
        return IteratorRange(
                begin(),
                end()
        );
    }

    auto keys() {
        using It = typename ParentTable::template TransformingIterator<KeyTransform>;
        return IteratorRange<It>(
                It(this -> entryBuffer, this -> capacity, 0, KeyTransform{}),
                It(this -> entryBuffer, this -> capacity, this -> capacity, KeyTransform{})
        );
    }

    auto values() {
        using It = typename ParentTable::template TransformingIterator<ValueTransform>;
        return IteratorRange<It>(
                It(this -> entryBuffer, this -> capacity, 0, ValueTransform{}),
                It(this -> entryBuffer, this -> capacity, this -> capacity, ValueTransform{})
        );
    }
};

#endif //CROCOS_HASHMAP_H
