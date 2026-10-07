# Retro-Towns

An FM TOWNS emulator for Android and Linux, built on the Tsugaru core behind a
shared SDL3 + Dear ImGui frontend.

The machine shapes the app: an FM TOWNS is a home computer that boots into an
operating system, so the launcher is a shelf of disc/floppy images and the
machine is a separate place you switch on — not a single screen wrapped around
one disc. Its BIOS is a directory of Fujitsu ROMs rather than one image, so the
first-run conversation is a setup wizard that finds that folder and the games
inside it.

## Project layout

```
Retro-Towns/
├── frontend/          SDL3 + Dear ImGui frontend (shared Linux/Android)
│   ├── towns_app.cpp  window, main loop, shelf, launcher, machine view
│   ├── towns_config.* key=value settings
│   ├── towns_library.* disc shelf scanner
│   ├── towns_setup.*  first-run wizard, SAF bridge, BIOS checklist
│   ├── towns_keys.*   FM Towns keyboard (host + on-screen)
│   ├── towns_pad_map.* external pad binding
│   └── touch_pad.*    on-screen controller
├── core/              Tsugaru FM TOWNS core (GPL-3.0), plus the retro/ bridge
│   └── retro/bridge/  ftowns_* C ABI between the frontend and the core
├── android/           Gradle project (SDLActivity + prebuilt .so's)
├── tool/build-linux.sh    desktop build
└── android/build-core.sh  Android native build (core + SDL3 + frontend)
```

## Build

**Linux:**

```sh
./tool/build-linux.sh
# -> ~/.cache/retro-restructure/retrotowns-linux/retrotowns
```

Needs a C++17 compiler, `sdl3` via pkg-config, and (optionally) minizip and
libcurl for zipped rips and RetroMedia.

**Android:**

```sh
ANDROID_ABI=arm64-v8a ./android/build-core.sh
cd android && ./gradlew assembleDebug
```

Needs the NDK (28.2.13676358), a C++17 toolchain, and the Android SDK. The
frontend reads Dear ImGui from `core/vendor/imgui/imgui`, falling back to the
Retro-Saturn checkout when it is not vendored here.

## License

The frontend, bridge and build scripts are Crown Park Computing Ltd,
GPL-3.0-or-later. The Tsugaru core is BSD-3-Clause (see `core/LICENSE`).
