<#
.SYNOPSIS
    End-to-end round-trip tests for WinSquish (used by CI and runnable locally).

.DESCRIPTION
    WinSquish is a GUI app: launched with --compress / --decompress /
    --compress-sfx it preloads the target and starts the operation on a worker
    thread, then leaves its window open. This script drives it "headlessly" by
    launching it, polling the filesystem for the expected output, and then
    terminating the process. It never needs the window to be visible — the
    worker runs regardless — so it works on a hosted CI runner.

    It verifies three paths against real byte content (SHA-256 of every file):
      1. folder  -> .sq            -> extract -> tree matches
      2. file    -> .sq            -> extract -> file matches
      3. folder  -> self-extracting .exe -> extract -> tree matches

.PARAMETER Exe
    Path to winsquish.exe (squish.dll must sit beside it).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Exe
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Exe = (Resolve-Path -LiteralPath $Exe).Path
Write-Host "Using: $Exe"

# --- helpers ---------------------------------------------------------------

# Win32 shims used to inspect the archive-browser window ("View Files"). The
# browser is a plain Win32 window with a SysListView32 child; we find it by
# class and read its row count with LVM_GETITEMCOUNT — no clicking required.
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class WsWin {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int m);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, int msg, IntPtr w, IntPtr l);
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  public static IntPtr browser = IntPtr.Zero;
  public static IntPtr list = IntPtr.Zero;
  static string Cls(IntPtr h){ var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
  static bool FindBrowser(IntPtr h, IntPtr p){ if (Cls(h) == "WinSquishBrowserWindow") browser = h; return true; }
  static bool FindList(IntPtr h, IntPtr p){ if (Cls(h).StartsWith("SysListView32")) list = h; return true; }
  // Row count of the browser's list, or -1 if the browser window is absent.
  public static int ListCount() {
    browser = IntPtr.Zero;
    EnumWindows(FindBrowser, IntPtr.Zero);
    if (browser == IntPtr.Zero) return -1;
    list = IntPtr.Zero;
    EnumChildWindows(browser, FindList, IntPtr.Zero);
    if (list == IntPtr.Zero) return -1;
    return (int)SendMessage(list, 0x1004 /* LVM_GETITEMCOUNT */, IntPtr.Zero, IntPtr.Zero);
  }
}
'@

function Start-Ws {
    param([string[]] $WsArgs)
    return Start-Process -FilePath $Exe -ArgumentList $WsArgs -PassThru
}

function Stop-Ws {
    param($Proc)
    Start-Sleep -Milliseconds 750           # let the last write flush + close
    if (-not $Proc.HasExited) {
        Stop-Process -Id $Proc.Id -Force
        $null = $Proc.WaitForExit(5000)
    }
}

# Wait until a single output file exists and its size stops changing.
function Wait-ForStableFile {
    param([string] $Path, [int] $TimeoutSec = 180)
    $last = -1; $stable = 0
    for ($i = 0; $i -lt ($TimeoutSec * 2); $i++) {
        Start-Sleep -Milliseconds 500
        if (Test-Path -LiteralPath $Path) {
            $sz = (Get-Item -LiteralPath $Path).Length
            if ($sz -gt 0 -and $sz -eq $last) { $stable++ } else { $stable = 0 }
            if ($stable -ge 4) { return $true }
            $last = $sz
        }
    }
    return $false
}

# Wait until an extracted tree has at least $Count files and its total size
# stops changing (robust to the order files are written in).
function Wait-ForStableTree {
    param([string] $Root, [int] $Count, [int] $TimeoutSec = 180)
    $last = -1; $stable = 0
    for ($i = 0; $i -lt ($TimeoutSec * 2); $i++) {
        Start-Sleep -Milliseconds 500
        if (Test-Path -LiteralPath $Root) {
            $files = @(Get-ChildItem -LiteralPath $Root -Recurse -File -Force -ErrorAction SilentlyContinue)
            if ($files.Count -ge $Count) {
                $total = ($files | Measure-Object -Property Length -Sum).Sum
                if ($total -eq $last) { $stable++ } else { $stable = 0 }
                if ($stable -ge 4) { return $true }
                $last = $total
            }
        }
    }
    return $false
}

