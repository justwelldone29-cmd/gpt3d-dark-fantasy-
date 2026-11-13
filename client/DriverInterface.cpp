#include "DriverInterface.h"
#include <winioctl.h>
#include <algorithm>
#include <iterator>

namespace {
constexpr DWORD ReadIoctl = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS);
constexpr DWORD ModuleIoctl = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS);
constexpr std::size_t MaxRead = 1024u * 1024u;
struct DumpRequest { ULONG pid; ULONG64 address; ULONG size; };
struct ModuleRequest { ULONG pid; WCHAR module_name[64]; };
static_assert(sizeof(void*) == 8 && sizeof(WCHAR) == 2);
static_assert(sizeof(DumpRequest) == 24 && offsetof(DumpRequest, address) == 8);
static_assert(offsetof(DumpRequest, size) == 16);
static_assert(sizeof(ModuleRequest) == 132 && offsetof(ModuleRequest, module_name) == 4);
}

bool DriverInterface::Open() {
    device_.Reset(CreateFileW(L"\\\\.\\Dumper", GENERIC_READ, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    error_ = device_ ? ERROR_SUCCESS : GetLastError();
    return static_cast<bool>(device_);
}

bool DriverInterface::ReadMemory(DWORD pid, std::uint64_t address,
    void* output, std::size_t size) {
    error_ = ERROR_SUCCESS;
    if (!device_) { error_ = ERROR_INVALID_HANDLE; return false; }
    if (!pid || !address || !output || !size || size > MaxRead ||
        address > UINT64_MAX - (size - 1)) {
        error_ = ERROR_INVALID_PARAMETER;
        return false;
    }
    DumpRequest request{};
    request.pid = pid;
    request.address = address;
    request.size = static_cast<ULONG>(size);
    DWORD returned = 0;
    if (!DeviceIoControl(device_.Get(), ReadIoctl, &request, sizeof(request),
        output, request.size, &returned, nullptr)) {
        error_ = GetLastError();
        return false;
    }
    if (returned != request.size) { error_ = ERROR_PARTIAL_COPY; return false; }
    return true;
}

bool DriverInterface::GetModuleBase(DWORD pid, std::wstring_view module,
    std::uint64_t& base) {
    base = 0;
    error_ = ERROR_SUCCESS;
    if (!device_) { error_ = ERROR_INVALID_HANDLE; return false; }
    ModuleRequest request{};
    if (!pid || module.empty() || module.size() >= std::size(request.module_name) ||
        module.find(L'\0') != std::wstring_view::npos) {
        error_ = ERROR_INVALID_PARAMETER;
        return false;
    }
    request.pid = pid;
    std::copy(module.begin(), module.end(), request.module_name);
    std::uint64_t result = 0;
    DWORD returned = 0;
    if (!DeviceIoControl(device_.Get(), ModuleIoctl, &request, sizeof(request),
        &result, sizeof(result), &returned, nullptr)) {
        error_ = GetLastError();
        return false;
    }
    if (returned != sizeof(result) || !result) {
        error_ = ERROR_INVALID_DATA;
        return false;
    }
    base = result;
    return true;
}
