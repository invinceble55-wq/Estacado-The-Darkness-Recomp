# Building Estacado from source

You build the port on your own PC from your own copy of the game. The
recompiled code (`generated/ppc`) and the executable built from it are
derived from the game: keep them for your own use.

## What you need

- Windows 10 or 11, 64-bit, a CPU with AVX2, about 30 GB of free disk space.
- **Visual Studio 2022** (Community is fine) or the **Build Tools for Visual
  Studio 2022**, with:
  - "Desktop development with C++" (MSVC v143 toolset 14.44 or newer, Windows
    11 SDK), and
  - "C++ Clang tools for Windows" (clang-cl 18 or newer).
- **CMake 3.25 or newer** and **Ninja** (the Visual Studio installation
  includes both; `winget install Kitware.CMake Ninja-build.Ninja` also
  works).
- **Git** (with long paths enabled: `git config --global core.longpaths true`).
- An internet connection for the first build (the submodules are
  downloaded).
- Your game: *The Darkness*, Xbox 360, USA/Europe disc, as a disc image
  (`.iso`) or as the extracted files (see README, *Game files*).

## One command

In a PowerShell window:

```powershell
git clone --recursive https://github.com/invinceble55-wq/Estacado-The-Darkness-Recomp.git
cd Estacado-The-Darkness-Recomp
powershell -ExecutionPolicy Bypass -File scripts\build-all.ps1 -Game "D:\Path\To\Darkness.iso" -Package build\package
```

This:

1. checks that your `default.xex` is the supported version (SHA-256
   `aace35a8...3c5f`) and extracts a disc image next to the repository
   (a folder named `Darkness, The (USA, Europe) (En,Fr,De,Es,It)`), or links
   an extracted folder there;
2. builds XenonRecomp, analyses your `default.xex` and recompiles it into
   `generated/ppc`;
3. builds ReXGlue (`rexruntime.dll`, `rexgpu-xenos.dll`), the runtime
   (`TheDarkness.exe`, `TheDarknessSettings.exe`) and the tests, and runs the
   tests;
4. with `-Package <folder>`, writes a ready-to-play folder (`-Zip` also makes
   a zip for your own use).

Then start `TheDarknessSettings.exe` from the package (or from
`build\runtime`). The first build takes a while (the recompiled code is
large); later builds are incremental.

### Shelved: temporal AA and upscalers

Temporal anti-aliasing and the vendor upscalers (NVIDIA DLAA, AMD FSR 3.1,
Intel XeSS) are shelved in this release: the code is kept, but the settings
stay hidden and a saved choice is ignored unless the environment variable
`DARKNESS_EXPERIMENTAL=temporal_aa` is set when the launcher or the game
starts. The vendor SDKs are not part of this repository;
`scripts\build-all.ps1 ... -FetchSdks -AcceptSdkLicenses` downloads them
(after you read their licences) for such builds. Their licences apply to
builds you share.

## Pieces

| Script | What it does |
|---|---|
| `scripts/build-all.ps1` | everything above |
| `scripts/build.ps1`, `configure.ps1` | XenonRecomp and XenonAnalyse |
| `scripts/analyse.ps1` | switch tables of your XEX (`config/darkness_switch_tables.toml`) |
| `scripts/recomp-switch-correction.ps1` | recompilation with `config/darkness_recomp_switch_correction.toml` |
| `scripts/build-rexglue.ps1` | ReXGlue (`-NoPgo` builds without the GPU PGO profile) |
| `scripts/fetch-sdks.ps1` | vendor SDKs for the shelved temporal AA |
| `scripts/package.ps1` | player package from the build outputs |
| `tools/game_setup` | disc image check and extraction (also in the launcher) |

