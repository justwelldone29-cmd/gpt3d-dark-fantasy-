#pragma once
#include <cstdint>
#include <string>
namespace silent_bridge {
struct Config {
    std::uint32_t version = 1, bytes = 24, enabled = 0;
    float fov = 10.f, distance = 200.f;
    std::uint32_t reserved = 0;
};
static_assert(sizeof(Config) == 24);
struct Status {
    std::uint32_t phase = 0, candidates = 0;
    std::uint64_t redirects = 0;
    std::string message = "Waiting for launcher";
    bool connected = false;
};
Config GetConfig();
bool SetConfig(bool enabled, float fov, float distance);
Status GetStatus();
}
#ifdef _WIN32
#define ROB_SILENT_EXPORT extern "C" __declspec(dllexport)
#else
#define ROB_SILENT_EXPORT extern "C"
#endif
ROB_SILENT_EXPORT int RobRunSilent_ReadConfig(void*, std::uint32_t);
ROB_SILENT_EXPORT int RobRunSilent_Configure(int, float, float);
ROB_SILENT_EXPORT void RobRunSilent_ReportStatus(std::uint32_t, std::uint64_t, std::uint32_t, const char*);
