/* Copyright (C) 2026  Paige Julianne Sullivan
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* ============================================================================
 * winsquish.cpp — Windows GUI front end for libsquish
 *
 * A small WinRAR-style utility: pick (or drop) a file, compress it to .sq or
 * extract a .sq stream, with live progress. "View Files…" opens an archive in
 * a browsable window — navigate its folders and extract just the files or
 * folders you select, WinRAR-style. Installs optional per-user Explorer
 * context-menu entries:
 *
 *   any file  ->  "Compress to .sq (WinSquish)"
 *   any file  ->  "Compress to self-extracting .exe (WinSquish)"
 *   .sq file  ->  "Extract with WinSquish" (+ double-click opens the GUI)
 *
 * Self-extracting archives (SFX)
 *   Ticking "Create self-extracting archive" produces a Windows .exe instead
 *   of a .sq: WinSquish itself is used as the stub, with the compressed
 *   payload, the original file name, and a 32-byte trailer appended. Running
 *   that .exe (e.g. double-clicking it) re-launches WinSquish, which detects
 *   the trailer in its own image and extracts the payload beside itself.
 *
 *   The trailer format ("SQSFX01") is shared with the squish CLI, so an SFX
 *   built on any platform (a Linux ELF or macOS Mach-O stub included) can be
 *   extracted here: extraction only reads the trailer + payload and never
 *   needs to run the foreign stub. Only self-extraction-by-running is
 *   platform-specific; opening a foreign SFX in WinSquish always works.
 *
 * Command line:
 *   winsquish.exe [file]                 open GUI, file preloaded
 *   winsquish.exe --compress <file>      open GUI and start compressing (.sq)
 *   winsquish.exe --compress-sfx <file>  open GUI and build a .exe SFX
 *   winsquish.exe --decompress <file>    open GUI and extract (.sq or SFX)
 *   winsquish.exe --view <file>          open the archive browser (.sq or SFX)
 *   winsquish.exe --register             install context-menu entries (HKCU)
 *   winsquish.exe --unregister           remove them
 *   winsquish.exe --register --quiet     as above, but no confirmation dialog
 *                                        (used by the installer/uninstaller)
 *
 * Registration is per-user (HKCU\Software\Classes): no admin rights needed.
 * ==========================================================================*/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <commdlg.h>
#include <string>
#include <string.h>
#include <vector>
#include <algorithm>
#include <set>

#include "../squish/squish.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

/* --- constants ------------------------------------------------------------ */
static const wchar_t *APP_NAME   = L"WinSquish";
static const wchar_t *WND_CLASS  = L"WinSquishMainWindow";
static const wchar_t *PROGID     = L"WinSquish.sq";
static const wchar_t *SQ_EXT     = L".sq";

#define WM_APP_PROGRESS (WM_APP + 1)   /* wParam = percent 0..100            */
#define WM_APP_DONE     (WM_APP + 2)   /* wParam = squish_status              */

/* control IDs */
#define IDC_FILE_EDIT    1001
#define IDC_BROWSE_BTN   1002
#define IDC_INFO_STATIC  1003
#define IDC_CPU_COMBO    1004
#define IDC_COMPRESS_BTN 1005
#define IDC_EXTRACT_BTN  1006
#define IDC_PROGRESS     1007
#define IDC_STATUS       1008
#define IDC_SFX_CHECK    1009
#define IDC_VIEW_BTN     1010

/* archive-browser window control IDs */
#define IDC_LV           1101
#define IDC_UP_BTN       1102
#define IDC_EXTRACT_SEL  1103
#define IDC_EXTRACT_ALL  1104
#define IDC_PATH_STATIC  1105

/* menu IDs */
#define IDM_OPEN         2001
#define IDM_EXIT         2002
#define IDM_REGISTER     2003
#define IDM_UNREGISTER   2004
#define IDM_ABOUT        2005
#define IDM_OPENFOLDER   2006

/* --- globals ---------------------------------------------------------------*/
static HWND      g_hwnd;
static HFONT     g_font;
static bool      g_busy = false;
static ULONGLONG g_t0;

struct Job {
    HWND         hwnd;
    std::wstring src, dst;
    bool         compress;   /* true = compress, false = extract              */
    bool         sfx;        /* compress: build .exe SFX; extract: src is SFX */
    bool         srcIsDir;   /* compress: source is a folder (SQAR archive)   */
    bool         tree;       /* extract: output is a directory tree (archive) */
    int          threads;    /* worker count; 1 = single-block (best ratio)   */
    volatile LONG lastPct;
};
static Job *g_job = nullptr;

/* Default worker count when nothing else is chosen. */
#define DEFAULT_THREADS 4

/* --- small helpers ---------------------------------------------------------*/
static std::string ToUtf8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring ExePath(void) {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

static bool HasSqExt(const std::wstring &path) {
    size_t n = path.size(), e = wcslen(SQ_EXT);
    return n > e && _wcsicmp(path.c_str() + n - e, SQ_EXT) == 0;
}

static long long FileSize(const std::wstring &path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa))
        return -1;
    return ((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}

static std::wstring PrettySize(long long n) {
    wchar_t buf[64];
    if (n < 0)                    return L"?";
    if (n < 1024)                 swprintf(buf, 64, L"%lld bytes", n);
    else if (n < 1024 * 1024)     swprintf(buf, 64, L"%.1f KB", n / 1024.0);
    else if (n < 1024LL * 1024 * 1024)
                                  swprintf(buf, 64, L"%.1f MB", n / 1048576.0);
    else                          swprintf(buf, 64, L"%.2f GB", n / 1073741824.0);
    return buf;
}

/* Read the stream header of a .sq file to learn the original size.
 * Returns true (and sets *origSize) only for a valid SQUISH header. */
static bool ReadSqHeader(const std::wstring &path, uint64_t *origSize) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    unsigned char hdr[16];
    DWORD got = 0;
    BOOL ok = ReadFile(h, hdr, sizeof hdr, &got, nullptr);
    CloseHandle(h);
    return ok && squish_decompressed_size(hdr, got, origSize) == SQUISH_OK;
}

/* --- self-extracting archive (SFX) container -------------------------------
 * Byte-compatible with the squish CLI's "SQSFX01" format:
 *
 *     [ stub executable ][ payload ][ name ][ 32-byte trailer ]
 *
 * payload is a plain SQUISH stream (exactly what squish_compress emits); name
 * is the raw UTF-8 basename of the original file (NOT null-terminated); the
 * trailer is little-endian:
 *
 *     off  size  field
 *       0     8  magic "SQSFX01\n"
 *       8     8  payload_off  (u64)  = stub length / offset of payload
 *      16     8  payload_len  (u64)
 *      24     4  name_len     (u32)
 *      28     4  flags        (u32)  = 0
 *
 * The trailer sits at end-of-file, so probing is platform-agnostic: a stub
 * built as a PE, ELF, or Mach-O binary is irrelevant to extraction. -------- */
static const unsigned char SFX_MAGIC[8] =
    { 'S','Q','S','F','X','0','1','\n' };
#define SFX_MAGIC_LEN   8u
#define SFX_TRAILER_LEN 32u
#define SFX_MAX_NAME    4096u

static void PutU32LE(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static void PutU64LE(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (unsigned char)(v >> (8 * i));
}
static uint32_t GetU32LE(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t GetU64LE(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

struct SfxInfo {
    uint64_t  payloadOff;
    uint64_t  payloadLen;
    uint32_t  nameLen;
    long long fileSize;
};

/* Open a file for shared reading — tolerant enough to read our own running
 * image (which the loader keeps open). */
static HANDLE OpenShared(const std::wstring &path) {
    return CreateFileW(path.c_str(), GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, 0, nullptr);
}

/* Validate and read the SFX trailer of `path`. Returns true for a well-formed
 * archive (any platform's stub). Mirrors the CLI's sfx_probe checks. */
static bool ProbeSfx(const std::wstring &path, SfxInfo *info) {
    HANDLE h = OpenShared(path);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li)) { CloseHandle(h); return false; }
    long long sz = li.QuadPart;
    if (sz < (long long)SFX_TRAILER_LEN) { CloseHandle(h); return false; }
    LARGE_INTEGER pos; pos.QuadPart = sz - (long long)SFX_TRAILER_LEN;
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) { CloseHandle(h); return false; }
    unsigned char t[SFX_TRAILER_LEN];
    DWORD got = 0;
    BOOL ok = ReadFile(h, t, SFX_TRAILER_LEN, &got, nullptr);
    CloseHandle(h);
    if (!ok || got != SFX_TRAILER_LEN) return false;
    if (memcmp(t, SFX_MAGIC, SFX_MAGIC_LEN) != 0) return false;
    uint64_t off  = GetU64LE(t + 8);
    uint64_t plen = GetU64LE(t + 16);
    uint32_t nlen = GetU32LE(t + 24);
    if (off == 0 || plen == 0 || nlen > SFX_MAX_NAME) return false;
    uint64_t tail = plen + (uint64_t)nlen + SFX_TRAILER_LEN;
    if (tail > (uint64_t)sz || off != (uint64_t)sz - tail) return false;
    info->payloadOff = off; info->payloadLen = plen;
    info->nameLen = nlen;   info->fileSize = sz;
    return true;
}

/* Read the stored (UTF-8) original name out of an SFX into a wide string. */
static bool ReadSfxName(const std::wstring &path, const SfxInfo &info,
                        std::wstring *outName) {
    outName->clear();
    if (info.nameLen == 0) return true;
    HANDLE h = OpenShared(path);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER pos;
    pos.QuadPart = (long long)(info.payloadOff + info.payloadLen);
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) { CloseHandle(h); return false; }
    std::string raw(info.nameLen, '\0');
    DWORD got = 0;
    BOOL ok = ReadFile(h, &raw[0], info.nameLen, &got, nullptr);
    CloseHandle(h);
    if (!ok || got != info.nameLen) return false;
    int n = MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), nullptr, 0);
    if (n <= 0) return false;
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, raw.data(), (int)raw.size(), &w[0], n);
    *outName = w;
    return true;
}

