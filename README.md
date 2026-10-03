<p align="center">
  <img src="assets/Clatasha%20Hud%20OBS.png" width="360" alt="Clatasha HUD logo">
</p>

# Clatasha HUD

Clatasha HUD is a Windows OBS Studio plugin that adds a compact, always-on-top status HUD for the streamer and a five-slot browser overlay manager for HUD-only, stream-only, or combined overlays.
<p align="center">
  <img src="assets/Clatasha%20HUD%20Status%20overlay2.png" width="360" alt="Clatasha HUD logo">
</p>

The plugin runs inside OBS Studio. It does not require a separate Python process.

### Current features
<p align="center">
  <img src="assets/clatasha%20hud%20image2.jpg" width="500" alt="Clatasha HUD logo">
</p>

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
- Visibility watchdog that restores the main HUD if Windows unexpectedly hides or drops its topmost state.
- Windows capture-exclusion request for local HUD windows where supported.

If game FPS cannot be read, Clatasha HUD displays `--` rather than substituting OBS renderer FPS.
<p align="center">
  <img src="assets/clatasha%20hud%20image.jpg" width="500" alt="Clatasha HUD logo">
</p>

### Browser Overlays

Clatasha HUD includes five configurable overlay slots. Each slot has:

- Enable/disable control.
- Friendly name.
- URL or local-file input.
- `HUD`, `VIDEO`, or `HUD / VIDEO` output mode.
- Preview/Edit placement tool with drag and resize handles.
- Separate source resolution and displayed size, so resizing scales the overlay instead of changing/cropping the browser viewport.
- **Lock Ratio** for HUD and VIDEO placement, enabled by default.
- Independent saved HUD and VIDEO placement/size.
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

### Input HUD

Enable the Input HUD in Clatasha HUD Settings → Advanced, choose its keys and mouse controls, then select **HUD**, **VIDEO**, or **BOTH**. VIDEO adds a live **Clatasha Input HUD** source to the current OBS scene; BOTH also keeps the local capture-excluded display. You can move and scale the source in the OBS preview. The Scale slider keeps the OBS source's bottom edge in place as it grows upward.

Assign **Show/Hide Input HUD** in Clatasha HUD Settings → Hotkeys or OBS Settings → Hotkeys. This shortcut toggles the input display in the selected output mode and starts unassigned. It preserves the OBS source's placement when toggled.

### Game display mode compatibility

Clatasha HUD uses Windows desktop overlay windows for the local status HUD, Input HUD, and browser HUD overlays.

- **Windowed and borderless games** — use the desktop HUD, with Windows capture exclusion where supported.
- **Exclusive fullscreen OpenGL and Direct3D 11 games** — the status HUD can render through an injected present hook. This in-game HUD can appear in recordings and streams. Support depends on the game and its rendering path.
- **Other exclusive fullscreen renderers** — desktop overlays may be covered by the game. Use borderless mode if the HUD is not visible.

**Show/Hide HUD** controls both the desktop status HUD and the injected in-game status HUD. Input HUD and browser HUD overlays have their own visibility shortcuts.

The fullscreen status HUD support does not extend to Input HUD or browser HUD overlays. VIDEO overlays remain normal OBS scene sources regardless of the game's display mode.

## Game FPS backend

Game/application FPS is collected by `clatasha-fps-helper.exe` using the Windows DXGI ETW provider and injected OpenGL/Direct3D 11 present hooks. The helper is launched elevated because starting the ETW session normally requires suitable Windows tracing permissions.

Current limitations:

- Unsupported rendering paths, including some Vulkan titles, may show `--`.
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
      clatasha-opengl-present-hook.dll
      clatasha-d3d11-present-hook.dll
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
C:\Program Files\obs-studio\data\obs-plugins\clatasha-hud\clatasha-opengl-present-hook.dll
C:\Program Files\obs-studio\data\obs-plugins\clatasha-hud\clatasha-d3d11-present-hook.dll
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

## Copyright and third-party software

Copyright © 2011–2026 Clatasha. All rights reserved.
