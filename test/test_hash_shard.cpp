#include <iostream>
#include <cstdint>

inline int stock_id_fast(const char* symbol, int shard_count) {
    uint64_t hash = 0;
    for (const char* p = symbol; *p && (p - symbol) < 40; ++p) {
        hash = hash * 31 + static_cast<unsigned char>(*p);
    }
    return static_cast<int>(hash % shard_count);
}

int main() {
    const char* symbol = "603122.SH";

    std::cout << "Symbol: " << symbol << "\n";
    std::cout << "Shard (count=1): " << stock_id_fast(symbol, 1) << "\n";
    std::cout << "Shard (count=4): " << stock_id_fast(symbol, 4) << "\n";

    return 0;
}