/* Path helpers. */
static std::wstring Basename(const std::wstring &path) {
    size_t s = path.find_last_of(L"\\/:");
    return (s == std::wstring::npos) ? path : path.substr(s + 1);
}
static std::wstring DirWithSlash(const std::wstring &path) {
    size_t s = path.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? std::wstring() : path.substr(0, s + 1);
}

/* Reduce a stored name to a safe basename in the output directory (blocks any
 * path traversal); matches the CLI's sfx_basename fallback. */
static std::wstring SafeStoredName(const std::wstring &stored) {
    std::wstring b = Basename(stored);
    if (b.empty() || b == L"." || b == L"..") return L"extracted.out";
    return b;
}

/* Default output path when building an SFX: swap the source's final extension
 * for ".exe", keeping its directory. Never collides with the source. */
static std::wstring SfxOutputPath(const std::wstring &src) {
    std::wstring dir = DirWithSlash(src), base = Basename(src);
    size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos && dot != 0) base = base.substr(0, dot);
    std::wstring out = dir + base + L".exe";
    if (_wcsicmp(out.c_str(), src.c_str()) == 0) out = src + L".exe";
    return out;
}

/* Length of our own executable's stub — i.e. everything before any SFX we may
 * already carry, so building an SFX from a self-extracting build re-uses the
 * clean stub instead of nesting payloads. */
static bool OwnStubLength(uint64_t *stubLen) {
    std::wstring self = ExePath();
    long long sz = FileSize(self);
    if (sz < 0) return false;
    SfxInfo info;
    *stubLen = ProbeSfx(self, &info) ? info.payloadOff : (uint64_t)sz;
    return true;
}

/* A scratch path in %TEMP% for the intermediate .sq payload. Empty on error. */
static std::wstring MakeTempPath(void) {
    wchar_t dir[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir)) return std::wstring();
    wchar_t path[MAX_PATH];
    if (!GetTempFileNameW(dir, L"wsq", 0, path)) return std::wstring();
    return path;
}

/* Copy `len` bytes from `in` (starting at `start`) to the current position of
 * `out`. Used to lay down the stub and payload without loading them whole. */
static bool CopyRange(HANDLE in, HANDLE out, uint64_t start, uint64_t len) {
    LARGE_INTEGER pos; pos.QuadPart = (long long)start;
    if (!SetFilePointerEx(in, pos, nullptr, FILE_BEGIN)) return false;
    const DWORD CH = 1u << 20;
    std::string buf(CH, '\0');
    uint64_t left = len;
    while (left) {
        DWORD want = (DWORD)(left < CH ? left : CH), got = 0;
        if (!ReadFile(in, &buf[0], want, &got, nullptr) || got == 0) return false;
        for (DWORD off = 0; off < got; ) {
            DWORD wr = 0;
            if (!WriteFile(out, buf.data() + off, got - off, &wr, nullptr)) return false;
            off += wr;
        }
        left -= got;
    }
    return true;
}

/* Assemble outPath = [stub][payload][name][trailer]. On any failure the
 * partial output is deleted. */
static int WriteSfx(uint64_t stubLen, const std::wstring &payloadPath,
                    uint64_t payloadLen, const std::string &nameUtf8,
                    const std::wstring &outPath) {
    HANDLE in = OpenShared(ExePath());
    if (in == INVALID_HANDLE_VALUE) return SQUISH_E_IO;
    HANDLE out = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) { CloseHandle(in); return SQUISH_E_IO; }

    int rc = SQUISH_OK;
    if (!CopyRange(in, out, 0, stubLen)) rc = SQUISH_E_IO;
    CloseHandle(in);

    if (rc == SQUISH_OK) {
        HANDLE pf = OpenShared(payloadPath);
        if (pf == INVALID_HANDLE_VALUE) rc = SQUISH_E_IO;
        else { if (!CopyRange(pf, out, 0, payloadLen)) rc = SQUISH_E_IO;
               CloseHandle(pf); }
    }
    if (rc == SQUISH_OK && !nameUtf8.empty()) {
        DWORD wr = 0;
        if (!WriteFile(out, nameUtf8.data(), (DWORD)nameUtf8.size(), &wr, nullptr) ||
            wr != nameUtf8.size()) rc = SQUISH_E_IO;
    }
    if (rc == SQUISH_OK) {
        unsigned char tr[SFX_TRAILER_LEN] = { 0 };
        memcpy(tr, SFX_MAGIC, SFX_MAGIC_LEN);
        PutU64LE(tr + 8,  stubLen);
        PutU64LE(tr + 16, payloadLen);
        PutU32LE(tr + 24, (uint32_t)nameUtf8.size());
        PutU32LE(tr + 28, 0);
        DWORD wr = 0;
        if (!WriteFile(out, tr, SFX_TRAILER_LEN, &wr, nullptr) ||
            wr != SFX_TRAILER_LEN) rc = SQUISH_E_IO;
    }
    CloseHandle(out);
    if (rc != SQUISH_OK) DeleteFileW(outPath.c_str());
    return rc;
}

/* --- directory archives ("SQAR02", docs FORMAT.md §12) ---------------------
 * A directory is packed into a seekable SQAR archive entirely by libsquish
 * (squish_archive_create): each file becomes its own compressed stream behind
 * a compact index, so a reader can list members and inflate one file or subtree
 * by seeking straight to it — never touching the rest. WinSquish no longer
 * serializes the tree itself; it just calls the archive API for creation,
 * listing (the browser), and extraction. The small helpers below (UTF-8
 * conversion, whole-file/range I/O, mkdir -p) remain because the SFX container
 * and the browser's own file writes still need them. */

/* UTF-8 (n bytes) -> wide. */
static std::wstring FromUtf8(const char *s, int n) {
    if (n <= 0) return std::wstring();
    int w = MultiByteToWideChar(CP_UTF8, 0, s, n, nullptr, 0);
    if (w <= 0) return std::wstring();
    std::wstring r(w, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, n, &r[0], w);
    return r;
}

static bool IsDirectory(const std::wstring &p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

/* Read an entire file into `out`. false on open/read failure or if it does not
 * fit in memory (size_t). */
static bool ReadWholeFile(const std::wstring &path, std::string *out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li) || li.QuadPart < 0 ||
        (unsigned long long)li.QuadPart > (size_t)-1) { CloseHandle(h); return false; }
    out->resize((size_t)li.QuadPart);
    size_t left = out->size(), off = 0;
    bool ok = true;
    while (left) {
        DWORD want = (DWORD)(left < (1u << 20) ? left : (1u << 20)), got = 0;
        if (!ReadFile(h, &(*out)[off], want, &got, nullptr) || got == 0) { ok = false; break; }
        off += got; left -= got;
    }
    CloseHandle(h);
    return ok;
}

/* Read [off, off+len) of `path` into `out`. */
static bool ReadRange(const std::wstring &path, uint64_t off, uint64_t len,
                      std::string *out) {
    if (len > (size_t)-1) return false;
    HANDLE h = OpenShared(path);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER pos; pos.QuadPart = (long long)off;
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) { CloseHandle(h); return false; }
    out->resize((size_t)len);
    size_t leftb = (size_t)len, o = 0;
    bool ok = true;
    while (leftb) {
        DWORD want = (DWORD)(leftb < (1u << 20) ? leftb : (1u << 20)), got = 0;
        if (!ReadFile(h, &(*out)[o], want, &got, nullptr) || got == 0) { ok = false; break; }
        o += got; leftb -= got;
    }
    CloseHandle(h);
    return ok;
}

/* Write `len` bytes to `path` (overwriting). 0 on success, -1 on failure (the
 * partial file is removed). */
static int WriteWholeFile(const std::wstring &path, const void *data, size_t len) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    const unsigned char *p = (const unsigned char *)data;
    size_t left = len;
    bool ok = true;
    while (left) {
        DWORD want = (DWORD)(left < (1u << 20) ? left : (1u << 20)), wr = 0;
        if (!WriteFile(h, p, want, &wr, nullptr) || wr == 0) { ok = false; break; }
        p += wr; left -= wr;
    }
    CloseHandle(h);
    if (!ok) DeleteFileW(path.c_str());
    return ok ? 0 : -1;
}

/* Create `path` and every missing parent directory. Absolute paths only.
 * Returns true once `path` exists as a directory. */
static bool MakeDirTree(const std::wstring &path) {
    if (path.empty()) return false;
    for (size_t i = 1; i < path.size(); i++)
        if (path[i] == L'\\' || path[i] == L'/') {
            std::wstring sub = path.substr(0, i);
            CreateDirectoryW(sub.c_str(), nullptr);   /* ignore "already exists" */
        }
    CreateDirectoryW(path.c_str(), nullptr);
    return IsDirectory(path);
}


/* --- context-menu registration (Software\Classes) --------------------------
 * Registration is written under one of two roots:
 *   HKEY_CURRENT_USER  — a per-user install: no admin rights, this user only.
 *   HKEY_LOCAL_MACHINE — a system-wide install: all users, needs elevation.
 * The key layout below is identical either way; only the root differs. The
 * installer picks the root (all-users vs. per-user) and passes --allusers to
 * winsquish --register/--unregister; the GUI's Tools menu always uses HKCU. */
