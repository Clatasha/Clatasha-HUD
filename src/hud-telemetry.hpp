#pragma once

#include <cstdint>

namespace clatasha {

constexpr std::uint32_t kHudTelemetryMagic = 0x44554843; // "CHUD"
constexpr std::uint32_t kHudTelemetryVersion = 2;
inline constexpr wchar_t kHudTelemetryMappingName[] =
    L"Local\\ClatashaHUD_Telemetry_v2";

constexpr std::int32_t kHudPixelWidth = 145;
constexpr std::int32_t kHudPixelHeight = 50;
constexpr std::int32_t kHudPixelBytes =
    kHudPixelWidth * kHudPixelHeight * 4;

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

    // The OBS process renders the final HUD image. Injected render backends
    // only copy these RGBA pixels to their API texture.
    volatile std::int32_t pixelWidth;
    volatile std::int32_t pixelHeight;
    volatile std::int32_t pixelBytes;
    volatile std::int32_t pixelReserved;
    std::uint8_t rgba[kHudPixelBytes];
};

} // namespace clatasha