# Map of "F:<relpath>" -> hash and "D:<relpath>" -> "" for every entry.
function Get-TreeHashes {
    param([string] $Root)
    $Root = (Resolve-Path -LiteralPath $Root).Path
    $map = @{}
    foreach ($item in Get-ChildItem -LiteralPath $Root -Recurse -Force) {
        $rel = $item.FullName.Substring($Root.Length).TrimStart('\')
        if ($item.PSIsContainer) {
            $map["D:$rel"] = ''
        } else {
            $map["F:$rel"] = (Get-FileHash -LiteralPath $item.FullName -Algorithm SHA256).Hash
        }
    }
    return $map
}

function Assert-TreesEqual {
    param($Expected, $Actual, [string] $Label)
    $ke = @($Expected.Keys | Sort-Object)
    $ka = @($Actual.Keys   | Sort-Object)
    $diff = Compare-Object -ReferenceObject $ke -DifferenceObject $ka
    if ($diff) {
        throw "$Label FAILED: entry sets differ:`n$($diff | Format-Table -AutoSize | Out-String)"
    }
    foreach ($k in $ke) {
        if ($Expected[$k] -ne $Actual[$k]) {
            throw "$Label FAILED: content differs for '$k'"
        }
    }
    Write-Host "  [PASS] $Label ($($ke.Count) entries match)"
}

# --- build a representative source tree ------------------------------------

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("wsq_ci_" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
Write-Host "Work dir: $work"

try {
    $src = Join-Path $work 'src'
    New-Item -ItemType Directory -Path (Join-Path $src 'sub')      | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $src 'emptydir') | Out-Null
    Set-Content -LiteralPath (Join-Path $src 'readme.txt')    -Value "hello squish directory archive`nsecond line" -NoNewline
    Set-Content -LiteralPath (Join-Path $src 'sub\data.csv')  -Value "a,b,c`n1,2,3`n4,5,6" -NoNewline
    ((1..4000 | ForEach-Object { "row $_ some repetitive, compressible content" }) -join "`n") |
        Set-Content -LiteralPath (Join-Path $src 'big.txt') -NoNewline
    $srcFileCount = @(Get-ChildItem -LiteralPath $src -Recurse -File).Count   # 3
    $srcHashes = Get-TreeHashes -Root $src

    # === Test 1: folder -> .sq -> extract =================================
    Write-Host "`nTest 1: folder compress + extract"
    $sq = Join-Path $work 'src.sq'
    $p = Start-Ws @('--compress', $src)
    $ok = Wait-ForStableFile -Path $sq
    Stop-Ws $p
    if (-not $ok) { throw "Test 1 FAILED: '$sq' was not produced" }
    Write-Host "  compressed -> $sq ($((Get-Item $sq).Length) bytes)"

    # Extract from a clean directory so the target 'src' folder does not collide
    # with the original.
    $ext = Join-Path $work 'extract'
    New-Item -ItemType Directory -Path $ext | Out-Null
    $sqCopy = Join-Path $ext 'src.sq'
    Copy-Item -LiteralPath $sq -Destination $sqCopy
    $extRoot = Join-Path $ext 'src'
    $p = Start-Ws @('--decompress', $sqCopy)
    $ok = Wait-ForStableTree -Root $extRoot -Count $srcFileCount
    Stop-Ws $p
    if (-not $ok) { throw "Test 1 FAILED: extraction under '$extRoot' did not complete" }
    Assert-TreesEqual -Expected $srcHashes -Actual (Get-TreeHashes -Root $extRoot) -Label "Test 1 folder round-trip"

    # === Test 2: single file -> .sq -> extract ============================
    Write-Host "`nTest 2: single-file compress + extract"
    $file = Join-Path $work 'lonely.txt'
    ((1..1500 | ForEach-Object { "single file line $_" }) -join "`n") | Set-Content -LiteralPath $file -NoNewline
    $fileHash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
    $fsq = "$file.sq"
    $p = Start-Ws @('--compress', $file)
    $ok = Wait-ForStableFile -Path $fsq
    Stop-Ws $p
    if (-not $ok) { throw "Test 2 FAILED: '$fsq' was not produced" }
    # Extract to a clean dir; winsquish strips .sq -> 'lonely.txt'.
    $ext2 = Join-Path $work 'extract2'
    New-Item -ItemType Directory -Path $ext2 | Out-Null
    $fsqCopy = Join-Path $ext2 'lonely.txt.sq'
    Copy-Item -LiteralPath $fsq -Destination $fsqCopy
    $outFile = Join-Path $ext2 'lonely.txt'
    $p = Start-Ws @('--decompress', $fsqCopy)
    $ok = Wait-ForStableFile -Path $outFile
    Stop-Ws $p
    if (-not $ok) { throw "Test 2 FAILED: '$outFile' was not produced" }
    if ((Get-FileHash -LiteralPath $outFile -Algorithm SHA256).Hash -ne $fileHash) {
        throw "Test 2 FAILED: single-file content differs"
    }
    Write-Host "  [PASS] Test 2 single-file round-trip"

    # === Test 3: folder -> self-extracting .exe -> extract ================
    Write-Host "`nTest 3: folder self-extracting archive"
    $sfx = Join-Path $work 'src.exe'
    $p = Start-Ws @('--compress-sfx', $src)
    $ok = Wait-ForStableFile -Path $sfx
    Stop-Ws $p
    if (-not $ok) { throw "Test 3 FAILED: '$sfx' was not produced" }
    Write-Host "  built SFX -> $sfx ($((Get-Item $sfx).Length) bytes)"
    $ext3 = Join-Path $work 'extract3'
    New-Item -ItemType Directory -Path $ext3 | Out-Null
    Copy-Item -LiteralPath $sfx -Destination (Join-Path $ext3 'src.exe')
    $sfxRoot = Join-Path $ext3 'src'
    $p = Start-Ws @('--decompress', (Join-Path $ext3 'src.exe'))
    $ok = Wait-ForStableTree -Root $sfxRoot -Count $srcFileCount
    Stop-Ws $p
    if (-not $ok) { throw "Test 3 FAILED: SFX extraction under '$sfxRoot' did not complete" }
    Assert-TreesEqual -Expected $srcHashes -Actual (Get-TreeHashes -Root $sfxRoot) -Label "Test 3 folder SFX round-trip"

    # === Test 4: archive browser lists contents ("View Files") ============
    # Open src.sq (from Test 1) in the browser and confirm its SysListView32
    # shows the four root entries: sub\, emptydir\, readme.txt, big.txt.
    Write-Host "`nTest 4: archive browser ('View Files')"
    $p = Start-Ws @('--view', $sq)
    $count = -1
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 500
        $count = [WsWin]::ListCount()
        if ($count -ge 0) { break }
    }
    Stop-Ws $p
    if ($count -lt 0) { throw "Test 4 FAILED: browser window / list never appeared" }
    if ($count -ne 4) {
        throw "Test 4 FAILED: expected 4 root entries in the browser, got $count"
    }
    Write-Host "  [PASS] Test 4 browser lists 4 root entries"

    Write-Host "`nAll round-trip tests passed."
    exit 0
}
catch {
    Write-Host "`n$($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}
