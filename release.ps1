# Assembles a GCML release for mods to vendor: bin\release\gcml\ (see "Using a release in your mod"
# in README.md). Run after building the loader:
#   & "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" loader\GameCoreModLoader.vcxproj -p:Configuration=Release -p:Platform=x64 -m -v:m -nologo
#   powershell -File release.ps1
$ErrorActionPreference = "Stop"

$root = $PSScriptRoot
$release = Join-Path $root "bin\release\gcml"
$loader = Join-Path $root "bin\GameCore_XP2_GCML_FinalRelease.dll"
if (-not (Test-Path $loader)) {
	throw "missing: $loader (build loader\GameCoreModLoader.vcxproj first)"
}
$versionLine = Select-String -Path (Join-Path $root "loader\LoaderVersion.h") -Pattern '#define GCML_LOADER_VERSION (\d+)'
$loaderVersion = $versionLine.Matches[0].Groups[1].Value

if (Test-Path $release) {
	Remove-Item $release -Recurse -Force
}
New-Item -ItemType Directory -Force $release | Out-Null
Copy-Item $loader $release
Copy-Item (Join-Path $root "sdk\GameCorePlugin.h") $release
Copy-Item (Join-Path $root "sdk\GameCoreModLoader.sql") $release
Copy-Item (Join-Path $root "LICENSE.txt") $release
[System.IO.File]::WriteAllText((Join-Path $release "loader-version.txt"), "$loaderVersion`n")
Write-Host "released loader $loaderVersion to $release"