static bool SetRegValue(HKEY root, const std::wstring &key, const wchar_t *name,
                        const std::wstring &val) {
    HKEY hk;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &hk, nullptr) != ERROR_SUCCESS)
        return false;
    LONG rc = RegSetValueExW(hk, name, 0, REG_SZ, (const BYTE *)val.c_str(),
                             (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
    return rc == ERROR_SUCCESS;
}

static bool RegisterShell(HKEY root) {
    std::wstring exe = ExePath();
    std::wstring quoted = L"\"" + exe + L"\"";
    bool ok = true;

    /* "Compress to .sq" on every file type */
    std::wstring k = L"Software\\Classes\\*\\shell\\WinSquish.Compress";
    ok &= SetRegValue(root,k, nullptr, L"Compress to .sq (WinSquish)");
    ok &= SetRegValue(root,k, L"Icon", quoted);
    ok &= SetRegValue(root,k + L"\\command", nullptr, quoted + L" --compress \"%1\"");

    /* "Compress to self-extracting .exe" on every file type */
    std::wstring ksfx = L"Software\\Classes\\*\\shell\\WinSquish.CompressSfx";
    ok &= SetRegValue(root,ksfx, nullptr,
                      L"Compress to self-extracting .exe (WinSquish)");
    ok &= SetRegValue(root,ksfx, L"Icon", quoted);
    ok &= SetRegValue(root,ksfx + L"\\command", nullptr,
                      quoted + L" --compress-sfx \"%1\"");

    /* The same two verbs on folders (right-click a directory): the whole tree
     * is packed into a single SQAR archive stream. */
    std::wstring kd = L"Software\\Classes\\Directory\\shell\\WinSquish.Compress";
    ok &= SetRegValue(root,kd, nullptr, L"Compress to .sq (WinSquish)");
    ok &= SetRegValue(root,kd, L"Icon", quoted);
    ok &= SetRegValue(root,kd + L"\\command", nullptr, quoted + L" --compress \"%1\"");

    std::wstring kdsfx =
        L"Software\\Classes\\Directory\\shell\\WinSquish.CompressSfx";
    ok &= SetRegValue(root,kdsfx, nullptr,
                      L"Compress to self-extracting .exe (WinSquish)");
    ok &= SetRegValue(root,kdsfx, L"Icon", quoted);
    ok &= SetRegValue(root,kdsfx + L"\\command", nullptr,
                      quoted + L" --compress-sfx \"%1\"");

    /* .sq extension -> ProgID */
    ok &= SetRegValue(root,L"Software\\Classes\\" + std::wstring(SQ_EXT),
                      nullptr, PROGID);

    /* ProgID: icon, double-click opens GUI, "View files" + "Extract" verbs */
    std::wstring p = L"Software\\Classes\\" + std::wstring(PROGID);
    ok &= SetRegValue(root,p, nullptr, L"SQUISH Compressed File");
    ok &= SetRegValue(root,p + L"\\DefaultIcon", nullptr, quoted + L",0");
    ok &= SetRegValue(root,p + L"\\shell\\open\\command", nullptr,
                      quoted + L" \"%1\"");
    ok &= SetRegValue(root,p + L"\\shell\\view", nullptr,
                      L"View files with WinSquish");
    ok &= SetRegValue(root,p + L"\\shell\\view", L"Icon", quoted);
    ok &= SetRegValue(root,p + L"\\shell\\view\\command", nullptr,
                      quoted + L" --view \"%1\"");
    ok &= SetRegValue(root,p + L"\\shell\\extract", nullptr,
                      L"Extract with WinSquish");
    ok &= SetRegValue(root,p + L"\\shell\\extract", L"Icon", quoted);
    ok &= SetRegValue(root,p + L"\\shell\\extract\\command", nullptr,
                      quoted + L" --decompress \"%1\"");

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

static void UnregisterShell(HKEY root) {
    RegDeleteTreeW(root,
                   L"Software\\Classes\\*\\shell\\WinSquish.Compress");
    RegDeleteTreeW(root,
                   L"Software\\Classes\\*\\shell\\WinSquish.CompressSfx");
    RegDeleteTreeW(root,
                   L"Software\\Classes\\Directory\\shell\\WinSquish.Compress");
    RegDeleteTreeW(root,
                   L"Software\\Classes\\Directory\\shell\\WinSquish.CompressSfx");
    RegDeleteTreeW(root,
                   (L"Software\\Classes\\" + std::wstring(PROGID)).c_str());
    /* remove the extension mapping only if it still points at us */
    HKEY hk;
    std::wstring extKey = L"Software\\Classes\\" + std::wstring(SQ_EXT);
    if (RegOpenKeyExW(root, extKey.c_str(), 0,
                      KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS) {
        wchar_t val[64] = L"";
        DWORD cb = sizeof val, type = 0;
        RegQueryValueExW(hk, nullptr, nullptr, &type, (BYTE *)val, &cb);
        RegCloseKey(hk);
        if (type == REG_SZ && _wcsicmp(val, PROGID) == 0)
            RegDeleteTreeW(root, extKey.c_str());
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

/* --- worker thread ----------------------------------------------------------*/
static void SquishProgress(uint64_t done, uint64_t total, void *user) {
    Job *job = (Job *)user;
    LONG pct = total ? (LONG)(100.0 * (double)done / (double)total) : 100;
    if (InterlockedExchange(&job->lastPct, pct) != pct)
        PostMessageW(job->hwnd, WM_APP_PROGRESS, (WPARAM)pct, 0);
}

/* Classify the payload beginning at `off` in `path`: 'a' = SQAR archive,
 * 's' = plain §1 stream, 0 = neither. Reads only the first bytes. */
static char ProbePayloadKind(const std::wstring &path, uint64_t off) {
    HANDLE h = OpenShared(path);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER pos; pos.QuadPart = (long long)off;
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) { CloseHandle(h); return 0; }
    unsigned char hd[16]; DWORD got = 0;
    BOOL ok = ReadFile(h, hd, sizeof hd, &got, nullptr);
    CloseHandle(h);
    if (!ok || got < 12) return 0;
    if (squish_archive_probe(hd, (size_t)got)) return 'a';
    uint64_t sz;
    if (squish_decompressed_size(hd, (size_t)got, &sz) == SQUISH_OK) return 's';
    return 0;
}

/* Open the archive browser (defined near the browser window code below). */
static void OpenBrowser(const std::wstring &archivePath, int threads);

/* Compress job->src to `outPath`. A directory becomes a seekable SQAR archive
 * (each file its own stream behind an index); a single file becomes a plain
 * SQUISH stream. libsquish does all the archive work. */
static int CompressToStream(Job *job, const std::wstring &outPath) {
    std::string src = ToUtf8(job->src), out = ToUtf8(outPath);
    if (IsDirectory(job->src))
        return squish_archive_create(src.c_str(), out.c_str(),
                                     job->threads, 0, SquishProgress, job);
    return job->threads > 1
         ? squish_compress_file_mt(src.c_str(), out.c_str(), job->threads, 0,
                                   SquishProgress, job)
         : squish_compress_file2(src.c_str(), out.c_str(), SquishProgress, job);
}

/* Extract a whole SQAR archive (every member) into the directory job->dst.
 * For an SFX the archive image is in memory (`data`/`len`, the payload region);
 * for a plain .sq we open the file by path so libsquish seeks per member. */
static int ExtractArchive(Job *job, const void *data, size_t len) {
    squish_archive *a = nullptr;
    int rc;
    if (job->sfx) {
        rc = squish_archive_open_memory(data, len, &a);
    } else {
        std::string src = ToUtf8(job->src);
        rc = squish_archive_open(src.c_str(), &a);
    }
    if (rc != SQUISH_OK) return rc;
    if (!MakeDirTree(job->dst)) { squish_archive_close(a); return SQUISH_E_IO; }
    std::string dst = ToUtf8(job->dst);
    rc = squish_archive_extract_subtree(a, nullptr, dst.c_str(),
                                        SquishProgress, job);
    squish_archive_close(a);
    return rc;
}

/* Extract a single §1 stream to the file job->dst. For an SFX the compressed
 * bytes are in memory (`data`/`len`); for a plain .sq we stream from the file. */
static int ExtractSingle(Job *job, const void *data, size_t len) {
    if (job->sfx) {
        void *raw = nullptr; size_t rn = 0;
        int rc = squish_decompress_alloc_mt(data, len, &raw, &rn,
                                            job->threads, SquishProgress, job);
        if (rc != SQUISH_OK) return rc;
        rc = WriteWholeFile(job->dst, raw, rn) == 0 ? SQUISH_OK : SQUISH_E_IO;
        squish_free(raw);
        return rc;
    }
    std::string src = ToUtf8(job->src), dst = ToUtf8(job->dst);
    return squish_decompress_file_mt(src.c_str(), dst.c_str(), job->threads,
                                     SquishProgress, job);
}

/* Build job->dst as a self-extracting .exe: compress the source (file or
 * directory) to a scratch payload (a §1 stream for a file, a whole SQAR archive
 * for a directory — the slow part, over which progress is reported), then
 * splice [stub][payload][name][trailer] into the output. */
static int SfxCompress(Job *job) {
    std::wstring tmp = MakeTempPath();
    if (tmp.empty()) return SQUISH_E_IO;
    int rc = CompressToStream(job, tmp);
    if (rc != SQUISH_OK) { DeleteFileW(tmp.c_str()); return rc; }

    uint64_t stubLen = 0;
    long long plen = FileSize(tmp);
    std::string name = ToUtf8(SafeStoredName(job->src));
    if (name.size() > SFX_MAX_NAME) name.resize(SFX_MAX_NAME);
    if (!OwnStubLength(&stubLen) || plen < 0) {
        DeleteFileW(tmp.c_str());
        return SQUISH_E_IO;
    }
    rc = WriteSfx(stubLen, tmp, (uint64_t)plen, name, job->dst);
    DeleteFileW(tmp.c_str());
    return rc;
}

/* Run an extraction job: read the SFX payload region into memory if needed,
 * then dispatch on job->tree (archive tree vs. single file). */
static int DoExtract(Job *job) {
    std::string payload;
    const void *data = nullptr; size_t len = 0;
    if (job->sfx) {
        SfxInfo info;
        if (!ProbeSfx(job->src, &info)) return SQUISH_E_FORMAT;
        if (!ReadRange(job->src, info.payloadOff, info.payloadLen, &payload))
            return SQUISH_E_IO;
        data = payload.data(); len = payload.size();
    }
    return job->tree ? ExtractArchive(job, data, len)
                     : ExtractSingle(job, data, len);
}

static DWORD WINAPI WorkerProc(LPVOID param) {
    Job *job = (Job *)param;
    int rc = job->compress
           ? (job->sfx ? SfxCompress(job) : CompressToStream(job, job->dst))
           : DoExtract(job);
    PostMessageW(job->hwnd, WM_APP_DONE, (WPARAM)rc, 0);
    return 0;
}

/* --- UI --------------------------------------------------------------------*/
static void SetStatus(const std::wstring &s) {
    SetDlgItemTextW(g_hwnd, IDC_STATUS, s.c_str());
}

/* Make Extract (true) or Compress (false) the default (Enter) button. */
static void SetDefaultButton(bool extractDefault) {
    SendDlgItemMessageW(g_hwnd, IDC_EXTRACT_BTN, BM_SETSTYLE,
                        extractDefault ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON, TRUE);
    SendDlgItemMessageW(g_hwnd, IDC_COMPRESS_BTN, BM_SETSTYLE,
                        extractDefault ? BS_PUSHBUTTON : BS_DEFPUSHBUTTON, TRUE);
}

static void UpdateFileInfo(void) {
    wchar_t path[MAX_PATH];
    GetDlgItemTextW(g_hwnd, IDC_FILE_EDIT, path, MAX_PATH);
    if (!path[0]) { SetDlgItemTextW(g_hwnd, IDC_INFO_STATIC, L""); return; }

    /* A folder is packed into a single SQAR archive stream before compression. */
    if (IsDirectory(path)) {
        bool sfxMode = IsDlgButtonChecked(g_hwnd, IDC_SFX_CHECK) == BST_CHECKED;
        std::wstring out = sfxMode ? SfxOutputPath(path)
                                   : std::wstring(path) + SQ_EXT;
        std::wstring info = L"folder  —  will pack + " +
                std::wstring(sfxMode ? L"build" : L"compress to") +
                L" \"" + Basename(out) + L"\"";
        SetDefaultButton(false);
        SetDlgItemTextW(g_hwnd, IDC_INFO_STATIC, info.c_str());
        return;
    }

    long long n = FileSize(path);
    if (n < 0) {
        SetDlgItemTextW(g_hwnd, IDC_INFO_STATIC, L"File or folder not found.");
        return;
    }
    std::wstring info = PrettySize(n);
    SfxInfo sfx;
    uint64_t orig;
    if (ProbeSfx(path, &sfx)) {
        std::wstring stored;
        ReadSfxName(path, sfx, &stored);
        bool arc = ProbePayloadKind(path, sfx.payloadOff) == 'a';
        info += arc ? L"  —  self-extracting SQUISH archive (folder)"
                    : L"  —  self-extracting SQUISH archive";
        if (!stored.empty())
            info += L", extracts to \"" + SafeStoredName(stored) + L"\"";
        SetDefaultButton(true);
    } else if (ProbePayloadKind(path, 0) == 'a') {
        std::string u8 = ToUtf8(path);
        squish_archive *a = nullptr;
        squish_archive_info ai;
        if (squish_archive_open(u8.c_str(), &a) == SQUISH_OK &&
            squish_archive_info_get(a, &ai) == SQUISH_OK) {
            wchar_t t[96];
            swprintf(t, 96, L"  —  SQUISH archive, %llu items, %s uncompressed",
                     (unsigned long long)ai.entry_count,
                     PrettySize((long long)ai.total_size).c_str());
            info += t;
        } else {
            info += L"  —  SQUISH directory archive";
        }
        if (a) squish_archive_close(a);
        SetDefaultButton(true);
    } else if (ReadSqHeader(path, &orig)) {
        info += L"  —  SQUISH stream, original size " +
                PrettySize((long long)orig);
        SetDefaultButton(true);
    } else {
        bool sfxMode = IsDlgButtonChecked(g_hwnd, IDC_SFX_CHECK) == BST_CHECKED;
        std::wstring out = sfxMode ? SfxOutputPath(path)
                                   : std::wstring(path) + SQ_EXT;
        info += L"  —  will " +
                std::wstring(sfxMode ? L"build" : L"compress to") +
                L" \"" + Basename(out) + L"\"";
        SetDefaultButton(false);
    }
    SetDlgItemTextW(g_hwnd, IDC_INFO_STATIC, info.c_str());
}

static void SetBusy(bool busy) {
    g_busy = busy;
    for (int id : { IDC_FILE_EDIT, IDC_BROWSE_BTN, IDC_CPU_COMBO, IDC_SFX_CHECK,
                    IDC_COMPRESS_BTN, IDC_EXTRACT_BTN, IDC_VIEW_BTN })
        EnableWindow(GetDlgItem(g_hwnd, id), !busy);
    if (!busy)
        SendDlgItemMessageW(g_hwnd, IDC_PROGRESS, PBM_SETPOS, 0, 0);
}

/* Worker count chosen in the CPU-cores combo (items are "1".."ncpu", so the
 * value is the selection index + 1). Falls back to the default if unset. */
static int SelectedThreads(void) {
    LRESULT sel = SendDlgItemMessageW(g_hwnd, IDC_CPU_COMBO, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) {
        int ncpu = squish_threads();
        return ncpu < DEFAULT_THREADS ? ncpu : DEFAULT_THREADS;
    }
    return (int)sel + 1;
}

/* Derive the output path for a job. Compress: append .sq.
 * Extract: strip .sq, or append .out if the name has no .sq suffix. */
static std::wstring OutputPath(const std::wstring &src, bool compress) {
    if (compress) return src + SQ_EXT;
    if (HasSqExt(src)) return src.substr(0, src.size() - wcslen(SQ_EXT));
    return src + L".out";
}

static void StartJob(bool compress) {
    if (g_busy) return;
    wchar_t buf[MAX_PATH];
    GetDlgItemTextW(g_hwnd, IDC_FILE_EDIT, buf, MAX_PATH);
    std::wstring src = buf;
    if (src.empty()) {
        MessageBoxW(g_hwnd, L"Choose a file or folder first.", APP_NAME,
                    MB_ICONINFORMATION);
        return;
    }
    bool srcIsDir = IsDirectory(src);
    if (!srcIsDir && FileSize(src) < 0) {
        MessageBoxW(g_hwnd, (L"File not found:\n" + src).c_str(), APP_NAME,
                    MB_ICONERROR);
        return;
    }
    if (srcIsDir && !compress) {
        MessageBoxW(g_hwnd, L"A folder can be compressed, but not extracted.\n"
                            L"Choose a .sq stream or self-extracting archive to "
                            L"extract.", APP_NAME, MB_ICONINFORMATION);
        return;
    }

    /* Decide direction, whether SFX is involved, whether the output is a
     * directory tree (an archive) or a single file, and the output path. */
    bool sfx = false, tree = false;
    std::wstring dst;
    if (compress) {
        sfx = IsDlgButtonChecked(g_hwnd, IDC_SFX_CHECK) == BST_CHECKED;
        tree = srcIsDir;
        dst = sfx ? SfxOutputPath(src) : src + SQ_EXT;
    } else {
        SfxInfo si;
        if (ProbeSfx(src, &si)) {
            sfx = true;
            tree = ProbePayloadKind(src, si.payloadOff) == 'a';
            std::wstring stored;
            ReadSfxName(src, si, &stored);
            dst = DirWithSlash(src) + SafeStoredName(stored);
        } else {
            char kind = ProbePayloadKind(src, 0);
            if (kind == 0) {
                MessageBoxW(g_hwnd,
                    L"This file is not a SQUISH stream, archive, or "
                    L"self-extracting archive (bad or missing header).",
                    APP_NAME, MB_ICONERROR);
                return;
            }
            sfx = false;
            tree = (kind == 'a');
            dst = OutputPath(src, false);
        }
    }

    if (_wcsicmp(dst.c_str(), src.c_str()) == 0) {
        MessageBoxW(g_hwnd,
            L"The output would overwrite the source file. Rename the source, "
            L"or choose a different file.", APP_NAME, MB_ICONERROR);
        return;
    }
    if (FileSize(dst) >= 0) {
        std::wstring q = dst + L"\nalready exists. Overwrite?";
        if (MessageBoxW(g_hwnd, q.c_str(), APP_NAME,
                        MB_YESNO | MB_ICONWARNING) != IDYES)
            return;
    }

    g_job = new Job{ g_hwnd, src, dst, compress, sfx, srcIsDir, tree,
                     SelectedThreads(), -1 };
    g_t0 = GetTickCount64();
    SetBusy(true);
    SetStatus(compress ? (sfx ? L"Building self-extracting archive…"
                              : L"Compressing…")
                       : L"Extracting…");
    HANDLE th = CreateThread(nullptr, 0, WorkerProc, g_job, 0, nullptr);
    if (!th) {
        SetBusy(false);
        delete g_job; g_job = nullptr;
        SetStatus(L"Failed to start worker thread.");
        return;
    }
    CloseHandle(th);
}

/* "View Files": open the selected archive in the browser window. A seekable
 * SQAR archive opens instantly (only its header + index are read); a plain
 * single-file stream shows as one member. The heavy lifting — decompressing an
 * individual member — happens later, only when the user extracts it. */
static void StartView(void) {
    if (g_busy) return;
    wchar_t buf[MAX_PATH];
    GetDlgItemTextW(g_hwnd, IDC_FILE_EDIT, buf, MAX_PATH);
    std::wstring src = buf;
    if (src.empty()) {
        MessageBoxW(g_hwnd, L"Choose a .sq or self-extracting archive first.",
                    APP_NAME, MB_ICONINFORMATION);
        return;
    }
    if (IsDirectory(src) || FileSize(src) < 0) {
        MessageBoxW(g_hwnd,
            L"Choose an existing .sq stream or self-extracting archive to view.",
            APP_NAME, MB_ICONINFORMATION);
        return;
    }
    OpenBrowser(src, SelectedThreads());
}

static void OnJobDone(int rc) {
    Job *job = g_job; g_job = nullptr;
    double secs = (GetTickCount64() - g_t0) / 1000.0;
    SetBusy(false);
    if (!job) return;

    if (rc != SQUISH_OK) {
        wchar_t err[128];
        MultiByteToWideChar(CP_UTF8, 0, squish_strerror(rc), -1, err, 128);
        SetStatus(std::wstring(L"Failed: ") + err);
        DeleteFileW(job->dst.c_str());   /* don't leave partial output */
        MessageBoxW(g_hwnd, (std::wstring(L"Operation failed:\n") + err).c_str(),
                    APP_NAME, MB_ICONERROR);
    } else {
        long long in = FileSize(job->src), out = FileSize(job->dst);
        bool outIsDir = IsDirectory(job->dst);
        wchar_t msg[512];
        if (job->compress && job->sfx) {
            if (job->srcIsDir)
                swprintf(msg, 512,
                         L"Done in %.1f s — folder → self-extracting archive %s",
                         secs, PrettySize(out).c_str());
            else
                swprintf(msg, 512,
                         L"Done in %.1f s — self-extracting archive %s → %s",
                         secs, PrettySize(in).c_str(), PrettySize(out).c_str());
        } else if (job->compress) {
            if (job->srcIsDir)
                swprintf(msg, 512,
                         L"Done in %.1f s — folder packed + compressed to %s (%s)",
                         secs, Basename(job->dst).c_str(), PrettySize(out).c_str());
            else {
                double ratio = (in > 0 && out > 0) ? 100.0 * out / in : 0;
                swprintf(msg, 512,
                         L"Done in %.1f s — %s → %s (%.1f%% of original)",
                         secs, PrettySize(in).c_str(), PrettySize(out).c_str(),
                         ratio);
            }
        } else if (outIsDir) {
            swprintf(msg, 512,
                     L"Done in %.1f s — extracted folder \"%s\" (checksum OK)",
                     secs, Basename(job->dst).c_str());
        } else {
            swprintf(msg, 512, L"Done in %.1f s — extracted %s (checksum OK)",
                     secs, PrettySize(out).c_str());
        }
        SetStatus(msg);
        SendDlgItemMessageW(g_hwnd, IDC_PROGRESS, PBM_SETPOS, 100, 0);
        /* reveal the result in Explorer-friendly fashion: select the file box */
        SetDlgItemTextW(g_hwnd, IDC_FILE_EDIT, job->dst.c_str());
        UpdateFileInfo();
    }
    delete job;
}

static void BrowseFile(void) {
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW ofn = { sizeof ofn };
    ofn.hwndOwner   = g_hwnd;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrFilter = L"All files\0*.*\0SQUISH files (*.sq)\0*.sq\0";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        SetDlgItemTextW(g_hwnd, IDC_FILE_EDIT, buf);
        UpdateFileInfo();
    }
}

/* Pick a folder to compress (IFileOpenDialog in pick-folders mode). */
static void BrowseFolder(void) {
    IFileOpenDialog *dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (SUCCEEDED(dlg->Show(g_hwnd))) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                SetDlgItemTextW(g_hwnd, IDC_FILE_EDIT, path);
                UpdateFileInfo();
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
}

static HRESULT CALLBACK AboutCallback(HWND, UINT notification, WPARAM,
                                      LPARAM lp, LONG_PTR) {
    if (notification == TDN_HYPERLINK_CLICKED)
        ShellExecuteW(nullptr, L"open", (const wchar_t *)lp,
                      nullptr, nullptr, SW_SHOWNORMAL);
    return S_OK;
}

static void ShowAbout(void) {
    wchar_t ver[64];
    MultiByteToWideChar(CP_UTF8, 0, squish_version(), -1, ver, 64);
    std::wstring msg =
        L"libsquish version " + std::wstring(ver) + L"\n\n"
        L"SQUISH predicts every bit with ten statistical models, fuses "
        L"them with a logistic mixer, and arithmetic-codes the result. "
        L"Excellent ratios; symmetric speed of roughly 0.5–0.7 MB/s "
        L"per core, so large files take a while.\n\n"
        L"WinSquish: <a href=\"https://github.com/paigejulianne/winsquish\">"
        L"github.com/paigejulianne/winsquish</a>\n"
        L"SQUISH library: <a href=\"https://github.com/paigejulianne/squish\">"
        L"github.com/paigejulianne/squish</a>\n\n"
        L"Licensed under the GNU GPL v3.";
    TASKDIALOGCONFIG tdc = { sizeof tdc };
    tdc.hwndParent         = g_hwnd;
    tdc.hInstance          = GetModuleHandleW(nullptr);
    tdc.dwFlags            = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tdc.dwCommonButtons    = TDCBF_OK_BUTTON;
    tdc.pszWindowTitle     = L"About WinSquish";
    tdc.pszMainIcon        = MAKEINTRESOURCEW(IDI_APP);
    tdc.pszMainInstruction = L"WinSquish — GUI for the SQUISH context-mixing "
                             L"compressor";
    tdc.pszContent         = msg.c_str();
    tdc.pfCallback         = AboutCallback;
    TaskDialogIndirect(&tdc, nullptr, nullptr, nullptr);
}

/* Scale a 96-dpi design coordinate to the window's DPI. */
static int g_dpi = 96;
static int S(int v) { return MulDiv(v, g_dpi, 96); }

/* ============================================================================
 * Archive browser — a WinRAR-style window listing the contents of an archive
 *
 * A seekable SQAR archive opens instantly — only its header and index are read
 * — and each file is inflated on demand, by seeking straight to its own stream,
 * when the user extracts it (libsquish's squish_archive_* API). A plain
 * single-file .sq (or SFX payload) shows as one member and is decompressed
 * whole on extraction. Navigate folders by double-clicking (Up / Backspace to
 * ascend), sort by column, and extract the current selection or everything.
 * ==========================================================================*/
static const wchar_t *BROWSER_CLASS = L"WinSquishBrowserWindow";

/* One archive member, as listed by the browser. */
struct ArcEntry {
    std::wstring path;    /* relative, '/'-separated                          */
    bool         isDir;
    uint64_t     size;    /* uncompressed size (0 for a directory)            */
    uint64_t     index;   /* member index for squish_archive_extract          */
};

/* One row of the current folder view (a child of the browser's cwd). */
struct ViewRow {
    std::wstring name;      /* leaf name shown in the Name column            */
    std::wstring fullPath;  /* archive-relative '/'-path, no trailing slash  */
    bool         isDir;
    bool         isUp;      /* the synthetic ".." row                        */
    uint64_t     size;
    int          entryIndex;/* index into Browser::entries (files); else -1  */
};

struct Browser {
    std::wstring          archivePath;  /* source .sq / .exe                  */
    squish_archive       *arc;          /* seekable handle; NULL if `single`  */
    std::string           payload;      /* SFX: bytes backing `arc`/`single`  */
    bool                  sfx;          /* source is a self-extracting .exe   */
    bool                  single;       /* not an archive: one file member    */
    std::vector<ArcEntry> entries;      /* every member                       */
    std::wstring          cwd;          /* "" (root) or "a/b/" with slash     */
    int                   threads;
    int                   sortCol;      /* 0 = name, 1 = size                 */
    bool                  sortAsc;
    std::vector<ViewRow>  view;         /* rows currently shown               */
    HWND                  hwnd;
    HWND                  hlist;
};

static bool StartsWith(const std::wstring &s, const std::wstring &pfx) {
    return s.size() >= pfx.size() &&
           wcsncmp(s.c_str(), pfx.c_str(), pfx.size()) == 0;
}

/* The system image list (small icons), so files show their real shell icon. */
static HIMAGELIST SysImageListSmall(void) {
    SHFILEINFOW sfi = { 0 };
    return (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof sfi,
                                      SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
}

/* System-image-list index for a name of the given kind, resolved purely from
 * the (fake) attributes + extension — the item need not exist on disk. */
static int IconIndex(const std::wstring &name, bool isDir) {
    SHFILEINFOW sfi = { 0 };
    DWORD attr = isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    SHGetFileInfoW(name.c_str(), attr, &sfi, sizeof sfi,
                   SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    return sfi.iIcon;
}

/* Text for the Type column ("File folder", "TXT file", or plain "File"). */
static std::wstring TypeText(const std::wstring &name, bool isDir) {
    if (isDir) return L"File folder";
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= name.size()) return L"File";
    std::wstring ext = name.substr(dot + 1);
    CharUpperBuffW(&ext[0], (DWORD)ext.size());
    return ext + L" file";
}

/* Rebuild Browser::view: the immediate children (folders then files) of cwd,
 * sorted per the active column. Directories are synthesized from deeper paths
 * as well as explicit directory entries, so intermediate folders always show. */
static void BuildView(Browser *b) {
    b->view.clear();
    const std::wstring &cwd = b->cwd;
    if (!cwd.empty()) {
        ViewRow up = { L"..", std::wstring(), true, true, 0, -1 };
        b->view.push_back(up);
    }
    std::set<std::wstring> dirSet;
    std::vector<ViewRow> dirs, files;
    for (size_t i = 0; i < b->entries.size(); i++) {
        const ArcEntry &e = b->entries[i];
        const std::wstring &p = e.path;
        if (p.size() <= cwd.size() || !StartsWith(p, cwd)) continue;
        std::wstring rem = p.substr(cwd.size());
        size_t slash = rem.find(L'/');
        if (slash != std::wstring::npos) {
            dirSet.insert(rem.substr(0, slash));
        } else if (e.isDir) {
            dirSet.insert(rem);
        } else {
            ViewRow r = { rem, p, false, false, e.size, (int)i };
            files.push_back(std::move(r));
        }
    }
    for (const std::wstring &d : dirSet) {
        ViewRow r = { d, cwd + d, true, false, 0, -1 };
        dirs.push_back(std::move(r));
    }
    auto cmp = [b](const ViewRow &x, const ViewRow &y) -> bool {
        if (b->sortCol == 1 && !x.isDir && !y.isDir && x.size != y.size)
            return b->sortAsc ? x.size < y.size : x.size > y.size;
        int c = _wcsicmp(x.name.c_str(), y.name.c_str());
        return b->sortAsc ? c < 0 : c > 0;
    };
    std::sort(dirs.begin(), dirs.end(), cmp);
    std::sort(files.begin(), files.end(), cmp);
    for (ViewRow &r : dirs)  b->view.push_back(std::move(r));
    for (ViewRow &r : files) b->view.push_back(std::move(r));
}

/* Push Browser::view into the ListView. */
static void FillList(Browser *b) {
    HWND lv = b->hlist;
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < b->view.size(); i++) {
        const ViewRow &r = b->view[i];
        LVITEMW it = { 0 };
        it.mask     = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        it.iItem    = (int)i;
        it.pszText  = const_cast<wchar_t *>(r.name.c_str());
        it.lParam   = (LPARAM)i;
        it.iImage   = r.isUp ? IconIndex(L"folder", true)
                             : IconIndex(r.name, r.isDir);
        int idx = ListView_InsertItem(lv, &it);
        if (idx < 0) continue;
        if (!r.isDir) {
            std::wstring sz = PrettySize((long long)r.size);
            ListView_SetItemText(lv, idx, 1, const_cast<wchar_t *>(sz.c_str()));
        }
        if (!r.isUp) {
            std::wstring tp = TypeText(r.name, r.isDir);
            ListView_SetItemText(lv, idx, 2, const_cast<wchar_t *>(tp.c_str()));
        }
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lv, nullptr, TRUE);
}

/* Parent of a "a/b/" cwd ("a/"); "" if already at the root. */
static std::wstring ParentCwd(const std::wstring &cwd) {
    if (cwd.empty()) return cwd;
    std::wstring t = cwd.substr(0, cwd.size() - 1);   /* drop trailing '/' */
    size_t s = t.find_last_of(L'/');
    return s == std::wstring::npos ? std::wstring() : t.substr(0, s + 1);
}

/* Move to a folder and refresh the list, path bar and Up button. */
static void NavigateTo(Browser *b, const std::wstring &cwd) {
    b->cwd = cwd;
    BuildView(b);
    FillList(b);
    std::wstring disp = L"\\" + b->cwd;
    for (wchar_t &c : disp) if (c == L'/') c = L'\\';
    SetWindowTextW(GetDlgItem(b->hwnd, IDC_PATH_STATIC), disp.c_str());
    EnableWindow(GetDlgItem(b->hwnd, IDC_UP_BTN), !b->cwd.empty());
}

/* Double-click / Enter on a row: descend into folders, ascend on "..". */
static void ActivateRow(Browser *b, int item) {
    LVITEMW it = { 0 };
    it.mask = LVIF_PARAM; it.iItem = item;
    if (!ListView_GetItem(b->hlist, &it)) return;
    size_t vi = (size_t)it.lParam;
    if (vi >= b->view.size()) return;
    const ViewRow &r = b->view[vi];
    if (r.isUp)       NavigateTo(b, ParentCwd(b->cwd));
    else if (r.isDir) NavigateTo(b, r.fullPath + L"/");
}

/* Pick a destination folder (IFileOpenDialog, pick-folders mode). */
static bool PickFolder(HWND owner, const std::wstring &initial,
                       std::wstring *out) {
    IFileOpenDialog *dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                    FOS_PATHMUSTEXIST);
    if (!initial.empty()) {
        IShellItem *si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr,
                                                  IID_PPV_ARGS(&si)))) {
            dlg->SetFolder(si);
            si->Release();
        }
    }
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem *item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                *out = p; CoTaskMemFree(p); ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

