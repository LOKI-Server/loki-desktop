# Builds LOKI Client in release mode and packs a portable Windows ZIP:
# the executable, only the wxWidgets DLLs it actually loads, the Microsoft C++
# runtime (app-local, so no redistributable install is needed), README and LICENSE.
#
#   powershell -ExecutionPolicy Bypass -File scripts\package.ps1
#
# Run it from PowerShell or a Developer Command Prompt, not an MSYS shell.
param([string]$Arch = "x64")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$version = (Select-String -Path xmake.lua -Pattern 'set_version\("([^"]+)"\)').Matches[0].Groups[1].Value
xmake f -y -p windows -a $Arch -m release
if ($LASTEXITCODE -ne 0) { throw "xmake configure failed" }
xmake build -y loki-sync
if ($LASTEXITCODE -ne 0) { throw "xmake build failed" }
$build = Join-Path $root "build\windows\$Arch\release"
$exe = Join-Path $build "loki-client.exe"

# The DLL names a PE file imports, read straight from its import directory.
function Get-Imports([string]$Path) {
    $b = [IO.File]::ReadAllBytes($Path)
    $pe = [BitConverter]::ToInt32($b, 0x3C)
    $sections = [BitConverter]::ToUInt16($b, $pe + 6)
    $optional = $pe + 24
    $magic = [BitConverter]::ToUInt16($b, $optional)
    $dirs = $optional + $(if ($magic -eq 0x20B) { 112 } else { 96 })
    $importRva = [BitConverter]::ToUInt32($b, $dirs + 8)
    $table = $optional + [BitConverter]::ToUInt16($b, $pe + 20)
    function Offset([uint32]$rva) {
        for ($i = 0; $i -lt $sections; $i++) {
            $s = $table + 40 * $i
            $va = [BitConverter]::ToUInt32($b, $s + 12)
            $size = [BitConverter]::ToUInt32($b, $s + 8)
            if ($rva -ge $va -and $rva -lt $va + $size) { return $rva - $va + [BitConverter]::ToUInt32($b, $s + 20) }
        }
        throw "RVA $rva is outside every section of $Path"
    }
    $names = @()
    if ($importRva -eq 0) { return $names }
    for ($d = Offset $importRva; [BitConverter]::ToUInt32($b, $d + 12) -ne 0; $d += 20) {
        $n = Offset ([BitConverter]::ToUInt32($b, $d + 12))
        $end = [Array]::IndexOf($b, [byte]0, [int]$n)
        $names += [Text.Encoding]::ASCII.GetString($b, $n, $end - $n)
    }
    return $names
}

# Every DLL from the build folder the executable needs, followed transitively.
$needed = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$queue = [Collections.Generic.Queue[string]]::new()
$queue.Enqueue($exe)
$runtime = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
while ($queue.Count -gt 0) {
    foreach ($dll in Get-Imports $queue.Dequeue()) {
        $local = Join-Path $build $dll
        if ((Test-Path $local) -and $needed.Add($dll)) { $queue.Enqueue($local) }
        elseif ($dll -match '^(msvcp|vcruntime|concrt)\d') { [void]$runtime.Add($dll) }
    }
}

# The newest Visual C++ redistributable runtime, which is backward compatible
# with the toolset that built the app.
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$crt = & $vswhere -all -products * -find "VC\Redist\MSVC\*\$Arch\Microsoft.VC*.CRT" |
    Sort-Object { [version](Split-Path (Split-Path (Split-Path $_ -Parent) -Parent) -Leaf) } |
    Select-Object -Last 1
if (-not $crt) { throw "No Visual C++ redistributable found; install the C++ workload in Visual Studio." }

$name = "LOKI-Client-$version-windows-$Arch"
$dist = Join-Path $root "dist"
$stage = Join-Path $dist $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $exe $stage
foreach ($dll in $needed) { Copy-Item (Join-Path $build $dll) $stage }
foreach ($dll in $runtime) {
    $source = Join-Path $crt $dll
    if (-not (Test-Path $source)) { throw "$dll is not in $crt" }
    Copy-Item $source $stage
}
Copy-Item README.md, LICENSE $stage

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip
Write-Host "Packed $zip"
Get-ChildItem $stage | ForEach-Object { Write-Host "  $($_.Name)" }
