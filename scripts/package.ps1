[CmdletBinding()]
param(
    # Output folder (created; must not exist).
    [Parameter(Mandatory)][string]$Output,
    # Version text for the package manifest and the zip name.
    [string]$Version = 'dev',
    # Also write <Output>.zip.
    [switch]$Zip,
    # Ready-made player package: the ReXGlue DLLs from the player build
    # (scripts/build-rexglue.ps1 -Player: no DLSS SDK, no paths of this
    # machine); NVIDIA DLSS is then left out.
    [switch]$Player,
    # Which SDK runtimes to include when their DLLs are present (dlss, fsr, xess;
    # none: no upscaler runtime, as in the public release while temporal AA is
    # shelved).
    [string[]]$Sdk = @('dlss', 'fsr', 'xess'),
    # The name for the documents' Estacado (default: DARKNESS_PRODUCT_NAME
    # from runtime/product_name.h, the name the programs show).
    [string]$DisplayName = '',
    # An optional language pack archive (scripts/arabic_language_pack.py
    # --asset) carried as language_packs/arabic_language_pack.zip: the
    # launcher installs it in one click.
    [string]$LanguagePack = '',
    # Extra documents for the package root (for example a tester guide).
    [string[]]$ExtraFile = @(),
    # 0.9.1 (#5): a folder with the data-free pipeline list - <title>.xshi
    # (shader hashes, sizes and a hash of their first bytes),
    # <title>.rtv.d3d12.xpso (pipeline descriptions keyed by shader hashes)
    # and <title>.xshp (which ProgramCache shader each one is, plus the
    # vertex-fetch bindings the console's Direct3D patches in), no shader
    # code - carried as runtime_data/pipeline_seed, so the first start
    # rebuilds the shaders from the player's own ProgramCache.xpc and
    # compiles every pipeline before it is needed.
    [string]$PipelineList = ''
)
# Builds a player package from the build outputs: the executables, the ReXGlue
# DLLs, the optional SDK runtime DLLs, presets, example configurations and the
# licence texts. Nothing derived from the game other than the recompiled
# executable goes in: no shader code or caches (the optional pipeline list
# names shaders by hash only), no replacement packs, no
# saves, settings, logs or installed language packs (a pack archive holds
# only our own files and patches). A manifest with SHA-256 hashes of our own
# runtime files lets the runtime check them at every start and with
# TheDarkness.exe --verify-package; the vendor SDK DLLs (players may update
# them), the documents and the licence texts are not in it.
. "$PSScriptRoot/_common.ps1"
$root = Get-ProjectRoot
$runtime = Join-Path $root 'build/runtime'
$rexglue = if ($Player) { Join-Path $root 'external/ReXGlue/out/win-amd64-player/Release' } else { $runtime }
$deps = Join-Path $root 'build/deps'
if (-not $DisplayName) {
    $nameHeader = Get-Content -Raw -LiteralPath (Join-Path $root 'runtime/product_name.h')
    if ($nameHeader -notmatch '#define DARKNESS_PRODUCT_NAME "([^"]+)"') { throw 'runtime/product_name.h has no DARKNESS_PRODUCT_NAME' }
    $DisplayName = $Matches[1]
}
# "-Sdk fsr,xess" arrives as one string through powershell -File.
$Sdk = @($Sdk | ForEach-Object { "$_" -split ',' } | ForEach-Object { $_.Trim().ToLowerInvariant() } |
    Where-Object { $_ })
if ($Sdk -contains 'none') { $Sdk = @() }
foreach ($item in $Sdk) {
    if ($item -notin 'dlss', 'fsr', 'xess') { throw "Unknown SDK '$item' (use dlss, fsr, xess)." }
}
# 0.9.8+: player packages may carry DLSS when asked for (-Sdk dlss,...).
$out = [IO.Path]::GetFullPath($Output)
# Player documents: release/ in the development tree, the repository root in
# the public source.
$docs = Join-Path $root 'release'
if (-not (Test-Path -LiteralPath (Join-Path $docs 'LICENSE') -PathType Leaf)) { $docs = $root }
if (Test-Path -LiteralPath $out) { throw "$out already exists" }

