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
 * extract a .sq stream, with live progress. Installs optional per-user
 * Explorer context-menu entries:
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
#include <commctrl.h>
#include <commdlg.h>
#include <string>
#include <string.h>

#include "../squish/squish.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")

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

/* menu IDs */
#define IDM_OPEN         2001
#define IDM_EXIT         2002
#define IDM_REGISTER     2003
#define IDM_UNREGISTER   2004
#define IDM_ABOUT        2005

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

/* --- context-menu registration (HKCU\Software\Classes, no admin) -----------*/
static bool SetRegValue(const std::wstring &key, const wchar_t *name,
                        const std::wstring &val) {
    HKEY hk;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &hk, nullptr) != ERROR_SUCCESS)
        return false;
    LONG rc = RegSetValueExW(hk, name, 0, REG_SZ, (const BYTE *)val.c_str(),
                             (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
    return rc == ERROR_SUCCESS;
}

static bool RegisterShell(void) {
    std::wstring exe = ExePath();
    std::wstring quoted = L"\"" + exe + L"\"";
    bool ok = true;

    /* "Compress to .sq" on every file type */
    std::wstring k = L"Software\\Classes\\*\\shell\\WinSquish.Compress";
    ok &= SetRegValue(k, nullptr, L"Compress to .sq (WinSquish)");
    ok &= SetRegValue(k, L"Icon", quoted);
    ok &= SetRegValue(k + L"\\command", nullptr, quoted + L" --compress \"%1\"");

    /* "Compress to self-extracting .exe" on every file type */
    std::wstring ksfx = L"Software\\Classes\\*\\shell\\WinSquish.CompressSfx";
    ok &= SetRegValue(ksfx, nullptr,
                      L"Compress to self-extracting .exe (WinSquish)");
    ok &= SetRegValue(ksfx, L"Icon", quoted);
    ok &= SetRegValue(ksfx + L"\\command", nullptr,
                      quoted + L" --compress-sfx \"%1\"");

    /* .sq extension -> ProgID */
    ok &= SetRegValue(L"Software\\Classes\\" + std::wstring(SQ_EXT),
                      nullptr, PROGID);

    /* ProgID: icon, double-click opens GUI, "Extract" verb */
    std::wstring p = L"Software\\Classes\\" + std::wstring(PROGID);
    ok &= SetRegValue(p, nullptr, L"SQUISH Compressed File");
    ok &= SetRegValue(p + L"\\DefaultIcon", nullptr, quoted + L",0");
    ok &= SetRegValue(p + L"\\shell\\open\\command", nullptr,
                      quoted + L" \"%1\"");
    ok &= SetRegValue(p + L"\\shell\\extract", nullptr,
                      L"Extract with WinSquish");
    ok &= SetRegValue(p + L"\\shell\\extract", L"Icon", quoted);
    ok &= SetRegValue(p + L"\\shell\\extract\\command", nullptr,
                      quoted + L" --decompress \"%1\"");

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

static void UnregisterShell(void) {
    RegDeleteTreeW(HKEY_CURRENT_USER,
                   L"Software\\Classes\\*\\shell\\WinSquish.Compress");
    RegDeleteTreeW(HKEY_CURRENT_USER,
                   L"Software\\Classes\\*\\shell\\WinSquish.CompressSfx");
    RegDeleteTreeW(HKEY_CURRENT_USER,
                   (L"Software\\Classes\\" + std::wstring(PROGID)).c_str());
    /* remove the extension mapping only if it still points at us */
    HKEY hk;
    std::wstring extKey = L"Software\\Classes\\" + std::wstring(SQ_EXT);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, extKey.c_str(), 0,
                      KEY_QUERY_VALUE, &hk) == ERROR_SUCCESS) {
        wchar_t val[64] = L"";
        DWORD cb = sizeof val, type = 0;
        RegQueryValueExW(hk, nullptr, nullptr, &type, (BYTE *)val, &cb);
        RegCloseKey(hk);
        if (type == REG_SZ && _wcsicmp(val, PROGID) == 0)
            RegDeleteTreeW(HKEY_CURRENT_USER, extKey.c_str());
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

/* Build job->dst as a self-extracting .exe: compress the source to a scratch
 * SQUISH stream (progress is reported over this, the slow part), then splice
 * [stub][payload][name][trailer] into the output. */
static int SfxCompress(Job *job) {
    std::wstring tmp = MakeTempPath();
    if (tmp.empty()) return SQUISH_E_IO;
    std::string src = ToUtf8(job->src), tmpU = ToUtf8(tmp);
    int rc = job->threads > 1
           ? squish_compress_file_mt(src.c_str(), tmpU.c_str(), job->threads, 0,
                                     SquishProgress, job)
           : squish_compress_file2(src.c_str(), tmpU.c_str(),
                                   SquishProgress, job);
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

/* Extract the payload of an SFX (job->src, any platform's stub) to job->dst:
 * copy the payload region to a scratch stream, then decompress it. */
static int SfxExtract(Job *job) {
    SfxInfo info;
    if (!ProbeSfx(job->src, &info)) return SQUISH_E_FORMAT;
    std::wstring tmp = MakeTempPath();
    if (tmp.empty()) return SQUISH_E_IO;

    HANDLE in = OpenShared(job->src);
    if (in == INVALID_HANDLE_VALUE) { DeleteFileW(tmp.c_str()); return SQUISH_E_IO; }
    HANDLE out = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        CloseHandle(in); DeleteFileW(tmp.c_str()); return SQUISH_E_IO;
    }
    bool ok = CopyRange(in, out, info.payloadOff, info.payloadLen);
    CloseHandle(in); CloseHandle(out);
    if (!ok) { DeleteFileW(tmp.c_str()); return SQUISH_E_IO; }

    std::string tmpU = ToUtf8(tmp), dst = ToUtf8(job->dst);
    int rc = squish_decompress_file_mt(tmpU.c_str(), dst.c_str(), job->threads,
                                       SquishProgress, job);
    DeleteFileW(tmp.c_str());
    return rc;
}

static DWORD WINAPI WorkerProc(LPVOID param) {
    Job *job = (Job *)param;
    std::string src = ToUtf8(job->src), dst = ToUtf8(job->dst);
    int rc;
    if (job->compress) {
        rc = job->sfx
           ? SfxCompress(job)
           : job->threads > 1
              ? squish_compress_file_mt(src.c_str(), dst.c_str(), job->threads,
                                        0, SquishProgress, job)
              : squish_compress_file2(src.c_str(), dst.c_str(),
                                      SquishProgress, job);
    } else {
        rc = job->sfx
           ? SfxExtract(job)
           : squish_decompress_file_mt(src.c_str(), dst.c_str(), job->threads,
                                       SquishProgress, job);
    }
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

    long long n = FileSize(path);
    if (n < 0) {
        SetDlgItemTextW(g_hwnd, IDC_INFO_STATIC, L"File not found.");
        return;
    }
    std::wstring info = PrettySize(n);
    SfxInfo sfx;
    uint64_t orig;
    if (ProbeSfx(path, &sfx)) {
        std::wstring stored;
        ReadSfxName(path, sfx, &stored);
        info += L"  —  self-extracting SQUISH archive";
        if (!stored.empty())
            info += L", extracts to \"" + SafeStoredName(stored) + L"\"";
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
                    IDC_COMPRESS_BTN, IDC_EXTRACT_BTN })
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
        MessageBoxW(g_hwnd, L"Choose a file first.", APP_NAME,
                    MB_ICONINFORMATION);
        return;
    }
    if (FileSize(src) < 0) {
        MessageBoxW(g_hwnd, (L"File not found:\n" + src).c_str(), APP_NAME,
                    MB_ICONERROR);
        return;
    }

    /* Decide direction, whether SFX is involved, and the output path. */
    bool sfx;
    std::wstring dst;
    if (compress) {
        sfx = IsDlgButtonChecked(g_hwnd, IDC_SFX_CHECK) == BST_CHECKED;
        dst = sfx ? SfxOutputPath(src) : src + SQ_EXT;
    } else {
        SfxInfo si;
        uint64_t orig;
        if (ProbeSfx(src, &si)) {
            sfx = true;
            std::wstring stored;
            ReadSfxName(src, si, &stored);
            dst = DirWithSlash(src) + SafeStoredName(stored);
        } else if (ReadSqHeader(src, &orig)) {
            sfx = false;
            dst = OutputPath(src, false);
        } else {
            MessageBoxW(g_hwnd,
                L"This file is not a SQUISH stream or self-extracting archive "
                L"(bad or missing header).",
                APP_NAME, MB_ICONERROR);
            return;
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

    g_job = new Job{ g_hwnd, src, dst, compress, sfx, SelectedThreads(), -1 };
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
        wchar_t msg[512];
        if (job->compress && job->sfx) {
            swprintf(msg, 512,
                     L"Done in %.1f s — self-extracting archive %s → %s",
                     secs, PrettySize(in).c_str(), PrettySize(out).c_str());
        } else if (job->compress) {
            double ratio = (in > 0 && out > 0) ? 100.0 * out / in : 0;
            swprintf(msg, 512, L"Done in %.1f s — %s → %s (%.1f%% of original)",
                     secs, PrettySize(in).c_str(), PrettySize(out).c_str(),
                     ratio);
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
        { L"STATIC",   L"Drop a file here, or Browse.", ES,
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
    AppendMenuW(file, MF_STRING, IDM_OPEN, L"&Open…\tCtrl+O");
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
        case IDC_COMPRESS_BTN: StartJob(true);                     return 0;
        case IDC_EXTRACT_BTN:  StartJob(false);                    return 0;
        case IDC_SFX_CHECK:
            if (HIWORD(wp) == BN_CLICKED) UpdateFileInfo();
            return 0;
        case IDC_FILE_EDIT:
            if (HIWORD(wp) == EN_KILLFOCUS) UpdateFileInfo();
            return 0;
        case IDM_REGISTER:
            MessageBoxW(hwnd, RegisterShell()
                ? L"Context-menu entries installed for the current user.\n\n"
                  L"Right-click any file for \"Compress to .sq\" or\n"
                  L"\"Compress to self-extracting .exe\", and any .sq file\n"
                  L"for \"Extract with WinSquish\"."
                : L"Registration failed (registry access denied?).",
                APP_NAME, MB_ICONINFORMATION);
            return 0;
        case IDM_UNREGISTER:
            UnregisterShell();
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
    for (int i = 1; i < argc; i++)
        if (std::wstring(argv[i]) == L"--quiet") quiet = true;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--register") {
            bool ok = RegisterShell();
            if (!quiet)
                MessageBoxW(nullptr, ok
                    ? L"WinSquish context-menu entries installed."
                    : L"Registration failed.", APP_NAME,
                    ok ? MB_ICONINFORMATION : MB_ICONERROR);
            return ok ? 0 : 1;
        }
        if (a == L"--unregister") {
            UnregisterShell();
            if (!quiet)
                MessageBoxW(nullptr, L"WinSquish context-menu entries removed.",
                            APP_NAME, MB_ICONINFORMATION);
            return 0;
        }
        if (a == L"--compress")            autoMode = 1;
        else if (a == L"--decompress")     autoMode = 2;
        else if (a == L"--compress-sfx")   autoMode = 3;
        else if (a == L"--quiet")          ; /* handled in the pre-scan above */
        else if (file.empty())             file = a;
    }
    LocalFree(argv);

    /* If we were double-clicked as a self-extracting archive (our own image
     * carries an SFX trailer) with no other request, extract ourselves. */
    if (file.empty() && autoMode == 0) {
        SfxInfo selfInfo;
        if (ProbeSfx(ExePath(), &selfInfo)) { file = ExePath(); autoMode = 2; }
    }

    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS };
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