/* The on-disk path an entry extracts to under destRoot (its archive path with
 * '/'-separators turned into '\\'). */
static std::wstring EntryDest(const ArcEntry &e, const std::wstring &destRoot) {
    std::wstring rel = e.path;
    for (wchar_t &c : rel) if (c == L'/') c = L'\\';
    return destRoot + L"\\" + rel;
}

static bool PathExists(const std::wstring &p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

/* Inflate file member `e` to `full`, creating parent directories. Seeks to and
 * decodes only that member's stream when the archive is seekable; for a plain
 * single-file source it decompresses the whole stream (SFX payload held in
 * memory, or the .sq read from disk). Returns true on success. */
static bool ExtractMember(Browser *b, const ArcEntry &e,
                          const std::wstring &full) {
    size_t s = full.find_last_of(L'\\');
    if (s != std::wstring::npos && !MakeDirTree(full.substr(0, s))) return false;
    void *raw = nullptr; size_t rn = 0;
    int rc;
    if (b->arc) {
        rc = squish_archive_extract(b->arc, e.index, &raw, &rn);
    } else if (b->sfx) {
        rc = squish_decompress_alloc_mt(b->payload.data(), b->payload.size(),
                                        &raw, &rn, b->threads, nullptr, nullptr);
    } else {
        std::string comp;
        if (!ReadWholeFile(b->archivePath, &comp)) return false;
        rc = squish_decompress_alloc_mt(comp.data(), comp.size(),
                                        &raw, &rn, b->threads, nullptr, nullptr);
    }
    if (rc != SQUISH_OK) return false;
    bool ok = WriteWholeFile(full, raw, rn) == 0;
    squish_free(raw);
    return ok;
}

/* The user's answer to a "target already exists" prompt. */
enum OwResult { OW_YES, OW_NO, OW_CANCEL };

/* Ask whether to overwrite an existing target. `*applyAll` is set from the
 * dialog's "do this for all" checkbox so the caller can stop asking. */
static OwResult AskOverwrite(HWND owner, const std::wstring &path,
                             bool *applyAll) {
    std::wstring content = path + L"\n\nOverwrite the existing file?";
    TASKDIALOGCONFIG tdc = { sizeof tdc };
    tdc.hwndParent         = owner;
    tdc.hInstance          = GetModuleHandleW(nullptr);
    tdc.dwFlags            = TDF_ALLOW_DIALOG_CANCELLATION |
                             TDF_POSITION_RELATIVE_TO_WINDOW;
    tdc.dwCommonButtons    = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON |
                             TDCBF_CANCEL_BUTTON;
    tdc.pszWindowTitle     = APP_NAME;
    tdc.pszMainIcon        = TD_WARNING_ICON;
    tdc.pszMainInstruction = L"A file already exists at the destination.";
    tdc.pszContent         = content.c_str();
    tdc.pszVerificationText = L"Do this for all remaining conflicts";
    int  button = 0;
    BOOL verify = FALSE;
    if (FAILED(TaskDialogIndirect(&tdc, &button, nullptr, &verify)))
        return OW_CANCEL;
    if (applyAll) *applyAll = (verify == TRUE);
    if (button == IDYES) return OW_YES;
    if (button == IDNO)  return OW_NO;
    return OW_CANCEL;
}

/* Extract either the current selection (all == false) or the whole archive to a
 * folder the user picks. Selecting a folder pulls everything beneath it; output
 * always preserves the full archive-relative path (WinRAR's default). */
static void ExtractSelection(Browser *b, bool all) {
    std::vector<char> pick(b->entries.size(), 0);
    std::vector<std::wstring> emptyDirs;   /* selected dirs with no entries  */
    bool any = false;
    if (all) {
        for (size_t i = 0; i < b->entries.size(); i++) { pick[i] = 1; any = true; }
    } else {
        int i = -1;
        while ((i = ListView_GetNextItem(b->hlist, i, LVNI_SELECTED)) != -1) {
            LVITEMW it = { 0 };
            it.mask = LVIF_PARAM; it.iItem = i;
            if (!ListView_GetItem(b->hlist, &it)) continue;
            size_t vi = (size_t)it.lParam;
            if (vi >= b->view.size()) continue;
            const ViewRow &r = b->view[vi];
            if (r.isUp) continue;
            if (r.isDir) {
                std::wstring pfx = r.fullPath + L"/";
                bool hit = false;
                for (size_t k = 0; k < b->entries.size(); k++) {
                    const std::wstring &p = b->entries[k].path;
                    if (p == r.fullPath || StartsWith(p, pfx)) { pick[k] = 1; hit = true; }
                }
                if (!hit) emptyDirs.push_back(r.fullPath);
                any = true;
            } else if (r.entryIndex >= 0) {
                pick[(size_t)r.entryIndex] = 1; any = true;
            }
        }
    }
    if (!any) {
        MessageBoxW(b->hwnd, L"Select one or more files or folders to extract.",
                    APP_NAME, MB_ICONINFORMATION);
        return;
    }

    std::wstring dest;
    if (!PickFolder(b->hwnd, DirWithSlash(b->archivePath), &dest)) return;

    uint64_t nfiles = 0, nbytes = 0, nskip = 0;
    bool okAll = true, cancelled = false;
    enum { ASK, ALL_YES, ALL_NO } overwrite = ASK;   /* existing-file policy */

    /* Selected-but-empty directories just get created (merging into any that
     * already exist is harmless, so those never prompt). */
    for (const std::wstring &d : emptyDirs) {
        std::wstring rel = d;
        for (wchar_t &c : rel) if (c == L'/') c = L'\\';
        if (!MakeDirTree(dest + L"\\" + rel)) okAll = false;
    }

    HCURSOR wait = LoadCursorW(nullptr, IDC_WAIT);
    for (size_t k = 0; k < b->entries.size() && !cancelled; k++) {
        if (!pick[k]) continue;
        const ArcEntry &e = b->entries[k];
        std::wstring full = EntryDest(e, dest);

        /* Only files are ever overwritten; existing directories merge. */
        if (!e.isDir && PathExists(full)) {
            if (overwrite == ALL_NO) { nskip++; continue; }
            if (overwrite == ASK) {
                bool applyAll = false;
                OwResult r = AskOverwrite(b->hwnd, full, &applyAll);
                if (r == OW_CANCEL) { cancelled = true; break; }
                if (r == OW_NO)  { if (applyAll) overwrite = ALL_NO; nskip++; continue; }
                if (applyAll)    overwrite = ALL_YES;   /* r == OW_YES */
            }
        }
        HCURSOR old = SetCursor(wait);
        bool ok = e.isDir ? MakeDirTree(full) : ExtractMember(b, e, full);
        SetCursor(old);
        if (ok) { if (!e.isDir) { nfiles++; nbytes += e.size; } }
        else    okAll = false;
    }

    std::wstring tail = L" to:\n" + dest;
    if (nskip) {
        wchar_t s[64];
        swprintf(s, 64, L"\n(%llu existing file%s skipped)",
                 (unsigned long long)nskip, nskip == 1 ? L"" : L"s");
        tail += s;
    }
    wchar_t msg[700];
    const wchar_t *plural = nfiles == 1 ? L"" : L"s";
    if (cancelled)
        swprintf(msg, 700, L"Extraction cancelled — %llu file%s written%s",
                 (unsigned long long)nfiles, plural, tail.c_str());
    else if (okAll)
        swprintf(msg, 700, L"Extracted %llu file%s (%s)%s",
                 (unsigned long long)nfiles, plural,
                 PrettySize((long long)nbytes).c_str(), tail.c_str());
    else
        swprintf(msg, 700, L"Finished with errors — %llu file%s written%s",
                 (unsigned long long)nfiles, plural, tail.c_str());
    MessageBoxW(b->hwnd, msg, APP_NAME,
                (okAll && !cancelled) ? MB_ICONINFORMATION : MB_ICONWARNING);
}

static LRESULT CALLBACK BrowserProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Browser *b = (Browser *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        b = (Browser *)cs->lpCreateParams;
        b->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)b);
        HINSTANCE hi = GetModuleHandleW(nullptr);
        DWORD bs = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
        HWND up  = CreateWindowExW(0, L"BUTTON", L"Up", bs,
                                   0, 0, 0, 0, hwnd, (HMENU)IDC_UP_BTN, hi, nullptr);
        HWND es  = CreateWindowExW(0, L"BUTTON", L"Extract Selected…", bs,
                                   0, 0, 0, 0, hwnd, (HMENU)IDC_EXTRACT_SEL, hi, nullptr);
        HWND ea  = CreateWindowExW(0, L"BUTTON", L"Extract All…", bs,
                                   0, 0, 0, 0, hwnd, (HMENU)IDC_EXTRACT_ALL, hi, nullptr);
        HWND pth = CreateWindowExW(0, L"STATIC", L"\\",
                                   WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS | SS_CENTERIMAGE,
                                   0, 0, 0, 0, hwnd, (HMENU)IDC_PATH_STATIC, hi, nullptr);
        HWND lv  = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                   LVS_REPORT | LVS_SHOWSELALWAYS,
                                   0, 0, 0, 0, hwnd, (HMENU)IDC_LV, hi, nullptr);
        b->hlist = lv;
        for (HWND h : { up, es, ea, pth, lv })
            SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
        ListView_SetExtendedListViewStyle(lv,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
        HIMAGELIST il = SysImageListSmall();
        if (il) ListView_SetImageList(lv, il, LVSIL_SMALL);
        LVCOLUMNW c = { 0 };
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.pszText = (LPWSTR)L"Name"; c.cx = S(260); c.iSubItem = 0; ListView_InsertColumn(lv, 0, &c);
        c.pszText = (LPWSTR)L"Size"; c.cx = S(90);  c.iSubItem = 1; ListView_InsertColumn(lv, 1, &c);
        c.pszText = (LPWSTR)L"Type"; c.cx = S(120); c.iSubItem = 2; ListView_InsertColumn(lv, 2, &c);
        NavigateTo(b, L"");
        return 0;
    }

    case WM_SIZE: {
        if (!b || !b->hlist) return 0;
        RECT rc; GetClientRect(hwnd, &rc);
        int pad = S(8), bh = S(26), by = pad, bx = pad;
        MoveWindow(GetDlgItem(hwnd, IDC_UP_BTN),       bx, by, S(48),  bh, TRUE); bx += S(48)  + pad;
        MoveWindow(GetDlgItem(hwnd, IDC_EXTRACT_SEL),  bx, by, S(140), bh, TRUE); bx += S(140) + pad;
        MoveWindow(GetDlgItem(hwnd, IDC_EXTRACT_ALL),  bx, by, S(100), bh, TRUE); bx += S(100) + pad;
        int pathW = rc.right - bx - pad; if (pathW < S(40)) pathW = S(40);
        MoveWindow(GetDlgItem(hwnd, IDC_PATH_STATIC),  bx, by + S(4), pathW, S(18), TRUE);
        int lvY = by + bh + pad;
        MoveWindow(b->hlist, pad, lvY, rc.right - 2 * pad, rc.bottom - lvY - pad, TRUE);
        return 0;
    }

    case WM_COMMAND:
        if (!b) break;
        switch (LOWORD(wp)) {
        case IDC_UP_BTN:      NavigateTo(b, ParentCwd(b->cwd)); return 0;
        case IDC_EXTRACT_SEL: ExtractSelection(b, false);       return 0;
        case IDC_EXTRACT_ALL: ExtractSelection(b, true);        return 0;
        }
        break;

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (!b || nh->idFrom != IDC_LV) break;
        if (nh->code == NM_DBLCLK) {
            NMITEMACTIVATE *ia = (NMITEMACTIVATE *)lp;
            if (ia->iItem >= 0) ActivateRow(b, ia->iItem);
            return 0;
        }
        if (nh->code == LVN_KEYDOWN) {
            NMLVKEYDOWN *kd = (NMLVKEYDOWN *)lp;
            if (kd->wVKey == VK_RETURN) {
                int i = ListView_GetNextItem(b->hlist, -1, LVNI_FOCUSED);
                if (i >= 0) ActivateRow(b, i);
                return 0;
            }
            if (kd->wVKey == VK_BACK) { NavigateTo(b, ParentCwd(b->cwd)); return 0; }
        }
        if (nh->code == LVN_COLUMNCLICK) {
            NMLISTVIEW *nl = (NMLISTVIEW *)lp;
            if (nl->iSubItem == b->sortCol) b->sortAsc = !b->sortAsc;
            else { b->sortCol = nl->iSubItem; b->sortAsc = true; }
            BuildView(b); FillList(b);
            return 0;
        }
        break;
    }

    case WM_DESTROY:
        if (b) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (b->arc) squish_archive_close(b->arc);
            delete b;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Fill b->entries from an open seekable archive handle. */
