param(
  [string]$OutDir = $(if ($env:CMUX_CHROME_OUT_DIR) { $env:CMUX_CHROME_OUT_DIR } else { "C:\cr\src\out\Release" }),
  [string]$ChromeProdVersion = $(if ($env:CHROME_PRODVERSION) { $env:CHROME_PRODVERSION } else { "138.0.0.0" }),
  # Keep this pin textually identical to scripts/fetch-extensions.sh.
  [string]$UBlockOriginVersion = $(if ($env:UBLOCK_ORIGIN_VERSION) { $env:UBLOCK_ORIGIN_VERSION } else { "1.72.2" })
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$UBlockId = "cjpalhdlnbpafiamejdnhcphjbkeiagm"
$UBlockExpectedName = "uBlock Origin"
$UBlockExpectedCrxSha256 = "6e02d8e6dce569eec721b531f9c7d28403e5426442417d5a855a17dd288b56f8"
$Target = Join-Path (Join-Path (Join-Path $OutDir "resources") "cmux-extensions") "ublock"
$CrxUrl = "https://clients2.google.com/service/update2/crx?response=redirect&prodversion=${ChromeProdVersion}&acceptformat=crx3&x=id%3D${UBlockId}%26installsource%3Dondemand%26uc"
$Utf8NoBom = [System.Text.UTF8Encoding]::new($false)

function Get-JsonProperty {
  param(
    [Parameter(Mandatory=$true)] $Object,
    [Parameter(Mandatory=$true)] [string]$Name
  )

  $property = $Object.PSObject.Properties[$Name]
  if ($null -eq $property) {
    return $null
  }
  return $property.Value
}

function ConvertTo-ExtensionId {
  param([Parameter(Mandatory=$true)] [byte[]]$PublicKey)

  $sha256 = [System.Security.Cryptography.SHA256]::Create()
  try {
    $digest = $sha256.ComputeHash($PublicKey)
  } finally {
    $sha256.Dispose()
  }

  $builder = [System.Text.StringBuilder]::new()
  for ($i = 0; $i -lt 16; ++$i) {
    [void]$builder.Append([char](97 + ($digest[$i] -shr 4)))
    [void]$builder.Append([char](97 + ($digest[$i] -band 0x0f)))
  }
  return $builder.ToString()
}

function Read-ProtoVarint {
  param(
    [Parameter(Mandatory=$true)] [byte[]]$Buffer,
    [Parameter(Mandatory=$true)] [ref]$Position
  )

  [uint64]$value = 0
  $shift = 0
  while ($true) {
    if ($Position.Value -ge $Buffer.Length) {
      throw "truncated protobuf varint"
    }
    $byte = [byte]$Buffer[$Position.Value]
    $Position.Value = $Position.Value + 1
    $value = $value -bor (([uint64]($byte -band 0x7f)) -shl $shift)
    if ($byte -lt 0x80) {
      return $value
    }
    $shift += 7
    if ($shift -gt 63) {
      throw "protobuf varint is too large"
    }
  }
}

function Read-ProtoFields {
  param([Parameter(Mandatory=$true)] [byte[]]$Buffer)

  $position = 0
  while ($position -lt $Buffer.Length) {
    $key = Read-ProtoVarint -Buffer $Buffer -Position ([ref]$position)
    $field = [int]($key -shr 3)
    $wireType = [int]($key -band 7)

    switch ($wireType) {
      0 {
        [void](Read-ProtoVarint -Buffer $Buffer -Position ([ref]$position))
      }
      1 {
        if ($position + 8 -gt $Buffer.Length) {
          throw "truncated fixed64 protobuf field"
        }
        $position += 8
      }
      2 {
        $size = [int](Read-ProtoVarint -Buffer $Buffer -Position ([ref]$position))
        if ($size -lt 0 -or $position + $size -gt $Buffer.Length) {
          throw "truncated length-delimited protobuf field"
        }
        $value = New-Object byte[] $size
        [Array]::Copy($Buffer, $position, $value, 0, $size)
        $position += $size
        [pscustomobject]@{
          Field = $field
          Value = $value
        }
      }
      5 {
        if ($position + 4 -gt $Buffer.Length) {
          throw "truncated fixed32 protobuf field"
        }
        $position += 4
      }
      default {
        throw "unsupported protobuf wire type $wireType"
      }
    }
  }
}

function Get-Crx3PublicKeyBase64ForId {
  param(
    [Parameter(Mandatory=$true)] [byte[]]$Header,
    [Parameter(Mandatory=$true)] [string]$ExpectedId
  )

  foreach ($field in Read-ProtoFields -Buffer $Header) {
    if ($field.Field -ne 2 -and $field.Field -ne 3) {
      continue
    }
    foreach ($proofField in Read-ProtoFields -Buffer ([byte[]]$field.Value)) {
      if ($proofField.Field -ne 1) {
        continue
      }
      [byte[]]$publicKey = $proofField.Value
      if ((ConvertTo-ExtensionId -PublicKey $publicKey) -eq $ExpectedId) {
        return [Convert]::ToBase64String($publicKey)
      }
    }
  }

  throw "CRX3 header did not contain key for $ExpectedId"
}

function Find-ZipStart {
  param(
    [Parameter(Mandatory=$true)] [byte[]]$Data,
    [Parameter(Mandatory=$true)] [int]$SearchStart
  )

  for ($i = $SearchStart; $i -le ($Data.Length - 4); ++$i) {
    if ($Data[$i] -eq 0x50 -and $Data[$i + 1] -eq 0x4b -and
        $Data[$i + 2] -eq 0x03 -and $Data[$i + 3] -eq 0x04) {
      return $i
    }
  }

  throw "CRX3 payload did not contain a ZIP local file header"
}

function Remove-ExtensionMetadata {
  param([Parameter(Mandatory=$true)] [string]$ExtensionDir)

  $metadata = Join-Path $ExtensionDir "_metadata"
  if (Test-Path -LiteralPath $metadata) {
    Remove-Item -LiteralPath $metadata -Recurse -Force
  }
}

function Assert-UBlockManifest {
  param(
    [Parameter(Mandatory=$true)] [string]$ManifestPath,
    [Parameter(Mandatory=$true)] [string]$ExpectedId,
    [Parameter(Mandatory=$true)] [string]$ExpectedName,
    [Parameter(Mandatory=$true)] [string]$ExpectedVersion
  )

  $manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
  $name = [string](Get-JsonProperty -Object $manifest -Name "name")
  $version = [string](Get-JsonProperty -Object $manifest -Name "version")
  $manifestVersion = Get-JsonProperty -Object $manifest -Name "manifest_version"
  $key = [string](Get-JsonProperty -Object $manifest -Name "key")
  $actualId = ""
  if ($key) {
    $actualId = ConvertTo-ExtensionId -PublicKey ([Convert]::FromBase64String($key))
  }

  $summary = "$name $version manifest_version=$manifestVersion id=$actualId"
  if ($name -ne $ExpectedName -or $version -ne $ExpectedVersion -or
      [int]$manifestVersion -ne 2 -or $actualId -ne $ExpectedId) {
    throw ("manifest validation failed: expected {0} {1} manifest_version=2 id={2}; got {3}" -f
           $ExpectedName, $ExpectedVersion, $ExpectedId, $summary)
  }

  return $summary
}

function Inject-UBlockKey {
  param(
    [Parameter(Mandatory=$true)] [string]$ManifestPath,
    [Parameter(Mandatory=$true)] [string]$PublicKeyBase64,
    [Parameter(Mandatory=$true)] [string]$ExpectedId,
    [Parameter(Mandatory=$true)] [string]$ExpectedName,
    [Parameter(Mandatory=$true)] [string]$ExpectedVersion
  )

  $manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
  $keyProperty = $manifest.PSObject.Properties["key"]
  if ($null -eq $keyProperty) {
    $manifest | Add-Member -NotePropertyName "key" -NotePropertyValue $PublicKeyBase64
  } else {
    $manifest.key = $PublicKeyBase64
  }

  $json = $manifest | ConvertTo-Json -Depth 100
  [System.IO.File]::WriteAllText($ManifestPath, $json + "`n", $Utf8NoBom)

  [void](Assert-UBlockManifest -ManifestPath $ManifestPath `
      -ExpectedId $ExpectedId -ExpectedName $ExpectedName `
      -ExpectedVersion $ExpectedVersion)
}

$existingManifest = Join-Path $Target "manifest.json"
if (Test-Path -LiteralPath $existingManifest) {
  Remove-ExtensionMetadata -ExtensionDir $Target
  try {
    $digestReceipt = Join-Path $Target ".cmux-crx-sha256"
    if (-not (Test-Path -LiteralPath $digestReceipt -PathType Leaf) -or
        (Get-Content -LiteralPath $digestReceipt -Raw).Trim() -ne
          $UBlockExpectedCrxSha256) {
      throw "cached uBlock Origin CRX digest receipt is missing or stale"
    }
    Write-Output (Assert-UBlockManifest -ManifestPath $existingManifest `
        -ExpectedId $UBlockId -ExpectedName $UBlockExpectedName `
        -ExpectedVersion $UBlockOriginVersion)
    exit 0
  } catch {
    Write-Warning $_.Exception.Message
  }
}

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("cmux-ublock." + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
  $crxPath = Join-Path $tempDir "ublock.crx"
  $zipPath = Join-Path $tempDir "ublock.zip"
  $unpackedDir = Join-Path $tempDir "unpacked"

  New-Item -ItemType Directory -Path (Split-Path -Path $Target -Parent) -Force | Out-Null
  Write-Output "fetching uBlock Origin $UBlockOriginVersion from Chrome Web Store"
  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
  Invoke-WebRequest -Uri $CrxUrl -OutFile $crxPath -UserAgent "Mozilla/5.0" -MaximumRedirection 5 -UseBasicParsing
  $actualCrxSha256 = (Get-FileHash -LiteralPath $crxPath -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($actualCrxSha256 -ne $UBlockExpectedCrxSha256) {
    throw "uBlock Origin CRX digest mismatch: $actualCrxSha256"
  }

  [byte[]]$crx = [System.IO.File]::ReadAllBytes($crxPath)
  if ($crx.Length -lt 16 -or [System.Text.Encoding]::ASCII.GetString($crx, 0, 4) -ne "Cr24") {
    throw "download was not a CRX file"
  }
  $crxVersion = [BitConverter]::ToUInt32($crx, 4)
  if ($crxVersion -ne 3) {
    throw "expected CRX3, got CRX$crxVersion"
  }
  $headerSize = [int][BitConverter]::ToUInt32($crx, 8)
  if ($crx.Length -lt 12 + $headerSize) {
    throw "truncated CRX3 header"
  }

  $header = New-Object byte[] $headerSize
  [Array]::Copy($crx, 12, $header, 0, $headerSize)
  $publicKeyBase64 = Get-Crx3PublicKeyBase64ForId -Header $header -ExpectedId $UBlockId

  $zipStart = Find-ZipStart -Data $crx -SearchStart (12 + $headerSize)
  $zipBytes = New-Object byte[] ($crx.Length - $zipStart)
  [Array]::Copy($crx, $zipStart, $zipBytes, 0, $zipBytes.Length)
  [System.IO.File]::WriteAllBytes($zipPath, $zipBytes)

  New-Item -ItemType Directory -Path $unpackedDir | Out-Null
  Expand-Archive -Path $zipPath -DestinationPath $unpackedDir -Force
  Remove-ExtensionMetadata -ExtensionDir $unpackedDir

  Inject-UBlockKey -ManifestPath (Join-Path $unpackedDir "manifest.json") `
      -PublicKeyBase64 $publicKeyBase64 -ExpectedId $UBlockId `
      -ExpectedName $UBlockExpectedName -ExpectedVersion $UBlockOriginVersion

  $tempTarget = "$Target.new"
  Remove-Item -LiteralPath $tempTarget -Recurse -Force -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Path $tempTarget | Out-Null
  Get-ChildItem -LiteralPath $unpackedDir -Force |
      Copy-Item -Destination $tempTarget -Recurse -Force
  [System.IO.File]::WriteAllText(
      (Join-Path $tempTarget ".cmux-crx-sha256"),
      $actualCrxSha256 + "`n",
      $Utf8NoBom)
  Remove-Item -LiteralPath $Target -Recurse -Force -ErrorAction SilentlyContinue
  Move-Item -LiteralPath $tempTarget -Destination $Target

  Write-Output (Assert-UBlockManifest -ManifestPath (Join-Path $Target "manifest.json") `
      -ExpectedId $UBlockId -ExpectedName $UBlockExpectedName `
      -ExpectedVersion $UBlockOriginVersion)
} finally {
  Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
}
