<p align="center">
  <img src="assets/Clatasha%20Hud%20OBS.png" width="360" alt="Clatasha HUD logo">
</p>

# Clatasha HUD

Clatasha HUD is a Windows OBS Studio plugin that adds a compact, always-on-top status HUD for the streamer and a five-slot browser overlay manager for HUD-only, stream-only, or combined overlays.

The plugin runs inside OBS Studio. It does not require a separate Python process.

## Current features

### Compact local HUD

- 145 × 50 frameless, always-on-top, click-through HUD.
- Actual foreground game/application FPS through the Clatasha DXGI ETW helper.
- OBS renderer FPS shown beside the game FPS.
- Game FPS is blue while idle and red while recording or streaming.
- Desktop Audio and Mic/Aux segmented level meters with separate monitor and microphone icons.
- Recording/streaming session timer.
- Recording-drive free-space display.
- Recording/streaming activity spinner.
- Adjustable HUD opacity and corner placement.
- Windows capture-exclusion request for local HUD windows where supported.

If game FPS cannot be read, Clatasha HUD displays `--` rather than substituting OBS renderer FPS.

## Browser Overlays

Clatasha HUD includes five configurable overlay slots. Each slot has:

- Enable/disable control.
- Friendly name.
- URL or local-file input.
- `HUD`, `VIDEO`, or `HUD / VIDEO` output mode.
- Preview/Edit placement tool with drag and resize handles.
- Saved HUD and VIDEO placement.
- Apply confirmation toast.

### Supported overlay inputs

Remote browser widgets such as Streamlabs alert boxes can be pasted directly into a slot.

Direct image URLs are automatically placed on a transparent canvas instead of Chromium's built-in image-viewer background. Supported image types include PNG, APNG, GIF, WebP, SVG, JPG/JPEG, BMP, and AVIF.

Local files can be selected with **Browse Local…**. Local transparent images use the same alpha-preserving path as remote images. Local HTML/HTM files use OBS Browser Source local-file mode.

### Output modes

- **HUD** — shown locally as a desktop HUD overlay.
- **VIDEO** — created as a Clatasha-managed OBS Browser Source in the current scene.
- **HUD / VIDEO** — shown in both places.

HUD browser overlays render through an off-screen OBS `browser_source` so transparent alert widgets stay invisible until they actually draw content.

## Game FPS backend

Game/application FPS is collected by `clatasha-fps-helper.exe` using the Windows DXGI ETW provider. The helper is launched elevated because starting the ETW session normally requires suitable Windows tracing permissions.

Current limitations:

- The FPS path is DXGI-focused. Vulkan/OpenGL titles may show `--`.
- The current target is the non-OBS foreground process.
- A UAC prompt may appear when the helper starts.
- The game FPS display intentionally holds the last valid sample briefly to prevent flicker during short ETW/state-file gaps.

## Requirements

- Windows 10 or Windows 11 x64.
- OBS Studio with Browser Source/obs-browser installed and enabled for browser overlays.
- Current development/testing is on OBS Studio 32.2.2.
- CI currently uses the OBS plugin-template dependency set pinned to OBS 31.1.1.

## Windows installation

The Windows ZIP uses the OBS installation-root layout:

```text
obs-plugins/
  64bit/
    clatasha-hud.dll
data/
  obs-plugins/
    clatasha-hud/
      clatasha-fps-helper.exe
      locale/
        en-US.ini
```

Close OBS Studio, then extract the ZIP directly into the OBS installation directory, normally:

```text
C:\Program Files\obs-studio
```

After extraction, the main files should be:

```text
C:\Program Files\obs-studio\obs-plugins\64bit\clatasha-hud.dll
C:\Program Files\obs-studio\data\obs-plugins\clatasha-hud\clatasha-fps-helper.exe
C:\Program Files\obs-studio\data\obs-plugins\clatasha-hud\locale\en-US.ini
```

Restart OBS and open the Clatasha HUD controls from the **Tools** menu.

## Building from source

Build requirements:

- Visual Studio 2022 with **Desktop development with C++**.
- CMake 3.28 or newer.
- Git.

```powershell
git clone https://github.com/Clatasha/Clatasha-HUD.git
cd Clatasha-HUD
cmake --preset windows-x64
cmake --build --preset windows-x64
```

The first configure downloads the OBS development dependencies, so it normally takes longer than later builds.

## Privacy and overlay URLs

Clatasha HUD stores its settings locally in the OBS plugin configuration area. The plugin does not log full browser-overlay URLs. This matters because alert-service URLs can contain private tokens.

Third-party browser widgets still connect to their own service through OBS Browser Source when you configure them.

## Project status

Clatasha HUD is in active development. The core HUD, DXGI game-FPS backend, browser overlay manager, transparent alert rendering, direct-image handling, and local-file support are functional, but the project is still being refined through real OBS/game testing.