static int LoadArchiveEntries(Browser *b) {
    uint64_t n = squish_archive_count(b->arc);
    b->entries.reserve((size_t)n);
    for (uint64_t i = 0; i < n; i++) {
        squish_archive_entry e;
        if (squish_archive_stat(b->arc, i, &e) != SQUISH_OK) return SQUISH_E_FORMAT;
        ArcEntry a;
        a.path  = FromUtf8(e.path, (int)strlen(e.path));
        a.isDir = e.is_dir != 0;
        a.size  = e.size;
        a.index = i;
        b->entries.push_back(std::move(a));
    }
    return SQUISH_OK;
}

static void OpenBrowser(const std::wstring &archivePath, int threads) {
    Browser *b     = new Browser();
    b->archivePath = archivePath;
    b->arc         = nullptr;
    b->sfx         = false;
    b->single      = false;
    b->threads     = threads;
    b->cwd         = L"";
    b->sortCol     = 0;
    b->sortAsc     = true;
    b->hwnd        = nullptr;
    b->hlist       = nullptr;

    /* Decide what we're looking at: an SFX payload region, or a plain file;
     * within that, a seekable archive or a single §1 stream. */
    SfxInfo si;
    uint64_t payloadOff = 0;
    b->sfx = ProbeSfx(archivePath, &si);
    if (b->sfx) payloadOff = si.payloadOff;
    char kind = ProbePayloadKind(archivePath, payloadOff);
    if (kind == 0) {
        MessageBoxW(g_hwnd,
            L"This file is not a SQUISH stream, archive, or self-extracting "
            L"archive, so its contents cannot be listed.",
            APP_NAME, MB_ICONERROR);
        delete b;
        return;
    }

    /* For an SFX we must hold the payload bytes: open_memory borrows them, and
     * a single-file payload is decompressed from them on extraction. */
    if (b->sfx) {
        if (!ReadRange(archivePath, si.payloadOff, si.payloadLen, &b->payload)) {
            MessageBoxW(g_hwnd, L"Could not read the archive payload.",
                        APP_NAME, MB_ICONERROR);
            delete b;
            return;
        }
    }

    int rc = SQUISH_OK;
    if (kind == 'a') {
        rc = b->sfx
           ? squish_archive_open_memory(b->payload.data(), b->payload.size(), &b->arc)
           : squish_archive_open(ToUtf8(archivePath).c_str(), &b->arc);
        if (rc == SQUISH_OK) rc = LoadArchiveEntries(b);
        if (rc != SQUISH_OK) {
            if (b->arc) { squish_archive_close(b->arc); b->arc = nullptr; }
            MessageBoxW(g_hwnd,
                L"The archive could not be opened (corrupt index).",
                APP_NAME, MB_ICONERROR);
            delete b;
            return;
        }
    } else {
        /* A single-file archive: one entry, named after the stored (SFX) name
         * or the name a plain .sq would extract to. */
        b->single = true;
        std::wstring nm;
        if (b->sfx) {
            std::wstring stored;
            ReadSfxName(archivePath, si, &stored);
            nm = SafeStoredName(stored);
        } else {
            nm = Basename(OutputPath(archivePath, false));
        }
        if (nm.empty()) nm = L"file.out";
        uint64_t orig = 0;
        if (b->sfx) squish_decompressed_size(b->payload.data(), b->payload.size(), &orig);
        else        ReadSqHeader(archivePath, &orig);
        ArcEntry e = { nm, false, orig, 0 };
        b->entries.push_back(std::move(e));
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof wc };
        wc.lpfnWndProc   = BrowserProc;
        wc.hInstance     = GetModuleHandleW(nullptr);
        wc.hIcon         = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP));
        wc.hIconSm       = wc.hIcon;
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = BROWSER_CLASS;
        RegisterClassExW(&wc);
        registered = true;
    }

    std::wstring title = Basename(archivePath) + L" — WinSquish";
    RECT r = { 0, 0, S(560), S(420) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, BROWSER_CLASS, title.c_str(),
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        nullptr, nullptr, GetModuleHandleW(nullptr), b);
    if (!hwnd) {                       /* WM_CREATE never ran => b not adopted */
        if (b->arc) squish_archive_close(b->arc);
        delete b;
        return;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
}

