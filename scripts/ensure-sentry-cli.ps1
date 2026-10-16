$ErrorActionPreference = 'Stop'

$version = '3.3.0'
$asset = 'sentry-cli-Windows-x86_64.exe'
$expectedSha256 = '25b938d377ef946ccec89d9b03c4f49ef6970c717404839d44c683313ec71c88'
$tempRoot = if ($env:RUNNER_TEMP) {
  $env:RUNNER_TEMP
} else {
  [IO.Path]::GetTempPath()
}
$installDirectory = Join-Path $tempRoot 'sentry-cli-bin'
$downloadPath = Join-Path $tempRoot "$asset-$version"
$sentryCli = Join-Path $installDirectory 'sentry-cli.exe'

New-Item -ItemType Directory -Path $installDirectory -Force | Out-Null
Write-Host "Installing sentry-cli $version into $installDirectory"
Invoke-WebRequest `
  -Uri "https://github.com/getsentry/sentry-cli/releases/download/$version/$asset" `
  -OutFile $downloadPath
$actualSha256 = (Get-FileHash -LiteralPath $downloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualSha256 -ne $expectedSha256) {
  throw "sentry-cli checksum mismatch: expected $expectedSha256, got $actualSha256"
}
Copy-Item -LiteralPath $downloadPath -Destination $sentryCli -Force

& $sentryCli --version | Write-Host
if ($LASTEXITCODE -ne 0) {
  throw 'installed sentry-cli failed its version check'
}
Write-Output $sentryCli
