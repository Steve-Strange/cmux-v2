param(
  [int]$Iterations = 100,
  [string]$RepositoryRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
if ($Iterations -lt 1) {
  throw 'Iterations must be positive'
}

$sourcePath = Join-Path $RepositoryRoot 'overlay\chrome\browser\cmux_term\cmux_update_script.cc'
$source = Get-Content -LiteralPath $sourcePath -Raw
$match = [regex]::Match(
  $source,
  'return R"POWERSHELL\((?<script>.*?)\)POWERSHELL";',
  [System.Text.RegularExpressions.RegexOptions]::Singleline
)
if (-not $match.Success) {
  throw "Could not extract the embedded Windows updater from $sourcePath"
}
$installer = $match.Groups['script'].Value

for ($iteration = 1; $iteration -le $Iterations; $iteration++) {
  $root = Join-Path $env:TEMP ("cmux-update-stress-" + [guid]::NewGuid().ToString('N'))
  $current = Join-Path $root 'current'
  $staged = Join-Path $root 'staged'
  $marker = Join-Path $root 'relaunched.txt'
  $apply = Join-Path $root 'apply.ps1'
  New-Item -ItemType Directory -Path $current, $staged | Out-Null
  Set-Content -LiteralPath (Join-Path $current 'old.txt') -Value 'old'
  Set-Content -LiteralPath (Join-Path $staged 'new.txt') -Value 'new'
  Set-Content -LiteralPath (Join-Path $staged 'cmux-browser.cmd') -Value @(
    '@echo off',
    "echo relaunched>`"$marker`""
  )
  Set-Content -LiteralPath $apply -Value $installer

  & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $apply `
    -ParentPid 2147483647 -Current $current -Staged $staged `
    -Relaunch 'cmux-browser.cmd'

  # Start-Process is asynchronous; leave headroom for loaded hosted runners.
  for ($attempt = 0; $attempt -lt 200 -and -not (Test-Path -LiteralPath $marker); $attempt++) {
    Start-Sleep -Milliseconds 50
  }
  if (-not (Test-Path -LiteralPath (Join-Path $current 'new.txt'))) {
    throw "Iteration $iteration did not install the staged tree"
  }
  if (Test-Path -LiteralPath (Join-Path $current 'old.txt')) {
    throw "Iteration $iteration retained the old tree"
  }
  if (-not (Test-Path -LiteralPath $marker)) {
    throw "Iteration $iteration did not relaunch"
  }
  if (Get-ChildItem -LiteralPath $root -Filter 'current.cmux-old.*') {
    throw "Iteration $iteration retained a rollback directory"
  }
  Remove-Item -LiteralPath $root -Recurse -Force

  if (($iteration % 10) -eq 0 -or $iteration -eq $Iterations) {
    Write-Output "Windows updater stress: $iteration/$Iterations passed"
  }
}

Write-Output "Windows updater stress passed: $Iterations iterations"