static void CreateControls(HWND hwnd) {
    struct Ctl {
        const wchar_t *cls, *text; DWORD style; int x, y, w, h, id;
    };
    const DWORD ES = WS_CHILD | WS_VISIBLE;
    const Ctl ctls[] = {
        { L"STATIC",   L"File:", ES, 12, 15, 30, 20, 0 },
        { L"EDIT",     L"", ES | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
                                  48, 12, 340, 24, IDC_FILE_EDIT },
        { L"BUTTON",   L"Browse…", ES | WS_TABSTOP,
                                  396, 11, 80, 26, IDC_BROWSE_BTN },
        { L"STATIC",   L"Drop a file or folder here, or use File ▸ Open.", ES,
                                  12, 44, 464, 20, IDC_INFO_STATIC },
        { L"STATIC",   L"CPU cores:", ES | SS_CENTERIMAGE,
                                  12, 68, 66, 22, 0 },
        { L"COMBOBOX", L"", ES | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
                                  82, 66, 56, 200, IDC_CPU_COMBO },
        { L"STATIC",   L"(1 = smallest file, single block; more = faster)",
                       ES | SS_CENTERIMAGE,
                                  146, 68, 330, 22, 0 },
        { L"BUTTON",   L"Create self-extracting archive (.exe, runs on Windows)",
                       ES | WS_TABSTOP | BS_AUTOCHECKBOX,
                                  12, 90, 464, 20, IDC_SFX_CHECK },
        { L"BUTTON",   L"Compress", ES | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                  12, 118, 110, 30, IDC_COMPRESS_BTN },
        { L"BUTTON",   L"Extract", ES | WS_TABSTOP,
                                  130, 118, 110, 30, IDC_EXTRACT_BTN },
        { L"BUTTON",   L"View Files…", ES | WS_TABSTOP,
                                  248, 118, 110, 30, IDC_VIEW_BTN },
        { PROGRESS_CLASSW, L"", ES, 12, 158, 464, 18, IDC_PROGRESS },
        { L"STATIC",   L"Ready.", ES | SS_PATHELLIPSIS,
                                  12, 182, 464, 20, IDC_STATUS },
    };
    for (const Ctl &c : ctls) {
        HWND h = CreateWindowExW(0, c.cls, c.text, c.style,
                                 S(c.x), S(c.y), S(c.w), S(c.h),
                                 hwnd, (HMENU)(INT_PTR)c.id,
                                 GetModuleHandleW(nullptr), nullptr);
        SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    }
    SendDlgItemMessageW(hwnd, IDC_PROGRESS, PBM_SETRANGE32, 0, 100);

    /* Populate the CPU-cores combo with 1..ncpu and default to 4 (clamped to
     * the machine). We deliberately do NOT default to "all cores". */
    int ncpu = squish_threads();
    if (ncpu < 1) ncpu = 1;
    HWND combo = GetDlgItem(hwnd, IDC_CPU_COMBO);
    for (int i = 1; i <= ncpu; i++) {
        wchar_t s[16];
        swprintf(s, 16, L"%d", i);
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)s);
    }
    int def = ncpu < DEFAULT_THREADS ? ncpu : DEFAULT_THREADS;
    SendMessageW(combo, CB_SETCURSEL, def - 1, 0);
}

