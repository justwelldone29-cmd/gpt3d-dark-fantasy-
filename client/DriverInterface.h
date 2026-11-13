#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~UniqueHandle() { Reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    HANDLE Get() const noexcept { return value_; }
    explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
    void Reset(HANDLE value = nullptr) noexcept {
        if (*this) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_;
};

// Synchronous, single-threaded wrapper for driver/dumper.h (native x64 ABI).
class DriverInterface {
public:
    bool Open();
    bool ReadMemory(DWORD pid, std::uint64_t address, void* output, std::size_t size);
    bool GetModuleBase(DWORD pid, std::wstring_view module, std::uint64_t& base);
    DWORD LastError() const noexcept { return error_; }

    template<class T>
    bool ReadMemory(DWORD pid, std::uint64_t address, T& output) {
        static_assert(std::is_trivially_copyable_v<T>);
        T temporary{};
        if (!ReadMemory(pid, address, &temporary, sizeof(T))) return false;
        output = temporary;
        return true;
    }
private:
    UniqueHandle device_;
    DWORD error_ = ERROR_SUCCESS;
};
