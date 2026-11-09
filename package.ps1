# Packages the built DLL into a mod-manager-ready archive:
#   SKSE/Plugins/InputLayoutGuard.dll
#   SKSE/Plugins/InputLayoutGuard.ini
#   SKSE/Plugins/InputLayoutGuard/LICENSE.txt, THIRD_PARTY_NOTICES.txt, SOURCE.txt
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$version = (Get-Content vcpkg.json -Raw | ConvertFrom-Json).'version-string'
$dll = 'build\Release\InputLayoutGuard.dll'
if (-not (Test-Path $dll)) { throw "$dll not found - build first" }

$root = 'dist\package'
if (Test-Path $root) { Remove-Item $root -Recurse -Force }
$plugins = Join-Path $root 'SKSE\Plugins'
$docs = Join-Path $plugins 'InputLayoutGuard'
New-Item -ItemType Directory -Force $docs | Out-Null

Copy-Item $dll $plugins
Copy-Item 'dist\InputLayoutGuard.ini' $plugins
Copy-Item 'LICENSE' (Join-Path $docs 'LICENSE.txt')
Copy-Item 'THIRD_PARTY_NOTICES.txt' $docs
Set-Content (Join-Path $docs 'SOURCE.txt') -Encoding ascii -Value @(
    "Input Layout Guard $version - Copyright (C) 2026 neoscox"
    'Licensed under the GNU General Public License v3.0 or later (see LICENSE.txt).'
    ''
    'Complete corresponding source code for this build:'
    "https://github.com/neoscox/InputLayoutGuard/tree/v$version"
)

$zip = Join-Path $PSScriptRoot "InputLayoutGuard-$version.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
# Write entries with '/' separators: Windows PowerShell 5.1's Compress-Archive uses '\', which some mod managers misread.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$base = (Resolve-Path $root).Path.TrimEnd('\') + '\'
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($f in Get-ChildItem $root -Recurse -File) {
        $entry = $f.FullName.Substring($base.Length).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally {
    $archive.Dispose()
}
Write-Host "=== packaged $zip ==="
