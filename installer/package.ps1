# Second half of package.cmd: copies the built files into dist\ and zips them.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
$version = (Get-Item "$root\host-windows\build\ScreenFleet.exe").VersionInfo.ProductVersion
$setup = "$PSScriptRoot\build\ScreenFleet-$version-setup.exe"
$apk = "$root\android\app\build\outputs\apk\release\app-release.apk"
if (-not (Test-Path $setup)) { throw "missing $setup" }
if (-not (Test-Path $apk)) { throw "no signed release APK (is the release key set up? see android/app/build.gradle)" }

$apkVersion = (Get-Content "$root\android\app\build\outputs\apk\release\output-metadata.json" | ConvertFrom-Json).elements[0].versionName
if ($apkVersion -ne $version) { throw "desktop is $version but Android is $apkVersion" }

$dist = "$PSScriptRoot\dist"
$out = "$dist\ScreenFleet-$version"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force $out | Out-Null

Copy-Item $setup "$out\ScreenFleet-$version-setup.exe"
Copy-Item $apk "$out\ScreenFleet-$version.apk"
Copy-Item "$root\LICENSE" "$out\LICENSE.txt"
Copy-Item "$root\assets\fonts\OFL.txt" "$out\OFL.txt"
# Windows line endings, so Notepad on any Windows version shows them right.
$readme = (Get-Content "$PSScriptRoot\README.txt" -Raw).Replace('{VERSION}', $version) -replace "`r?`n", "`r`n"
[IO.File]::WriteAllText("$out\README.txt", $readme)

$sums = foreach ($f in "ScreenFleet-$version-setup.exe", "ScreenFleet-$version.apk") {
    "$((Get-FileHash "$out\$f" -Algorithm SHA256).Hash.ToLower())  $f"
}
[IO.File]::WriteAllText("$out\SHA256SUMS.txt", ($sums -join "`r`n") + "`r`n")

$zip = "$dist\ScreenFleet-$version.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $out -DestinationPath $zip
Write-Host "Release ready: $out"
Write-Host "          zip: $zip"
