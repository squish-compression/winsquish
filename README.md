# WinSquish

A small WinRAR-style Windows GUI for the [SQUISH](https://github.com/paigejulianne/squish)
context-mixing compressor, with Explorer right-click integration.

![SQ](src/winsquish.ico)

## Features

- **GUI**: pick or drag-and-drop a file, then **Compress** (to `.sq`) or
  **Extract**, with a live progress bar. Shows the original size stored in a
  `.sq` stream's header before you extract.
- **Self-extracting archives (SFX)**: tick **Create self-extracting archive**
  and Compress produces a Windows `.exe` instead of a `.sq` — WinSquish itself
  is the stub, so double-clicking that `.exe` extracts the payload beside it,
  no WinSquish install required on the other machine. **Extract** also opens
  an SFX built on *any* platform (a Linux ELF or macOS Mach-O stub included):
  extraction only reads the archive's trailer and payload, never the stub, so
  a foreign SFX opens here even though it won't *run* here.
- **Explorer context menu** (per-user, no admin required):
  - right-click any file → **Compress to .sq (WinSquish)**
  - right-click any file → **Compress to self-extracting .exe (WinSquish)**
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

This locates Visual Studio via `vswhere`, compiles `src\winsquish.cpp`
together with the vendored `squish\squish.c`, and produces
`build\winsquish.exe`. No other dependencies.

## Installer

```bat
installer\build-installer.bat
```

Produces `build\winsquish-setup.exe`, a per-user installer (requires
[Inno Setup](https://jrsoftware.org/isdl.php) 6.3+; it builds
`winsquish.exe` first if needed). The installer needs **no administrator
rights** — it installs into `%LOCALAPPDATA%\Programs\WinSquish`, registers
the `.sq` file type and Explorer context-menu entries under `HKCU`, adds a
Start Menu shortcut, and creates an Add/Remove Programs entry. Registration
is delegated to `winsquish.exe --register`, so the installer and the GUI's
**Tools ▸ Register** command write exactly the same keys. Uninstalling runs
`--unregister` (which only drops the `.sq` mapping if it still points at
WinSquish) before removing the files.

## Command line

```
winsquish.exe [file]                open GUI with the file preloaded
winsquish.exe --compress <file>     open GUI and start compressing to .sq
winsquish.exe --compress-sfx <file> open GUI and build a self-extracting .exe
winsquish.exe --decompress <file>   open GUI and extract a .sq or an SFX
winsquish.exe --register            install the context-menu entries (HKCU)
winsquish.exe --unregister          remove them
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

## Layout

```
src\winsquish.cpp    the application (pure Win32, no MFC/ATL)
src\winsquish.rc     icon, version info, manifest
squish\              vendored libsquish (squish.c / squish.h), unmodified
build.bat            one-step MSVC build
installer\           Inno Setup script + one-step installer build
```

## License

GPL-3.0 (the vendored libsquish is GPL-3.0; WinSquish follows suit).
Copyright (C) 2026 Paige Julianne Sullivan.
