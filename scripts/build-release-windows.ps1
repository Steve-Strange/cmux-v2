param(
  [string]$ChromiumSrc = $(if ($env:CHROMIUM_SRC) { $env:CHROMIUM_SRC } else { 'C:\cr\src' }),
  [string]$Output = $(if ($env:CMUX_RELEASE_OUTPUT) { $env:CMUX_RELEASE_OUTPUT } else { '' })
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = $env:CMUX_RELEASE_VERSION
$channel = $env:CMUX_RELEASE_CHANNEL
if (-not $version) { throw 'CMUX_RELEASE_VERSION is required' }
if ($channel -notin @('stable', 'nightly')) { throw "invalid channel: $channel" }
if (-not $Output) { $Output = Join-Path $root 'release' }
if (-not (Test-Path -LiteralPath (Join-Path $ChromiumSrc '.git'))) {
  throw "warm Chromium checkout missing: $ChromiumSrc"
}

# Restore with the previous branch's mirrored definition before replacing
# .cmux-patches. The subsequent blanket restore remains the Windows release
# runner's defense against other tracked worktree residue.
$permissionRestore = Join-Path $root 'scripts\restore-custom-window-permissions.py'
& python $permissionRestore $ChromiumSrc
if ($LASTEXITCODE -ne 0) { throw 'previous permission patch restoration failed' }

& git -C $ChromiumSrc restore --worktree -- .
if ($LASTEXITCODE -ne 0) { throw 'git restore failed' }

$owned = Join-Path $ChromiumSrc 'chrome\browser\cmux_term'
if (Test-Path -LiteralPath $owned) { Remove-Item -LiteralPath $owned -Recurse -Force }
Copy-Item -LiteralPath (Join-Path $root 'overlay\chrome\browser\cmux_term') `
  -Destination $owned -Recurse
& robocopy (Join-Path $root 'overlay') $ChromiumSrc /E /NFL /NDL /NJH /NJS /NP
if ($LASTEXITCODE -gt 7) { throw "overlay robocopy failed: $LASTEXITCODE" }
$patches = Join-Path $ChromiumSrc '.cmux-patches'
if (Test-Path -LiteralPath $patches) { Remove-Item -LiteralPath $patches -Recurse -Force }
Copy-Item -LiteralPath (Join-Path $root 'patches') -Destination $patches -Recurse
$heliumThirdParty = Join-Path $ChromiumSrc 'third_party\helium'
if (Test-Path -LiteralPath $heliumThirdParty) {
  Remove-Item -LiteralPath $heliumThirdParty -Recurse -Force
}
Copy-Item -LiteralPath (Join-Path $root 'third_party\helium') `
  -Destination $heliumThirdParty -Recurse

$env:CHROMIUM_SRC = $ChromiumSrc
& python (Join-Path $root 'scripts\apply_win_chrome.py')
if ($LASTEXITCODE -ne 0) { throw 'Windows Chromium patching failed' }
& python (Join-Path $root 'scripts\stamp-release-build.py') `
  --chromium-src $ChromiumSrc
if ($LASTEXITCODE -ne 0) { throw 'release-build stamping failed' }
& python (Join-Path $root 'scripts\release_version.py') `
  --chromium-src $ChromiumSrc --version $version
if ($LASTEXITCODE -ne 0) { throw 'release version patching failed' }

function Assert-GnArg([string]$Name, [string]$Expected) {
  Push-Location $ChromiumSrc
  try {
    $actual = (& gn args out\Release "--list=$Name" --short | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { throw "gn args failed for $Name" }
  } finally {
    Pop-Location
  }
  if (-not $actual.Contains("$Name = $Expected")) {
    throw "out\Release requires $Name = $Expected; got: $actual"
  }
}
Assert-GnArg 'is_component_build' 'false'
Assert-GnArg 'is_debug' 'false'
Assert-GnArg 'symbol_level' '1'
Assert-GnArg 'target_cpu' '"x64"'

$certificate = $env:CMUX_WINDOWS_SIGNING_CERTIFICATE
$password = $env:CMUX_WINDOWS_SIGNING_PASSWORD
if (-not $certificate -or -not $password) {
  throw 'Windows signing certificate/password were not provided'
}

$releaseRoot = Join-Path $ChromiumSrc 'out\Release'
$noticeRoot = Join-Path $releaseRoot 'cmux-licenses'
New-Item -ItemType Directory `
  -Path (Join-Path $noticeRoot 'third_party\helium') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $noticeRoot -Force
Copy-Item -LiteralPath (Join-Path $root 'THIRD_PARTY_NOTICES.md') `
  -Destination $noticeRoot -Force
Copy-Item -LiteralPath (Join-Path $root 'third_party\helium\LICENSE') `
  -Destination (Join-Path $noticeRoot 'third_party\helium') -Force
Copy-Item -LiteralPath (Join-Path $root 'third_party\helium\README.chromium') `
  -Destination (Join-Path $noticeRoot 'third_party\helium') -Force

$feedFile = Join-Path $releaseRoot 'cmux-update-feed-url'
if ($channel -eq 'nightly') {
  Set-Content -LiteralPath $feedFile -Encoding ascii `
    -Value 'https://github.com/manaflow-ai/cmux-v2/releases/download/nightly/update.json'
} else {
  Remove-Item -LiteralPath $feedFile -Force -ErrorAction SilentlyContinue
}

# Include the channel selector in Chromium's own installer archive. The next
# git restore resets this tracked manifest before a subsequent runner job.
$releaseManifest = Join-Path $ChromiumSrc 'chrome\installer\mini_installer\chrome.release'
$releaseEntry = 'cmux-update-feed-url: %(ChromeDir)s\'
$manifestText = Get-Content -LiteralPath $releaseManifest -Raw
$releaseEntries = @(
  'cmux-licenses\LICENSE: %(ChromeDir)s\cmux-licenses\',
  'cmux-licenses\THIRD_PARTY_NOTICES.md: %(ChromeDir)s\cmux-licenses\',
  'cmux-licenses\third_party\helium\LICENSE: %(ChromeDir)s\cmux-licenses\third_party\helium\',
  'cmux-licenses\third_party\helium\README.chromium: %(ChromeDir)s\cmux-licenses\third_party\helium\'
)
if ($channel -eq 'nightly') {
  $releaseEntries += $releaseEntry
}
foreach ($entry in $releaseEntries) {
  if ($manifestText.Contains($entry)) { continue }
  $sectionAnchor = '[HIDPI]'
  if (-not $manifestText.Contains($sectionAnchor)) {
    throw 'chrome.release GENERAL section anchor was not found'
  }
  $manifestText = $manifestText.Replace(
    $sectionAnchor, "$entry`r`n$sectionAnchor")
}
Set-Content -LiteralPath $releaseManifest -Encoding ascii -NoNewline -Value $manifestText

Push-Location $ChromiumSrc
try {
  # Build the archive once so every runtime dependency listed by Chromium's
  # packaging graph exists before signing begins.
  & autoninja -C out\Release chrome mini_installer
  if ($LASTEXITCODE -ne 0) { throw 'Chromium Windows payload build failed' }
} finally {
  Pop-Location
}

# Sign the files consumed by chrome.release and setup.exe before Chromium's
# archive action runs. The installed browser is therefore signed, not merely
# the outer bootstrap executable.
Get-ChildItem -LiteralPath $releaseRoot -File |
  Where-Object { $_.Extension -in @('.exe', '.dll') } |
  ForEach-Object {
    & signtool sign /fd SHA256 /td SHA256 /tr http://timestamp.digicert.com `
      /f $certificate /p $password $_.FullName
    if ($LASTEXITCODE -ne 0) { throw "Authenticode signing failed: $($_.FullName)" }
  }

# Force the archive action to consume the now-signed payload. Merely changing
# its inputs is invisible to Ninja because signing happens outside the graph.
foreach ($generated in @('chrome.7z', 'chrome.packed.7z', 'setup.ex_', 'mini_installer.exe')) {
  Remove-Item -LiteralPath (Join-Path $releaseRoot $generated) `
    -Force -ErrorAction SilentlyContinue
}

Push-Location $ChromiumSrc
try {
  & autoninja -C out\Release mini_installer
  if ($LASTEXITCODE -ne 0) { throw 'signed mini_installer build failed' }
} finally {
  Pop-Location
}

$archive = Get-ChildItem -LiteralPath $releaseRoot -Filter 'chrome.7z' -File |
  Select-Object -First 1
if (-not $archive) { throw 'mini_installer did not produce chrome.7z' }
$work = Join-Path $env:TEMP ("cmux-windows-release-" + [guid]::NewGuid().ToString('N'))
$stage = Join-Path $Output 'cmux-browser'
try {
  New-Item -ItemType Directory -Path $work, $Output -Force | Out-Null
  & 7z x $archive.FullName "-o$work" -y | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'chrome.7z extraction failed' }
  $chrome = Get-ChildItem -LiteralPath $work -Filter 'chrome.exe' -File -Recurse |
    Sort-Object { $_.FullName.Length } | Select-Object -First 1
  if (-not $chrome) { throw 'chrome.exe was not found in chrome.7z' }

  if ($channel -eq 'nightly' -and
      -not (Test-Path -LiteralPath (Join-Path $chrome.Directory.FullName 'cmux-update-feed-url'))) {
    throw 'installed Windows payload is missing its update channel selector'
  }

  if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
  Copy-Item -LiteralPath $chrome.Directory.FullName -Destination $stage -Recurse

  & python (Join-Path $root 'scripts\build-update-archive.py') `
    --input $stage --output (Join-Path $Output 'cmux-windows-x64.zip')
  if ($LASTEXITCODE -ne 0) { throw 'Windows update archive failed' }

  $installer = Join-Path $ChromiumSrc 'out\Release\mini_installer.exe'
  & signtool sign /fd SHA256 /td SHA256 /tr http://timestamp.digicert.com `
    /f $certificate /p $password $installer
  if ($LASTEXITCODE -ne 0) { throw 'mini_installer signing failed' }
  Copy-Item -LiteralPath $installer `
    -Destination (Join-Path $Output 'cmux-windows-x64-installer.exe') -Force
  if (-not (Test-Path -LiteralPath (Join-Path $stage 'chrome.exe'))) {
    throw 'staged Windows root is missing chrome.exe'
  }
  if (-not (Test-Path -LiteralPath (
      Join-Path $stage 'cmux-licenses\THIRD_PARTY_NOTICES.md'))) {
    throw 'staged Windows root is missing third-party notices'
  }
  if (-not (Test-Path -LiteralPath (
      Join-Path $stage 'cmux-licenses\third_party\helium\LICENSE'))) {
    throw 'staged Windows root is missing the Helium license'
  }
  Get-ChildItem -LiteralPath $stage -Recurse -File |
    Where-Object { $_.Extension -in @('.exe', '.dll') } |
    ForEach-Object {
      & signtool verify /pa /all $_.FullName | Out-Null
      if ($LASTEXITCODE -ne 0) { throw "unsigned packaged binary: $($_.FullName)" }
    }
  & signtool verify /pa /all (Join-Path $Output 'cmux-windows-x64-installer.exe') |
    Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'packaged mini_installer signature verification failed' }
  Write-Output "Windows release artifact: $Output\cmux-windows-x64.zip"
} finally {
  if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}
