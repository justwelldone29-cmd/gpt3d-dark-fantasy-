#pragma once
#include "DriverInterface.h"
#include <imgui.h>
#include <vector>

struct Offsets {
    // TODO: Insert verified module-relative offsets. Zero means unconfigured.
    static constexpr std::uint64_t offset_entity_list = 0;
    static constexpr std::uint64_t offset_local_player = 0;
    static constexpr std::uint64_t offset_view_matrix = 0;
};

struct EntityScreenRect {
    ImVec2 topLeft;
    ImVec2 bottomRight;
};

inline std::vector<EntityScreenRect> EnumerateEntities(
    DriverInterface& driver, DWORD pid, std::uint64_t base, ImVec2 viewport) {
    // Stub: no assumed Rust object layout or pointer chain.
    (void)driver; (void)pid; (void)base; (void)viewport;
    // TODO: Read list/local player/camera with driver.ReadMemory, checking each read.
    // Bound the entity count; validate pointer arithmetic and skip invalid entries.
    // Project positions using the verified matrix convention; reject points behind
    // the camera and return finite rectangles in overlay client coordinates.
    return {};
}

inline void DrawESP(DriverInterface& driver, DWORD pid, std::uint64_t base) {
    if (!base || !Offsets::offset_entity_list || !Offsets::offset_local_player ||
        !Offsets::offset_view_matrix) return;
    auto* draw = ImGui::GetBackgroundDrawList();
    for (const auto& entity : EnumerateEntities(driver, pid, base, ImGui::GetIO().DisplaySize))
        draw->AddRect(entity.topLeft, entity.bottomRight, IM_COL32(80, 220, 120, 255));
}
