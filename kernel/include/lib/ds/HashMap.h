//
// Created by Spencer Martin on 4/11/25.
//

#ifndef CROCOS_HASHMAP_H
#define CROCOS_HASHMAP_H

#include "stddef.h"
#include <lib/math.h>
#include <utility.h>
#include <lib/ds/Tuple.h>

template<typename K>
struct DefaultHasher {
    size_t operator()(const K& key) const {
        static_assert(is_integral_v<K>, "No default hash for this type");
        return static_cast<size_t>(key);
    }
};

template<typename K, typename V, typename Hasher = DefaultHasher<K>>
class HashMap {
private:
    enum class EntryState {
        empty = 0,
        tombstone = 1,
        occupied = 2
    };

    struct Entry {
        K key;
        V value;
        EntryState state;
    };

    template <typename F>
    class TransformingIterator {
    public:
        TransformingIterator(Entry* e, size_t c, size_t i, F t)
                : entries(e), capacity(c), index(i), transform(t) {
            advanceToNextOccupied();
        }

        decltype(auto) operator*() const { return transform(entries[index]); }

        TransformingIterator& operator++() {
            index++;
            advanceToNextOccupied();
            return *this;
        }

        bool operator!=(const TransformingIterator& other) const {
            return index != other.index;
        }

    private:
        void advanceToNextOccupied() {
            while (index < capacity && entries[index].state != EntryState::occupied) {
                index++;
            }
        }

        Entry* entries;
        size_t capacity;
        size_t index;
        F transform;
    };

    template<typename It>
    class IteratorRange {
    public:
        IteratorRange(It begin, It end) : b(begin), e(end) {}

        It begin() const { return b; }
        It end()   const { return e; }

    private:
        It b, e;
    };

    Entry* entryBuffer;
    size_t capacity;
    size_t count;

    Hasher hasher;

    //I want to use integer math - maybe a silly optimization, but it makes the brain happy
    static constexpr size_t LOAD_FACTOR_PERCENTAGE_INCREASE_THRESHOLD = 75;
    static constexpr size_t LOAD_FACTOR_PERCENTAGE_DECREASE_THRESHOLD = 30;

    void resize(size_t newCapacity){
        assert(count <= newCapacity, "Tried to resize hash map to be too small");
        Entry* newEntries = static_cast<Entry*>(operator new(sizeof(Entry) * newCapacity));
        memset(newEntries, 0, sizeof(Entry) * newCapacity);
        for(size_t index = 0; index < capacity; index++){
            auto& entry = entryBuffer[index];
            if(entry.state != EntryState::occupied){
                continue;
            }
            auto newEntryIndex = hasher(entry.key) % newCapacity;
            while(newEntries[newEntryIndex].state == EntryState::occupied){
                newEntryIndex = (newEntryIndex + 1) % newCapacity;
            }
            auto& newEntry = newEntries[newEntryIndex];
            if constexpr (is_trivially_copyable_v<K>) {
                newEntry.key = entry.key;
            }
            else{
                new(&newEntry.key) K(move(entry.key));  // Move each element into the new buffer
                entry.key.~K();  // Explicitly call the destructor of old element
            }
            if constexpr (is_trivially_copyable_v<V>) {
                newEntry.value = entry.value;
            }
            else{
                new(&newEntry.value) V(move(entry.value));  // Move each element into the new buffer
                entry.value.~V();  // Explicitly call the destructor of old element
            }
            newEntry.state = EntryState::occupied;
        }
        capacity = newCapacity;
        operator delete(entryBuffer);
        entryBuffer = newEntries;
    }

    void resizeIfNecessary(){
        size_t loadFactorPercentage = divideAndRoundDown(count * 100, capacity);
        if(loadFactorPercentage > LOAD_FACTOR_PERCENTAGE_INCREASE_THRESHOLD){
            resize(count * 2);
        }
        if((loadFactorPercentage < LOAD_FACTOR_PERCENTAGE_DECREASE_THRESHOLD) && (capacity > 16)){
            auto newCapacity = max(divideAndRoundUp(count * 6, 5ul), 16ul);
            resize(newCapacity);
        }
    }

    Entry& probeIndex(const K& key) const {
        auto startIndex = hasher(key) % capacity;
        auto index = startIndex;
        Entry* firstSeenTombstone = nullptr;
        do{
            auto& entry = entryBuffer[index];
            //if we encounter an empty entry, then the queried key was never in the hash map to begin with
            if(entry.state == EntryState::empty){
                if(firstSeenTombstone != nullptr){
                    return *firstSeenTombstone;
                }
                return entry;
            }
            if((entry.state == EntryState::occupied) && (entry.key == key)){
                return entry;
            }
            if(entry.state == EntryState::tombstone){
                firstSeenTombstone = &entry;
            }
            index = (index + 1) % capacity;
        } while(index != startIndex);
        assertNotReached("HashMap did not obey its own load limit");
    }

public:
    HashMap(size_t init_capacity = 16){
        capacity = init_capacity;
        count = 0;
        entryBuffer = static_cast<Entry*>(operator new(sizeof(Entry) * init_capacity));
        memset(entryBuffer, 0, sizeof(Entry) * init_capacity);
    }

