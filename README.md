# Clatasha HUD

Clatasha HUD is a Windows-first native OBS Studio plugin prototype for a local-only gaming HUD.

## v0.1 proof of concept

- Loads inside OBS Studio. No Python process is required.
- Adds `Tools > Clatasha HUD` to toggle the HUD.
- Creates a frameless, always-on-top, click-through Qt HUD.
- Shows OBS recording and streaming state with session timers.
- Starts and stops with OBS.

## Planned next steps

1. Windows capture exclusion so OBS recordings/streams do not contain the HUD.
2. OBS render FPS, bitrate, CPU and dropped-frame statistics.
3. Real-time microphone and desktop-audio meters.
4. HUD position, opacity, scale and enable/disable settings.
5. Warning states such as muted microphone or stopped recording.
6. Optional true game-FPS integration later.

## Build requirements

- Windows 10/11 x64
- Visual Studio 2022 with **Desktop development with C++**
- CMake 3.28 or newer
- Git

The build scaffolding is based on the official `obsproject/obs-plugintemplate`. The current official template pins OBS SDK/dependencies to OBS 31.1.1; v0.1 intentionally keeps that known-good template pin for the first compile/load test.

## Build

```powershell
git clone https://github.com/derspawn/Clatasha-HUD.git
cd Clatasha-HUD
cmake --preset windows-x64
cmake --build --preset windows-x64
```

The first configure downloads/builds the OBS development dependencies, so it takes longer than later builds.

## Status

Early prototype. Do not treat v0.1 as a finished or capture-safe HUD yet.
