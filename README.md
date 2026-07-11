# WinSquish

[![CI](https://github.com/paigejulianne/winsquish/actions/workflows/ci.yml/badge.svg)](https://github.com/paigejulianne/winsquish/actions/workflows/ci.yml)

A small WinRAR-style Windows GUI for the [SQUISH](https://github.com/paigejulianne/squish)
context-mixing compressor, with Explorer right-click integration.

![SQ](src/winsquish.ico)

## Features

- **GUI**: pick or drag-and-drop a **file or a folder**, then **Compress**
  (to `.sq`) or **Extract**, with a live progress bar. Shows the original size
  stored in a `.sq` stream's header before you extract.
- **Folders / seekable archives**: drop a folder (or use **File ▸ Open
  folder…**) and Compress packs the whole tree into a single `.sq` in SQUISH's
  seekable **`SQAR02`** archive format — each file is compressed as its own
  stream behind a compact index, so a reader can list members and pull out one
  file or subtree by seeking straight to it, without inflating the rest. Folder
  archives interoperate byte-for-byte with the `squish` CLI (`c`/`l`/`x`/`d`)
  in both directions. Extracting such a `.sq` recreates the directory tree
  (empty directories and all); extracting a single-file `.sq` writes the file —
  WinSquish detects which it is automatically.
- **Browse an archive (WinRAR-style)**: click **View Files…** (or run
  `winsquish.exe --view <archive>`) to open a `.sq` or self-extracting `.exe`
  in a browsable window. A seekable archive opens **instantly** — only its
  header and index are read. Navigate its folders (double-click to descend,
  **Up** to ascend), sort by name or size, and **Extract Selected…** files or
  folders — or **Extract All…** — to a folder you choose, with the archive's
  directory structure preserved. Each file is inflated on demand by seeking
  straight to its own stream, so extracting a few files out of a huge archive
  never decompresses the whole thing. When a target already exists you're asked
  whether to overwrite (with an "apply to all" option).
- **Self-extracting archives (SFX)**: tick **Create self-extracting archive**
  and Compress produces a Windows `.exe` instead of a `.sq` — WinSquish itself
  is the stub, so double-clicking that `.exe` extracts the payload beside it,
  no WinSquish install required on the other machine. **Extract** also opens
  an SFX built on *any* platform (a Linux ELF or macOS Mach-O stub included):
  extraction only reads the archive's trailer and payload, never the stub, so
  a foreign SFX opens here even though it won't *run* here.
- **Explorer context menu** (per-user, no admin required):
  - right-click any file **or folder** → **Compress to .sq (WinSquish)**
  - right-click any file **or folder** → **Compress to self-extracting .exe
    (WinSquish)**
  - right-click a `.sq` file → **View files with WinSquish** (opens the browser)
  - right-click a `.sq` file → **Extract with WinSquish**
  - double-clicking a `.sq` file opens it in the GUI
- **CPU cores**: pick how many workers to use (default **4**, capped at the
  machine's core count — it does *not* grab every core by default). Two or more
  cores use SQUISH's multi-block format for near-linear speedup; **1** uses the
  ratio-optimal single-block format for the smallest output. The same setting
  governs extraction.
- Every extraction is checksum-verified by libsquish; failed operations
  never leave partial output files behind.

SQUISH is a prediction-based compressor (ten statistical models, a logistic
mixer, and an arithmetic coder). It gets excellent ratios but runs at roughly
0.5–0.7 MB/s **per core** — large files take a while; that's what the
progress bar is for.

## Building

Requires Visual Studio with the C++ workload (any recent version).

```bat
build.bat
```

This locates Visual Studio via `vswhere`, compiles `src\winsquish.cpp`, and
links it against the prebuilt **libsquish DLL** (`squish\squish.lib` is the
import library) to produce `build\winsquish.exe`. It also copies
`squish\squish.dll` into `build\` so the exe runs straight from there.
WinSquish no longer vendors the compressor's `.c` source — it links the DLL and
ships it. To refresh the DLL, rebuild it in the [SQUISH] repo (`make dll` or
`build-windows.bat`) and copy `squish.dll`, `squish.lib`, and `squish.h` into
`squish\`.

## Installer

```bat
installer\build-installer.bat
```

Produces `build\winsquish-setup.exe` (requires
[Inno Setup](https://jrsoftware.org/isdl.php) 6.3+; it builds
`winsquish.exe` first if needed). It installs both `winsquish.exe` and its
`squish.dll` dependency. At startup it asks how to install:

- **For all users** (default, system-wide): installs into
  `%ProgramFiles%\WinSquish` and registers the `.sq` file type + context-menu
  entries under `HKLM` for every user. This one choice triggers a single UAC
  elevation prompt.
- **For me only** (per-user): installs into `%LOCALAPPDATA%\Programs\WinSquish`
  and registers under `HKCU`. No administrator rights required.

Either way it adds a Start Menu shortcut and an Add/Remove Programs entry.
Registration is delegated to `winsquish.exe --register` (with `--allusers` for
the system-wide scope), so the installer and the GUI's **Tools ▸ Register**
command write exactly the same keys, into the appropriate root. Uninstalling
runs `--unregister` (which only drops the `.sq` mapping if it still points at
WinSquish) before removing the files.

## Command line

```
winsquish.exe [path]                open GUI with the file/folder preloaded
winsquish.exe --compress <path>     open GUI and start compressing to .sq
                                    (<path> may be a file or a folder)
winsquish.exe --compress-sfx <path> open GUI and build a self-extracting .exe
winsquish.exe --decompress <file>   open GUI and extract a .sq or an SFX
winsquish.exe --view <file>         open the archive browser on a .sq or SFX
winsquish.exe --register            install the context-menu entries (HKCU)
winsquish.exe --unregister          remove them
winsquish.exe --register --allusers register system-wide in HKLM (needs admin;
                                     --allusers also applies to --unregister)
winsquish.exe --register --quiet    register with no confirmation dialog
                                    (--quiet also applies to --unregister;
                                     used by the installer/uninstaller)
```

A self-extracting `.exe` produced here carries an `"SQSFX01"` trailer that is
byte-compatible with the [SQUISH] CLI's `s` command, so archives interoperate
in both directions. Running an SFX (`winsquish` built as a stub with a payload
appended) with no arguments extracts it; `--decompress` handles both `.sq`
streams and SFX archives, deciding by inspecting the file.

[SQUISH]: https://github.com/paigejulianne/squish

Registration can also be done from the GUI (**Tools** menu). It writes only
to `HKCU\Software\Classes`, so it never needs elevation and affects only the
current user. The registered commands point at the exe's location at the
time of registration — re-register after moving the exe.

## Continuous integration

`.github/workflows/ci.yml` runs on every push and pull request (Windows,
MSVC x64): it builds `winsquish.exe`, runs the round-trip tests, and builds the
Inno Setup installer, uploading both as artifacts. The tests live in
`test\roundtrip.ps1` and drive the GUI headlessly (launch → poll for the
output → terminate), verifying folder, single-file, and self-extracting
round-trips by SHA-256. Run them locally against a build with:

```powershell
powershell -ExecutionPolicy Bypass -File test\roundtrip.ps1 -Exe build\winsquish.exe
```

## Layout

```
src\winsquish.cpp    the application (pure Win32, no MFC/ATL)
src\winsquish.rc     icon, version info, manifest
squish\              libsquish SDK: squish.h + squish.lib (import lib) +
                     squish.dll (linked at run time, shipped in the installer)
build.bat            one-step MSVC build (links the DLL, copies it to build\)
installer\           Inno Setup script + one-step installer build
test\roundtrip.ps1   headless end-to-end round-trip tests
.github\workflows\   CI: build + test + installer on every push/PR
```

## License

GPL-3.0 (the vendored libsquish is GPL-3.0; WinSquish follows suit).
Copyright (C) 2026 Paige Julianne Sullivan.