$files = [Collections.Generic.List[object]]::new()
function Add-File([string]$Source, [string]$Relative, [switch]$Optional, [switch]$Unchecked) {
    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        if ($Optional) { return }
        throw "missing: $Source"
    }
    $target = Join-Path $out $Relative
    New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath $Source -Destination $target
    $files.Add([pscustomobject]@{ Path = $Relative.Replace('\', '/'); Target = $target; Checked = -not $Unchecked })
}

# Executables and runtime libraries.
foreach ($name in 'TheDarkness.exe', 'TheDarknessSettings.exe') {
    Add-File (Join-Path $runtime $name) $name
}
foreach ($name in 'rexruntime.dll', 'rexgpu-xenos.dll') {
    Add-File (Join-Path $rexglue $name) $name
}
Add-File (Join-Path $runtime 'mspack_lzx.dll') 'mspack_lzx.dll' -Optional
Add-File (Join-Path $runtime 'TheDarkness.pc.example.toml') 'TheDarkness.pc.example.toml'
Add-File (Join-Path $runtime 'TheDarkness.mods.example.toml') 'TheDarkness.mods.example.toml'
foreach ($preset in Get-ChildItem -LiteralPath (Join-Path $runtime 'presets') -Filter '*.toml') {
    Add-File $preset.FullName ('presets/' + $preset.Name)
}
# Interface languages (one file per language, ui_language.h).
foreach ($language in Get-ChildItem -LiteralPath (Join-Path $runtime 'languages') -Filter '*.toml') {
    Add-File $language.FullName ('languages/' + $language.Name)
}

# Optional SDK runtimes (present only when the build had the SDKs).
$sdkRuntime = @(
    # (The leading comma keeps a single [source, name] pair as one entry.)
    @{ Id = 'dlss'; Dll = 'DLSS/lib/Windows_x86_64/rel/nvngx_dlss.dll'; Name = 'nvngx_dlss.dll'
       Licences = @(,@('DLSS/LICENSE.txt', 'NVIDIA_DLSS_SDK_LICENSE.txt')) }
    @{ Id = 'fsr'; Dll = 'FidelityFX-SDK/PrebuiltSignedDLL/amd_fidelityfx_dx12.dll'; Name = 'amd_fidelityfx_dx12.dll'
       Licences = @(,@('FidelityFX-SDK/LICENSE.txt', 'AMD_FidelityFX_SDK_LICENSE.txt')) }
    @{ Id = 'xess'; Dll = 'XeSS/bin/libxess.dll'; Name = 'libxess.dll'
       Licences = @(@('XeSS/LICENSE.txt', 'Intel_XeSS_SDK_LICENSE.txt'),
                    @('XeSS/third-party-programs.txt', 'Intel_XeSS_third-party-programs.txt')) }
)
foreach ($vendor in $sdkRuntime) {
    if ($vendor.Id -notin $Sdk) { continue }
    $dll = Join-Path $deps $vendor.Dll
    if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) { continue }
    Add-File $dll $vendor.Name -Unchecked
    foreach ($licence in $vendor.Licences) {
        Add-File (Join-Path $deps $licence[0]) ('licenses/' + $licence[1]) -Unchecked
    }
}
if (Test-Path -LiteralPath (Join-Path $out 'nvngx_dlss.dll')) {
    Add-File (Join-Path $docs 'licenses/NVIDIA_DLSS_END_USER_TERMS.txt') 'licenses/NVIDIA_DLSS_END_USER_TERMS.txt' -Unchecked
}

# Our licence, the notices and the component licences.
Add-File (Join-Path $docs 'LICENSE') 'licenses/LICENSE.txt' -Unchecked
Add-File (Join-Path $docs 'THIRD_PARTY_NOTICES.md') 'licenses/THIRD_PARTY_NOTICES.md' -Unchecked
Add-File (Join-Path $docs 'README.md') 'README.md' -Unchecked
Add-File (Join-Path $docs 'CHANGELOG.md') 'CHANGELOG.md' -Optional -Unchecked
$rex = Join-Path $root 'external/ReXGlue'
$xenon = Join-Path $root 'external/XenonRecomp'
$componentLicences = @(
    @("$rex/LICENSE", 'ReXGlue_and_Xenia_BSD-3-Clause.txt'),
    @("$xenon/LICENSE.md", 'XenonRecomp_MIT.txt'),
    @("$rex/thirdparty/libmspack/libmspack/COPYING.LIB", 'libmspack_LGPL-2.1.txt'),
    @("$rex/thirdparty/FFmpeg/COPYING.LGPLv2.1", 'FFmpeg_LGPL-2.1.txt'),
    @("$rex/thirdparty/FFmpeg/LICENSE.md", 'FFmpeg_LICENSE.md'),
    @("$rex/thirdparty/sdl3/LICENSE.txt", 'SDL3_zlib.txt'),
    @("$rex/thirdparty/imgui/LICENSE.txt", 'DearImGui_MIT.txt'),
    @("$rex/thirdparty/tomlplusplus/LICENSE", 'tomlplusplus_MIT.txt'),
    @("$rex/thirdparty/simde/COPYING", 'SIMDe_MIT.txt'),
    @("$rex/thirdparty/fmt/LICENSE", 'fmt_MIT.txt'),
    @("$rex/thirdparty/spdlog/LICENSE", 'spdlog_MIT.txt'),
    @("$rex/thirdparty/xxHash/LICENSE", 'xxHash_BSD-2-Clause.txt'),
    @("$rex/thirdparty/snappy/COPYING", 'Snappy_BSD-3-Clause.txt'),
    @("$rex/thirdparty/o1heap/LICENSE", 'o1heap_MIT.txt'),
    @("$rex/thirdparty/utfcpp/LICENSE", 'utfcpp_BSL-1.0.txt'),
    @("$rex/src/graphics/shaders/smaa/LICENSE.txt", 'SMAA_MIT.txt')
)
foreach ($licence in $componentLicences) {
    Add-File $licence[0] ('licenses/' + $licence[1]) -Optional -Unchecked
}
# The documents' placeholders.
foreach ($document in 'README.md', 'CHANGELOG.md', 'licenses/LICENSE.txt', 'licenses/THIRD_PARTY_NOTICES.md') {
    $path = Join-Path $out $document
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
    $text = [IO.File]::ReadAllText($path)
    $text = $text.Replace('Estacado', $DisplayName).Replace('Download the zip from the [Releases](../../releases) page and extract it to a
folder of your choice (not inside *Program Files*). Or build it yourself: see
[BUILDING.md](BUILDING.md).',
        "Extract the zip to a folder of your choice (not inside *Program Files*).")
    # The screenshots live in the repository, not in the package.
    $text = [regex]::Replace($text, '(?ms)^## Screenshots\r?\n.*?(?=^## )', '')
    if ($text -match '\{\{[A-Z_]+\}\}') { throw "unfilled placeholder in $document" }
    [IO.File]::WriteAllText($path, $text, [Text.UTF8Encoding]::new($false))
}
if ($LanguagePack) {
    Add-File ([IO.Path]::GetFullPath($LanguagePack)) 'language_packs/arabic_language_pack.zip' -Unchecked
}
if ($PipelineList) {
    $listRoot = [IO.Path]::GetFullPath($PipelineList)
    foreach ($name in '545407EE.xshi', '545407EE.xshp', '545407EE.rtv.d3d12.xpso') {
        Add-File (Join-Path $listRoot $name) ('runtime_data/pipeline_seed/' + $name)
    }
}
foreach ($extra in $ExtraFile) {
    $source = [IO.Path]::GetFullPath($extra)
    Add-File $source ([IO.Path]::GetFileName($source)) -Unchecked
}

# Manifest (the runtime's package check).
$manifest = [Text.StringBuilder]::new()
[void]$manifest.AppendLine("# $Version player package; excludes writable content, caches and game files.")
[void]$manifest.AppendLine('package_schema_version = 1')
[void]$manifest.AppendLine("package_version = `"$Version`"")
foreach ($file in $files | Where-Object Checked) {
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.Target).Hash
    [void]$manifest.AppendLine('')
    [void]$manifest.AppendLine('[[file]]')
    [void]$manifest.AppendLine("path = `"$($file.Path)`"")
    [void]$manifest.AppendLine("sha256 = `"$hash`"")
}
[IO.File]::WriteAllText((Join-Path $out 'TheDarkness.package.toml'), $manifest.ToString(),
    [Text.UTF8Encoding]::new($false))

# Nothing derived from the game besides TheDarkness.exe and the data-free
# pipeline list (hashes and render states, no shader code).
$allowedList = @('runtime_data/pipeline_seed/545407EE.xshi', 'runtime_data/pipeline_seed/545407EE.xshp',
                 'runtime_data/pipeline_seed/545407EE.rtv.d3d12.xpso')
$forbidden = @('*.xex', '*.xsrp', '*.xsh', '*.xpso', '*.xfc', '*.xcd', '*.xtc', '*.xdf', '*.iso',
               'TheDarkness.pc.toml')
foreach ($pattern in $forbidden) {
    $hit = @(Get-ChildItem -LiteralPath $out -Recurse -File -Filter $pattern | Where-Object {
        $relative = $_.FullName.Substring($out.Length + 1).Replace('\', '/')
        $relative -notin $allowedList })
    if ($hit.Count) { throw "package contains $pattern" }
}
if (Test-Path -LiteralPath (Join-Path $out 'runtime_data')) {
    $extraData = @(Get-ChildItem -LiteralPath (Join-Path $out 'runtime_data') -Recurse -File | Where-Object {
        $_.FullName.Substring($out.Length + 1).Replace('\', '/') -notin $allowedList })
    if ($extraData.Count) { throw "package contains runtime_data beyond the pipeline list: $($extraData[0].Name)" }
}
# A player package names no path of this machine (the repository, the user
# profile) in any file: ASCII and UTF-16 texts inside the binaries included.
if ($Player) {
    $needles = @($root, $root.Replace('\', '/'), $env:USERPROFILE, $env:USERPROFILE.Replace('\', '/'),
                 $env:COMPUTERNAME) | Where-Object { $_ } | Select-Object -Unique
    foreach ($file in Get-ChildItem -LiteralPath $out -Recurse -File) {
        $bytes = [IO.File]::ReadAllBytes($file.FullName)
        $ascii = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
        $wide = [Text.Encoding]::Unicode.GetString($bytes)
        foreach ($needle in $needles) {
            if ($ascii.IndexOf($needle, [StringComparison]::OrdinalIgnoreCase) -ge 0 -or
                $wide.IndexOf($needle, [StringComparison]::OrdinalIgnoreCase) -ge 0) {
                throw "package file $($file.Name) contains a path or name of this machine: $needle"
            }
        }
    }
}
Write-Host "package: $out ($($files.Count) files)"
if ($Zip) {
    $zipPath = "$out.zip"
    if (Test-Path -LiteralPath $zipPath) { throw "$zipPath already exists" }
    # bsdtar (Windows 10 1803+) writes folder entries and Unix permissions, so
    # Linux archive tools (Steam Deck) extract the presets and licenses folders
    # readable (#2); Compress-Archive is the fallback.
    $bsdtar = Join-Path $env:SystemRoot 'System32\tar.exe'
    if (Test-Path -LiteralPath $bsdtar -PathType Leaf) {
        $items = @(Get-ChildItem -LiteralPath $out -Name)
        & $bsdtar -a -c -f $zipPath -C $out @items
        if ($LASTEXITCODE -ne 0) { throw "tar.exe failed with exit code $LASTEXITCODE" }
    } else {
        Compress-Archive -Path (Join-Path $out '*') -DestinationPath $zipPath
    }
    Write-Host "zip: $zipPath"
}
