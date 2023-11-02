#ifndef BALLANCEMMOSERVER_RECORD_ENTRY_HPP
#define BALLANCEMMOSERVER_RECORD_ENTRY_HPP
#include <cstdint>
#include <cstring>
#include <cstddef>

namespace bmmo {
    struct record_entry {
        int32_t size = 0;
        std::byte* data = nullptr;
        record_entry(): size(0), data(nullptr) {}

        explicit record_entry(int32_t s) {
            assert(size >= 0);
            size = s;
            if (s > 0)
                data = new std::byte[size];
        }

        record_entry(int64_t time, int32_t size, std::byte* msg) {
            assert(size >= 0);
            data = new std::byte[size + sizeof(time) + sizeof(size)];
            std::memcpy(data, &time, sizeof(time));
            std::memcpy(data + sizeof(time), &size, sizeof(size));
            std::memcpy(data + sizeof(time) + sizeof(size), msg, size);

            this->size = size + sizeof(time) + sizeof(size);
        }

        record_entry(record_entry& other) = delete;
        record_entry(record_entry&& other) noexcept {
            this->data = other.data;
            this->size = other.size;
            other.data = nullptr;
            other.size = 0;
        }

        record_entry& operator=(record_entry&& other)  noexcept {
            this->data = other.data;
            this->size = other.size;
            other.data = nullptr;
            other.size = 0;
            return *this;
        }

        ~record_entry() {
            assert(size >= 0);
            delete[] data;
        }
    };
}

#endif //BALLANCEMMOSERVER_RECORD_ENTRY_HPP