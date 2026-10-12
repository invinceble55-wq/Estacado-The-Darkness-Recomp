# Third-party notices

Estacado's own source code is under the MIT licence (LICENSE). It builds on
and ships with the components below, each under its own licence. The full
licence texts are in the `licenses` folder of a release package and in each
component's source.

## Source components (built from source)

| Component | Licence | Where | In the release binaries |
|---|---|---|---|
| ReXGlue SDK (Tom Clay), with code derived from Xenia (Ben Vanik and the Xenia project contributors), plus this project's changes | BSD-3-Clause | `external/ReXGlue` (fork) | `rexruntime.dll`, `rexgpu-xenos.dll`; the settings panel in `TheDarkness.exe` and `TheDarknessSettings.exe` |
| Parts of this project's runtime (kernel and file-system contracts) modelled on Xenia | BSD-3-Clause (Xenia notice kept) | `runtime/` | `TheDarkness.exe` |
| XenonRecomp / XenonUtils (hedge-dev and contributors) | MIT | `external/XenonRecomp` (fork) | `TheDarkness.exe` (XEX loader); the recompiler at build time |
| libmspack (Stuart Caie) - LZX decoder | LGPL-2.1 | ReXGlue and XenonRecomp `thirdparty/libmspack` | `rexruntime.dll`; `mspack_lzx.dll` (a separate library the game executable loads, replaceable with your own build) |
| FFmpeg (libavcodec, libavutil subset) | LGPL-2.1-or-later | ReXGlue `thirdparty/FFmpeg` | `rexruntime.dll` |
| SDL 3 | zlib | ReXGlue `thirdparty/sdl3` | `rexruntime.dll`, `TheDarknessSettings.exe` |
| Dear ImGui (including the ProggyClean/ProggyTiny font data) | MIT | ReXGlue `thirdparty/imgui` | `rexruntime.dll`, `TheDarknessSettings.exe` |
| toml++ | MIT | `thirdparty/tomlplusplus` | all executables |
| SIMDe | MIT | `thirdparty/simde` | all executables |
| fmt | MIT | ReXGlue `thirdparty/fmt` | `rexruntime.dll` |
| spdlog | MIT | ReXGlue `thirdparty/spdlog` | `rexruntime.dll` |
| xxHash | BSD-2-Clause | `thirdparty/xxhash` | `rexruntime.dll`, `rexgpu-xenos.dll` |
| Snappy (Google) | BSD-3-Clause | ReXGlue `thirdparty/snappy` | `rexgpu-xenos.dll` |
| o1heap | MIT | ReXGlue `thirdparty/o1heap` | `rexruntime.dll` |
| disruptorplus | MIT | ReXGlue `thirdparty/disruptorplus` | `rexruntime.dll` |
| aes_128 (LuoPeng), DES (F. Fallahi), picosha2 (okdshin) | MIT | ReXGlue `thirdparty` | `rexruntime.dll` |
| tiny-AES-c | Unlicense | XenonRecomp `thirdparty/tiny-AES-c` | `TheDarkness.exe` |
| TinySHA1 (S. Mohapatra) | ISC-style | XenonRecomp `thirdparty/TinySHA1` | build only |
| utfcpp | BSL-1.0 | ReXGlue `thirdparty/utfcpp` | `rexruntime.dll` |
| stb_image | MIT / public domain | ReXGlue `thirdparty/stb` | `rexruntime.dll`, `rexgpu-xenos.dll` |
| SMAA (Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez) | MIT | ReXGlue `src/graphics/shaders/smaa` | `rexgpu-xenos.dll` |
| AMD FidelityFX CAS and FSR 1 shaders (in Xenia's presenter) | MIT | ReXGlue `src/ui/shaders` | `rexruntime.dll` |
| DXBC checksum (AMD; inherited from Xenia, no licence text upstream) | see source | ReXGlue `thirdparty/dxbc` | `rexgpu-xenos.dll` |
| AMD FidelityFX SDK (FSR 3.1 upscaler runtime, signed prebuilt) | MIT | `licenses/AMD_FidelityFX_SDK_LICENSE.txt` | `amd_fidelityfx_dx12.dll` (unmodified) |
| Intel XeSS SDK (upscaler runtime) | Intel Simplified Software License | `licenses/Intel_XeSS_SDK_LICENSE.txt` | `libxess.dll` (unmodified) |
| NVIDIA DLSS SDK (DLSS Super Resolution runtime and the NVIDIA NGX code it is loaded through) | NVIDIA RTX SDKs License (proprietary; NOT covered by this project's MIT licence) | `licenses/NVIDIA_DLSS_SDK_LICENSE.txt`, `licenses/NVIDIA_DLSS_END_USER_TERMS.txt` | `nvngx_dlss.dll` (unmodified) and the NGX code linked into `rexgpu-xenos.dll` |

The LGPL components are used unmodified. Their complete source is available
at the locations above (and from their upstream projects); the release
package points to the exact revisions.

Tools used only at build time and never shipped: the binutils PowerPC
disassembler (GPL-2.0-or-later, in XenonRecomp and ReXGlue `thirdparty/disasm`),
CLI11, inja, Catch2, glslang, SPIRV tools and headers, Vulkan headers, volk
and VMA (their own permissive licences).

AMD, FidelityFX and FSR are trademarks of Advanced Micro Devices, Inc. Intel and XeSS are trademarks of Intel Corporation.
This product uses NVIDIA DLSS. NVIDIA, the NVIDIA logo, GeForce, GeForce RTX, RTX and DLSS are
trademarks and/or registered trademarks of NVIDIA Corporation in the U.S. and other countries.
The NVIDIA components are licensed by NVIDIA under its own terms; this project's open-source
licence does not apply to them, and they may only be used as part of this application.
