# imadamnduck

A Geode mod for Geometry Dash 2.2091.

The mod adds a compact live HUD and a custom pause-menu dashboard. It records player-one jump-button events and samples player interaction-state changes after the game's logic-step hook (`processCommands`). After a short look-ahead, each input is matched to the nearest sampled interaction boundary and tallied into frame-distance buckets.

## Important accuracy note

The value shown is an **interaction-edge estimate**, not a mathematically exact physics window. It uses ground/slope, ring-contact, gravity, rotation, death, and jump-buffer state changes as timing boundaries. This is useful as a live timing inspector, but it does not prove how many alternate input ticks would still complete the level.

For exact measured windows, the established method is to record a successful run and replay it while moving one input earlier and later, one logic tick at a time, checking whether the level still completes. The separate MIT-licensed [Frame Window Lab](https://github.com/ImKubuuss/framecounterv2) project implements that replay scan for its original Geode 5.10.1 / Geometry Dash 2.2081 target. Its replay/scanning engine has **not** been ported into this 2.2091 nightly project; don't interpret imadamnduck's estimates as that level of measurement.

The counter uses a 240-tick baseline and reports buckets:

- `1`, `2`, `3`, `4`
- `5-6`, `7-8`, `9-10`, `11+`
- `UNKNOWN` when no sampled edge is found within the search radius

Input recording is observational. It does not synthesize or change gameplay input.

## UI

- **Live HUD:** screen-pinned running counts and bucket totals in the upper-left corner.
- **Pause-menu button:** press `DUCK` to open the dashboard.
- **LIVE tab:** session totals and baseline information.
- **RANGES tab:** bucket distribution.
- **HISTORY tab:** the latest classified presses/releases.
- **CLEAR:** clears the session counters.
- Coloured input markers fade after a short time.

## Target SDK

- Geometry Dash: Windows 2.2091
- Geode: latest `nightly` development SDK
- C++: C++23
- CLI: latest Geode CLI (v4+ required by the Geode v6 development line)

Geode v6 is still a developer alpha: the v6.0.0-alpha.1 release introduced 2.209x support, and the development branch has continued to change APIs since then. The GitHub workflow builds against `sdk: nightly`, so use a matching current Geode nightly loader. This project has not been compiled in this workspace because the SDK/toolchain is not installed here.

## Build locally (Windows PowerShell)

1. Install the Geode CLI using the official instructions: <https://docs.geode-sdk.org/getting-started/geode-cli>.
2. Install the latest Geode development SDK with the Geode CLI (the GitHub workflow uses `sdk: nightly`). For local builds, point `GEODE_SDK` at that nightly SDK source folder.
3. In this project folder, run:

```powershell
$env:GEODE_SDK = "C:\GeodeSDK\geode-v6"
geode build
```

Pushing this repo to GitHub starts the Windows build workflow in `.github/workflows/build.yml`.

## Install

Copy the generated `imadamnduck.framescope.geode` into your Geometry Dash Geode mods folder, then restart Geometry Dash. The pause-menu `DUCK` button opens the dashboard.

## Links

The Geode build layout follows the official Geode template and build action. Some ideas for the HUD and counters came from open-source frame-window projects, including [AutoFPCounter](https://github.com/novabot-dev/AutoFPCounter), [Frame Tracker](https://github.com/lightningstudiosx/frame_inspector), and [Frame Window Lab](https://github.com/ImKubuuss/framecounterv2). No source files, art, or audio from those projects are bundled here. Frame Window Lab is linked as a reference for the more rigorous replay-scan approach; its code is not included in imadamnduck.

## Known limitations

- Windows / Geometry Dash 2.2091 is the declared target; other platforms are not declared.
- Only player-one jump-button presses are tracked by default; releases can be included with the `Count releases` setting (off by default).
- A player respawn that replaces the `PlayerObject` resets the state-boundary timeline for that attempt.
- Because the tracker is observational, it cannot discover every possible physics interaction edge or measure alternate-input success windows.
- For true frame-window results, use a replay-based scanner such as the linked Frame Window Lab project, after verifying its SDK / Geometry Dash version compatibility.
