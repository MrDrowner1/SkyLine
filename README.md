# SkyLine

SKSE plugin, built on CommonLibSSE-NG.

Adds a splash screen with the progress bar before the Skyrim window appears.

Adds a progress bar and shows what is loading while the game starts.

## Settings
Settings are in `SKSE/Plugins/SkyLine.ini`. Keys set in `SKSE/Plugins/SkyLine_Custom.ini` will override it.

| Setting | Description |
|---|---|
| `bEnabled` | turns the plugin on or off |
| `bSplash` | 0 skips the splash window |
| `iSplashWidth` | splash window width |
| `sSplashImage` | path to custom splash picture, PNG can be transparent |
| `bBarOnly` | 1 shows only the progress bar |
| `bShowDetail` | 0 hides the line with detailed info |
| `iFontSize`, `sTextColor` | text size and color |
| `sFont` | name of an installed font |
| `bTextShadow` | 0 removes the shadow behind the text |
| `bDpiScaling` | 0 ignores Windows display scaling |
| `iBarHeight`, `iBarLength`, `sBarColor` | bar size and color |
| `sPosition`, `iOffsetX`, `iOffsetY` | position and offset |
| `iLingerSeconds` | for how many seconds to keep the overlay after the game is loaded |
| `bLog` | enable/disable log |

The last start's timings and the splash position are kept in `SKSE/Plugins/SkyLine.yaml`

## Requirements
- [SKSE64](https://skse.silverlock.org/)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)

## Building
You need CMake ≥ 3.21, a C++23 MSVC or clang-cl toolchain, and vcpkg with CommonLibSSE-NG and `spdlog`. Build with
the static CRT (triplet `x64-windows-static`, or `x64-windows-static-clangcl` for clang-cl), so the DLL doesn't depend
on VC++ runtime.
```
cmake -S plugin -B build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
```

## Credits
- **Bethesda Game Studios**: Skyrim, key art in the default splash picture
- **ianpatt, behippo and the SKSE team**: [SKSE64](https://skse.silverlock.org/)
- **meh321**: [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG) and its contributors
- [spdlog](https://github.com/gabime/spdlog)
