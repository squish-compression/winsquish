<#
    Authenticode code-signing helper for WinSquish.

    Signs one or more files with signtool.exe, reading the certificate from
    environment variables so no secret ever lives in the repo. Reused for both
    the application binaries (from build-installer.bat) and the installer +
    embedded uninstaller (from Inno Setup's SignTool directive).

    Configure exactly ONE certificate source:
      SIGN_PFX          path to a .pfx / .p12 file
      SIGN_PFX_PASSWORD its password (optional if the file has none)
        -- or --
      SIGN_THUMBPRINT   SHA1 thumbprint of a cert already in a Windows store
        -- or --
      SIGN_SUBJECT      a substring of the subject name of a store cert

    Optional:
      SIGN_TIMESTAMP_URL  RFC-3161 timestamp server
                          (default: http://timestamp.digicert.com)
      SIGNTOOL            explicit path to signtool.exe (else auto-located)

    Usage:  powershell -NoProfile -ExecutionPolicy Bypass -File sign.ps1 <file> [<file> ...]
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)]
    [string[]] $Files
)

$ErrorActionPreference = 'Stop'

function Fail($msg) { Write-Error "sign: $msg"; exit 1 }

# --- locate signtool.exe --------------------------------------------------
$signtool = $env:SIGNTOOL
if (-not $signtool) {
    $cmd = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($cmd) { $signtool = $cmd.Source }
}
if (-not $signtool) {
    # Newest signtool.exe under any installed Windows 10/11 SDK.
    $roots = @("${env:ProgramFiles(x86)}\Windows Kits\10\bin",
               "${env:ProgramFiles}\Windows Kits\10\bin") | Where-Object { $_ -and (Test-Path $_) }
    $signtool = Get-ChildItem -Path $roots -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\' } |
        Sort-Object { try { [version]$_.Directory.Parent.Name } catch { [version]'0.0' } } -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $signtool -or -not (Test-Path $signtool)) {
    Fail "signtool.exe not found. Install the Windows SDK, or set the SIGNTOOL environment variable to its full path."
}

# --- certificate source ---------------------------------------------------
$certArgs = @()
if ($env:SIGN_PFX) {
    if (-not (Test-Path $env:SIGN_PFX)) { Fail "SIGN_PFX file not found: $($env:SIGN_PFX)" }
    $certArgs += @('/f', $env:SIGN_PFX)
    if ($env:SIGN_PFX_PASSWORD) { $certArgs += @('/p', $env:SIGN_PFX_PASSWORD) }
}
elseif ($env:SIGN_THUMBPRINT) {
    $certArgs += @('/sha1', ($env:SIGN_THUMBPRINT -replace '[\s:]', ''))
}
elseif ($env:SIGN_SUBJECT) {
    $certArgs += @('/n', $env:SIGN_SUBJECT)
}
else {
    Fail "no certificate configured. Set SIGN_PFX (+SIGN_PFX_PASSWORD), SIGN_THUMBPRINT, or SIGN_SUBJECT."
}

$timestamp = if ($env:SIGN_TIMESTAMP_URL) { $env:SIGN_TIMESTAMP_URL } else { 'http://timestamp.digicert.com' }

# --- sign -----------------------------------------------------------------
$commonArgs = @('sign', '/fd', 'sha256', '/td', 'sha256', '/tr', $timestamp) + $certArgs
foreach ($file in $Files) {
    if (-not (Test-Path $file)) { Fail "file to sign not found: $file" }
    Write-Host "Signing $file"
    & $signtool @commonArgs $file
    if ($LASTEXITCODE -ne 0) { Fail "signtool failed for '$file' (exit $LASTEXITCODE)." }
}
