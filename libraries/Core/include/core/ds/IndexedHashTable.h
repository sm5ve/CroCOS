//
// Created by Spencer Martin on 7/18/25.
//

#ifndef INDEXEDHASHTABLE_H
#define INDEXEDHASHTABLE_H
#include "kernel.h"
#include "core/Hasher.h"
#include "core/math.h"
#include "core/TypeTraits.h"
#include "core/utility.h"

template<typename EntryType, typename Key, typename Extractor>
concept KeyExtractor = requires(EntryType entry, Extractor extractor) {
    { extractor(entry) } -> convertible_to<Key>;
};

template <typename Inserter, typename Entry, typename... Ts>
concept Inserts = requires(Inserter inserter, Entry& entry, Ts... ts) {
    Inserter::freshInsert(entry, forward<Ts>(ts)...);
    Inserter::overwrite(entry, forward<Ts>(ts)...);
};

template<typename Key, typename EntryType, typename Extractor, typename Hasher = DefaultHasher<Key>>
requires (KeyExtractor<EntryType, Key, Extractor> && Hashable<Key, Hasher>)
class IndexedHashTable {
protected:
    using InternalEntryType = EntryType;

    enum class EntryState {
        empty = 0,
        tombstone = 1,
        occupied = 2
    };

    struct Entry {
        EntryType value;
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

    Entry* entryBuffer;
    size_t capacity;
    size_t count;

    Hasher hasher;
    Extractor extractor;

    //I want to use integer math - maybe a silly optimization, but it makes the brain happy
    static constexpr size_t LOAD_FACTOR_PERCENTAGE_INCREASE_THRESHOLD = 75;
    static constexpr size_t LOAD_FACTOR_PERCENTAGE_DECREASE_THRESHOLD = 30;

    void resize(size_t newCapacity){
        //assert(count <= newCapacity, "Tried to resize hash map to be too small");
        auto* newEntries = static_cast<Entry*>(operator new(sizeof(Entry) * newCapacity));
        memset(newEntries, 0, sizeof(Entry) * newCapacity);
        for(size_t index = 0; index < capacity; index++){
            auto& entry = entryBuffer[index];
            if(entry.state != EntryState::occupied){
                continue;
            }
            auto newEntryIndex = hasher(extractor(entry.value)) % newCapacity;
            while(newEntries[newEntryIndex].state == EntryState::occupied){
                newEntryIndex = (newEntryIndex + 1) % newCapacity;
            }
            auto& newEntry = newEntries[newEntryIndex];
            if constexpr (is_trivially_copyable_v<EntryType>) {
                newEntry.value = entry.value;
            }
            else{
                new(&newEntry.value) EntryType(move(entry.value));  // Move each element into the new buffer
                entry.value.~EntryType();  // Explicitly call the destructor of old element
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

    Entry& probeEntry(const Key& key) const {
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
            if((entry.state == EntryState::occupied) && (extractor(entry.value) == key)){
                return entry;
            }
            if(entry.state == EntryState::tombstone){
                firstSeenTombstone = &entry;
            }
            index = (index + 1) % capacity;
        } while(index != startIndex);
        assertNotReached("HashMap did not obey its own load limit");
    }

    explicit IndexedHashTable(const size_t init_capacity){
        capacity = init_capacity;
        count = 0;
        entryBuffer = static_cast<Entry*>(operator new(sizeof(Entry) * init_capacity));
        memset(entryBuffer, 0, sizeof(Entry) * init_capacity);
    }

    ~IndexedHashTable(){
        if(entryBuffer == nullptr) return;
        for(size_t i = 0; i < capacity; i++){
            auto& entry = entryBuffer[i];
            if(entry.state != EntryState::occupied) continue;
            entry.value.~EntryType();
        }
        operator delete(entryBuffer);
        entryBuffer = nullptr;
    }

    template<typename Inserter, typename... Ts> requires Inserts<Inserter, Entry, Ts...>
    void _insert(const Key& key, Ts... ts){
        resizeIfNecessary();
        auto& entry = probeEntry(key);
        if(entry.state != EntryState::occupied){
            count++;
            entry.state = EntryState::occupied;
            Inserter::freshInsert(entry, forward<Ts>(ts)...);
            resizeIfNecessary();
        } else {
            Inserter::overwrite(entry, forward<Ts>(ts)...);
        }
    }

    bool _remove(const Key& key){
        auto& entry = probeEntry(key);
        if(entry.state == EntryState::occupied){
            entry.value.~EntryType();
            entry.state = EntryState::tombstone;
            count--;
            resizeIfNecessary();
            return true;
        }
        return false;
    }
public:
    [[nodiscard]]
    bool contains(const Key& key) const{
        auto& entry = probeEntry(key);
        return entry.state == EntryState::occupied;
    }

    [[nodiscard]] size_t size() const{
        return count;
    }
};
#endif //INDEXEDHASHTABLE_H
