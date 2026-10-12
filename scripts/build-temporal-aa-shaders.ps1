[CmdletBinding()]
param()
# Compiles the temporal AA resolve (external/ReXGlue/src/graphics/shaders/
# temporal_aa.cs.hlsl) into the D3D12 bytecode header the GPU plugin embeds:
# temporal_aa_resolve_cs.h (shaders::temporal_aa_resolve_cs). Uses the pinned
# Windows SDK FXC like the other embedded shaders.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$fxc = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.22621.0\x64\fxc.exe'
if (!(Test-Path -LiteralPath $fxc)) { throw 'Pinned Windows SDK FXC unavailable' }
$source = Join-Path $root 'external/ReXGlue/src/graphics/shaders/temporal_aa.cs.hlsl'
$out = Join-Path $root 'external/ReXGlue/src/graphics/shaders/bytecode/d3d12_5_1'
& $fxc /nologo /T cs_5_1 /E main /O3 /Vn temporal_aa_resolve_cs /Fh (Join-Path $out 'temporal_aa_resolve_cs.h') $source
if ($LASTEXITCODE -ne 0) { throw 'FXC failed for temporal_aa_resolve_cs' }
# Temporal upscaling of the finished frame (#3, #23): two entry points.
$upscale = Join-Path $root 'external/ReXGlue/src/graphics/shaders/temporal_upscale.cs.hlsl'
foreach ($entry in 'prepare', 'composite', 'vr_hud', 'vr_preview') {
    $name = "temporal_upscale_${entry}_cs"
    & $fxc /nologo /T cs_5_1 /E $entry /O3 /Vn $name /Fh (Join-Path $out "$name.h") $upscale
    if ($LASTEXITCODE -ne 0) { throw "FXC failed for $name" }
}