static HMENU BuildMenu(void) {
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_OPEN, L"&Open file…\tCtrl+O");
    AppendMenuW(file, MF_STRING, IDM_OPENFOLDER, L"Open &folder…");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");
    HMENU tools = CreatePopupMenu();
    AppendMenuW(tools, MF_STRING, IDM_REGISTER,
                L"&Install Explorer context menu");
    AppendMenuW(tools, MF_STRING, IDM_UNREGISTER,
                L"&Remove Explorer context menu");
    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About WinSquish");
    HMENU bar = CreateMenu();
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file,  L"&File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)tools, L"&Tools");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help,  L"&Help");
    return bar;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        CreateControls(hwnd);
        DragAcceptFiles(hwnd, TRUE);
        return 0;

    case WM_DROPFILES: {
        if (g_busy) break;
        wchar_t path[MAX_PATH];
        if (DragQueryFileW((HDROP)wp, 0, path, MAX_PATH)) {
            SetDlgItemTextW(hwnd, IDC_FILE_EDIT, path);
            UpdateFileInfo();
        }
        DragFinish((HDROP)wp);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSE_BTN:
        case IDM_OPEN:         BrowseFile();                       return 0;
        case IDM_OPENFOLDER:   BrowseFolder();                     return 0;
        case IDC_COMPRESS_BTN: StartJob(true);                     return 0;
        case IDC_EXTRACT_BTN:  StartJob(false);                    return 0;
        case IDC_VIEW_BTN:     StartView();                        return 0;
        case IDC_SFX_CHECK:
            if (HIWORD(wp) == BN_CLICKED) UpdateFileInfo();
            return 0;
        case IDC_FILE_EDIT:
            if (HIWORD(wp) == EN_KILLFOCUS) UpdateFileInfo();
            return 0;
        case IDM_REGISTER:
            /* The GUI registers per-user (HKCU): no elevation needed. */
            MessageBoxW(hwnd, RegisterShell(HKEY_CURRENT_USER)
                ? L"Context-menu entries installed for the current user.\n\n"
                  L"Right-click any file for \"Compress to .sq\" or\n"
                  L"\"Compress to self-extracting .exe\", and any .sq file\n"
                  L"for \"Extract with WinSquish\"."
                : L"Registration failed (registry access denied?).",
                APP_NAME, MB_ICONINFORMATION);
            return 0;
        case IDM_UNREGISTER:
            UnregisterShell(HKEY_CURRENT_USER);
            MessageBoxW(hwnd, L"Context-menu entries removed.",
                        APP_NAME, MB_ICONINFORMATION);
            return 0;
        case IDM_ABOUT:        ShowAbout();                        return 0;
        case IDM_EXIT:         DestroyWindow(hwnd);                return 0;
        }
        break;

    case WM_APP_PROGRESS:
        SendDlgItemMessageW(hwnd, IDC_PROGRESS, PBM_SETPOS, wp, 0);
        {
            const wchar_t *verb = L"Compressing…";
            if (g_job) {
                if (!g_job->compress)    verb = L"Extracting…";
                else if (g_job->sfx)     verb = L"Building…";
            }
            wchar_t s[64];
            swprintf(s, 64, L"%s %d%%", verb, (int)wp);
            SetStatus(s);
        }
        return 0;

    case WM_APP_DONE:
        OnJobDone((int)wp);
        return 0;

    case WM_CLOSE:
        if (g_busy &&
            MessageBoxW(hwnd, L"An operation is still running. Quit anyway?\n"
                              L"(The partial output file will be incomplete.)",
                        APP_NAME, MB_YESNO | MB_ICONWARNING) != IDYES)
            return 0;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* --- entry point -------------------------------------------------------------*/
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    /* parse command line */
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring file;
    int autoMode = 0;   /* 0=none 1=compress 2=decompress 3=compress-sfx */
    bool quiet = false; /* suppress register/unregister confirmation dialogs */
    bool allUsers = false;  /* register/unregister in HKLM (system-wide) */
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--quiet")         quiet = true;
        else if (a == L"--allusers") allUsers = true;
    }
    HKEY regRoot = allUsers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--register") {
            bool ok = RegisterShell(regRoot);
            if (!quiet)
                MessageBoxW(nullptr, ok
                    ? L"WinSquish context-menu entries installed."
                    : L"Registration failed.", APP_NAME,
                    ok ? MB_ICONINFORMATION : MB_ICONERROR);
            return ok ? 0 : 1;
        }
        if (a == L"--unregister") {
            UnregisterShell(regRoot);
            if (!quiet)
                MessageBoxW(nullptr, L"WinSquish context-menu entries removed.",
                            APP_NAME, MB_ICONINFORMATION);
            return 0;
        }
        if (a == L"--compress")            autoMode = 1;
        else if (a == L"--decompress")     autoMode = 2;
        else if (a == L"--compress-sfx")   autoMode = 3;
        else if (a == L"--view")           autoMode = 4;
        else if (a == L"--quiet")          ; /* handled in the pre-scan above */
        else if (a == L"--allusers")       ; /* handled in the pre-scan above */
        else if (file.empty())             file = a;
    }
    LocalFree(argv);

    /* If we were double-clicked as a self-extracting archive (our own image
     * carries an SFX trailer) with no other request, extract ourselves. */
    if (file.empty() && autoMode == 0) {
        SfxInfo selfInfo;
        if (ProbeSfx(ExePath(), &selfInfo)) { file = ExePath(); autoMode = 2; }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    INITCOMMONCONTROLSEX icc = { sizeof icc,
        ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    g_dpi = GetDpiForSystem();
    NONCLIENTMETRICSW ncm = { sizeof ncm };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm       = wc.hIcon;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = WND_CLASS;
    RegisterClassExW(&wc);

    RECT r = { 0, 0, S(488), S(214) };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                         WS_MINIMIZEBOX, TRUE);
    HWND hwnd = CreateWindowExW(0, WND_CLASS, APP_NAME,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        nullptr, BuildMenu(), hInst, nullptr);
    ShowWindow(hwnd, nCmdShow);

    if (!file.empty()) {
        if (autoMode == 3) CheckDlgButton(hwnd, IDC_SFX_CHECK, BST_CHECKED);
        SetDlgItemTextW(hwnd, IDC_FILE_EDIT, file.c_str());
        UpdateFileInfo();
        if (autoMode == 1 || autoMode == 3)
            PostMessageW(hwnd, WM_COMMAND, IDC_COMPRESS_BTN, 0);
        if (autoMode == 2)
            PostMessageW(hwnd, WM_COMMAND, IDC_EXTRACT_BTN, 0);
        if (autoMode == 4)
            PostMessageW(hwnd, WM_COMMAND, IDC_VIEW_BTN, 0);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DeleteObject(g_font);
    return (int)msg.wParam;
}
