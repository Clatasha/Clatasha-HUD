#pragma once

#include <cstdint>

namespace clatasha {

constexpr std::uint32_t kHudTelemetryMagic = 0x44554843; // "CHUD"
constexpr std::uint32_t kHudTelemetryVersion = 1;
inline constexpr wchar_t kHudTelemetryMappingName[] =
    L"Local\\ClatashaHUD_Telemetry_v1";

enum HudLocation : std::int32_t {
    HudTopLeft = 0,
    HudTopRight = 1,
    HudBottomLeft = 2,
    HudBottomRight = 3,
};

struct alignas(8) HudTelemetryShared {
    std::uint32_t magic;
    std::uint32_t version;
    volatile std::int32_t sequence;
    volatile std::int32_t gameFpsValid;
    double gameFps;
    double obsFps;
    float desktopLevel;
    float micLevel;
    volatile std::int32_t sessionActive;
    volatile std::int32_t replayBufferActive;
    volatile std::int32_t opacityPercent;
    volatile std::int32_t location;
    std::int64_t elapsedMs;
    wchar_t diskText[16];
};

} // namespace clatasha
