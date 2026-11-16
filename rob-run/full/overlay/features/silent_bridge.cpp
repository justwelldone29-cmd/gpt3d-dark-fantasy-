#include "silent_bridge.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <cstddef>
namespace {
std::mutex bridge_mutex;
silent_bridge::Config config;
silent_bridge::Status status;
auto heartbeat = std::chrono::steady_clock::time_point{};
static_assert(offsetof(silent_bridge::Config, enabled) == 8);
static_assert(offsetof(silent_bridge::Config, fov) == 12);
static_assert(offsetof(silent_bridge::Config, distance) == 16);
}
namespace silent_bridge {
Config GetConfig() { std::lock_guard<std::mutex> lock(bridge_mutex); return config; }
bool SetConfig(bool enabled, float fov, float distance) {
    if (!std::isfinite(fov) || !std::isfinite(distance) || fov < .1f || fov > 90.f || distance < 1.f || distance > 500.f) return false;
    std::lock_guard<std::mutex> lock(bridge_mutex);
    config.enabled = enabled ? 1u : 0u; config.fov = fov; config.distance = distance;
    return true;
}
Status GetStatus() {
    std::lock_guard<std::mutex> lock(bridge_mutex);
    auto result = status;
    result.connected = result.phase != 0 && heartbeat != std::chrono::steady_clock::time_point{} &&
        std::chrono::steady_clock::now() - heartbeat < std::chrono::seconds(3);
    return result;
}
}
int RobRunSilent_ReadConfig(void* output, std::uint32_t bytes) {
    if (!output || bytes < sizeof(silent_bridge::Config)) return 0;
    const auto current = silent_bridge::GetConfig();
    std::memcpy(output, &current, sizeof(current)); return 1;
}
int RobRunSilent_Configure(int enabled, float fov, float distance) {
    if (enabled != 0 && enabled != 1) return 0;
    return silent_bridge::SetConfig(enabled != 0, fov, distance) ? 1 : 0;
}
void RobRunSilent_ReportStatus(std::uint32_t phase, std::uint64_t redirects, std::uint32_t candidates, const char* message) {
    std::lock_guard<std::mutex> lock(bridge_mutex);
    status.phase = phase <= 2 ? phase : 2;
    status.redirects = redirects; status.candidates = candidates;
    status.message = message ? std::string(message).substr(0, 255) : "";
    heartbeat = std::chrono::steady_clock::now();
    if (status.phase != 1) config.enabled = 0;
}