    ~HashMap(){
        if(entryBuffer == nullptr) return;
        for(size_t i = 0; i < capacity; i++){
            auto& entry = entryBuffer[i];
            if(entry.state != EntryState::occupied) continue;
            entry.value.~V();
            entry.key.~K();
        }
        operator delete(entryBuffer);
        entryBuffer = nullptr;
    }

    void insert(const K& key, const V& value){
        resizeIfNecessary();
        auto& entry = probeIndex(key);
        if(entry.state != EntryState::occupied){
            count++;
            resizeIfNecessary();
            entry.state = EntryState::occupied;
            if constexpr (is_trivially_copyable_v<K>) {
                entry.key = key;
            } else {
                new(&entry.key) K(key);
            }
            if constexpr (is_trivially_copyable_v<V>) {
                entry.value = value;
            } else {
                new(&entry.value) V(value);
            }
        } else {
            // Overwrite value only
            if constexpr (is_trivially_copyable_v<V>) {
                entry.value = value;
            } else {
                entry.value.~V();
                new(&entry.value) V(value);
            }
        }
    }

    void insert(const K& key, V&& value){
        resizeIfNecessary();
        auto& entry = probeIndex(key);
        if(entry.state != EntryState::occupied){
            count++;
            resizeIfNecessary();
            entry.state = EntryState::occupied;
            if constexpr (is_trivially_copyable_v<K>) {
                entry.key = key;
            } else {
                new(&entry.key) K(key);
            }
            if constexpr (is_trivially_copyable_v<V>) {
                entry.value = value;
            } else {
                new(&entry.value) V(move(value));
            }
        } else {
            if constexpr (is_trivially_copyable_v<V>) {
                entry.value = value;
            } else {
                entry.value.~V();
                new(&entry.value) V(move(value));
            }
        }
    }

    bool get(const K& key, V& outValue) const{
        auto& entry = probeIndex(key);
        if(entry.state == EntryState::occupied){
            outValue = entry.value;
            return true;
        }
        return false;
    }

    bool take(const K& key, V& outValue){
        auto& entry = probeIndex(key);
        if(entry.state == EntryState::occupied){
            outValue = move(entry.value);
            entry.key.~K();
            entry.value.~V();
            entry.state = EntryState::tombstone;
            count--;
            resizeIfNecessary();
            return true;
        }
        return false;
    }

    bool remove(const K& key){
        auto& entry = probeIndex(key);
        if(entry.state == EntryState::occupied){
            entry.key.~K();
            entry.value.~V();
            entry.state = EntryState::tombstone;
            count--;
            resizeIfNecessary();
            return true;
        }
        return false;
    }

    bool contains(const K& key) const{
        auto& entry = probeIndex(key);
        return entry.state == EntryState::occupied;
    }

    V& at(const K& key) const {
        auto& entry = probeIndex(key);
        if (entry.state != EntryState::occupied) {
            assertNotReached("Key not found in HashMap::at()");
        }
        return entry.value;
    }

    V& operator[](const K& key) {
        auto& entry = probeIndex(key);
        if (entry.state != EntryState::occupied) {
            // Inserting a new entry
            new(&entry.key) K(key);
            new(&entry.value) V(); // Default-construct the value
            entry.state = EntryState::occupied;
            count++;
            resizeIfNecessary();
        }
        return entry.value;
    }

    size_t size() const{
        return count;
    }

    struct KVPTransform {
        Tuple<const K&, V&> operator()(Entry& e) const {
            return makeTuple((const K&) e.key, e.value);
        }
    };

    struct KeyTransform {
        const K& operator()(Entry& e) const {
            return e.key;
        }
    };

    struct ValueTransform {
        V& operator()(Entry& e) const {
            return e.value;
        }
    };

    auto begin(){
        return TransformingIterator(entryBuffer, capacity, 0, KVPTransform{});
    }

    auto end(){
        return TransformingIterator(entryBuffer, capacity, capacity, KVPTransform{});
    }

    auto entries() {
        return IteratorRange(
                begin(),
                end()
        );
    }

    auto keys() {
        using It = TransformingIterator<FunctionRef<const K&(Entry&)>>;
        return IteratorRange<It>(
                It(entryBuffer, capacity, 0, KeyTransform{}),
                It(entryBuffer, capacity, capacity, KeyTransform{})
        );
    }

    auto values() {
        using It = TransformingIterator<FunctionRef<V&(Entry&)>>;
        return IteratorRange<It>(
                It(entryBuffer, capacity, 0, ValueTransform{}),
                It(entryBuffer, capacity, capacity, ValueTransform{})
        );
    }
};



#endif //CROCOS_HASHMAP_H
