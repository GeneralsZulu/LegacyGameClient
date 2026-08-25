// Zulu launcher: lightweight pre-game shim that checks for updates against
// a JSON manifest in our public GCS bucket and, if a newer release is
// available, downloads the matching installer to %TEMP% and hands off to
// it. On no-update / network failure / user decline it just launches the
// installed game with whatever args were forwarded by the shortcut.
//
// Why this exists:
//   * The game .exe lives in Program Files, so overwriting it needs admin.
//     Putting that admin prompt in the launcher means the game itself can
//     keep running unprivileged.
//   * The installed .exe carries its semver in a VS_VERSION_INFO resource
//     (see GeneralsMD/Code/Main/ZuluVersion.rc.in), so we don't need a
//     sidecar version file to know what's on disk.
//
// "Don't downgrade" rule:
//   We only update when latest > installed (component-wise major.minor.build).
//   That keeps dev builds (which can be ahead of any released installer) from
//   being rolled back to the latest released installer when they run the
//   launcher.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wininet.h>
#include <shellapi.h>
#include <commdlg.h>
#include <objbase.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "BuildVariant.h"

// VC6 SDK predates these definitions.
#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif

// VC6's wincrypt.h hides the legacy CryptoAPI types behind a _WIN32_WINNT
// guard that isn't tripped in this build, so we forward-declare just the
// types/constants we touch and resolve the entry points out of advapi32
// at runtime. Same pattern StatsUploader.cpp uses for GetAdaptersInfo.
// The "handle" types are ULONG_PTR-sized in modern SDKs but only 32-bit
// on 32-bit Windows (which is the only target VC6 can produce), so plain
// unsigned long is ABI-compatible and avoids needing basetsd.h.
typedef unsigned long LocalHCRYPTPROV;
typedef unsigned long LocalHCRYPTHASH;
typedef unsigned long LocalHCRYPTKEY;
typedef DWORD         LocalALG_ID;

#define LOCAL_CALG_SHA_256        0x0000800c
#define LOCAL_PROV_RSA_AES        24
#define LOCAL_CRYPT_VERIFYCONTEXT 0xF0000000
#define LOCAL_HP_HASHVAL          2

typedef BOOL (WINAPI *FnCryptAcquireContextA)(LocalHCRYPTPROV *, LPCSTR, LPCSTR, DWORD, DWORD);
typedef BOOL (WINAPI *FnCryptReleaseContext)(LocalHCRYPTPROV, DWORD);
typedef BOOL (WINAPI *FnCryptCreateHash)(LocalHCRYPTPROV, LocalALG_ID, LocalHCRYPTKEY, DWORD, LocalHCRYPTHASH *);
typedef BOOL (WINAPI *FnCryptHashData)(LocalHCRYPTHASH, const BYTE *, DWORD, DWORD);
typedef BOOL (WINAPI *FnCryptGetHashParam)(LocalHCRYPTHASH, DWORD, BYTE *, DWORD *, DWORD);
typedef BOOL (WINAPI *FnCryptDestroyHash)(LocalHCRYPTHASH);

// Manifest URL is variant-aware: dev launchers read latest-dev.json so dev
// installs don't get dragged onto the public release line and vice versa.
// Both manifests live under the same kAllowedURLPrefix, so the post-download
// URL pin still applies to both code paths.
static const char *kLatestJsonURLRelease =
    "https://storage.googleapis.com/zulu-installer/latest.json";
static const char *kLatestJsonURLDev =
    "https://storage.googleapis.com/zulu-installer/latest-dev.json";
// Defense-in-depth: refuse to download/run anything whose URL isn't a public
// object in our installer bucket. HTTPS to that bucket already authenticates
// the origin; this pin just prevents a tampered manifest from pointing the
// elevated installer hand-off at an arbitrary external host.
static const char *kAllowedURLPrefix =
    "https://storage.googleapis.com/zulu-installer/";
static const char *kGameExeName  = "generalszh_zulu.exe";
// Must match LAUNCHERNAME in installer/Zulu.nsi: the installer renames the
// running launcher to "<this>.old" so it can write the new one, and we sweep
// that leftover on the next start.
static const char *kLauncherExeName = "ZuluLauncher.exe";
static const char *kInstallerLeafRelease = "Zulu_Setup_update.exe";
static const char *kInstallerLeafDev     = "Zulu_Setup_Dev_update.exe";
static const char *kAppName       = "Zulu";

static bool isDevBuild() {
    return strcmp(ZULU_BUILD_VARIANT_KIND, "dev") == 0;
}

struct SemVer {
    unsigned major;
    unsigned minor;
    unsigned build;
};

static int semVerCompare(const SemVer &a, const SemVer &b) {
    if (a.major != b.major) return (a.major < b.major) ? -1 : 1;
    if (a.minor != b.minor) return (a.minor < b.minor) ? -1 : 1;
    if (a.build != b.build) return (a.build < b.build) ? -1 : 1;
    return 0;
}

static bool parseSemVer(const char *s, SemVer &out) {
    out.major = out.minor = out.build = 0;
    if (!s) return false;
    int n = sscanf(s, "%u.%u.%u", &out.major, &out.minor, &out.build);
    return n >= 1;
}

// Reads FILEVERSION (e.g. 1.4.601) from an exe's VS_FIXEDFILEINFO block.
// Stored as two DWORDs: MS = major<<16 | minor, LS = build<<16 | revision.
static bool getFileVersion(const char *path, SemVer &out) {
    // VC6's headers type these as LPSTR (non-const) even though the APIs
    // don't actually modify the path. Cast through to silence C2664.
    char *pathArg = const_cast<char *>(path);
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeA(pathArg, &handle);
    if (size == 0) return false;
    void *buf = malloc(size);
    if (!buf) return false;
    if (!GetFileVersionInfoA(pathArg, handle, size, buf)) { free(buf); return false; }
    VS_FIXEDFILEINFO *ffi = NULL;
    UINT len = 0;
    if (!VerQueryValueA(buf, "\\", (LPVOID *)&ffi, &len) || !ffi) {
        free(buf);
        return false;
    }
    out.major = HIWORD(ffi->dwFileVersionMS);
    out.minor = LOWORD(ffi->dwFileVersionMS);
    out.build = HIWORD(ffi->dwFileVersionLS);
    free(buf);
    return true;
}

// Purpose-built scanner for the tiny manifests we publish. Not a general
// JSON parser — assumes well-formed UTF-8 we produced ourselves.
static bool jsonGetString(const char *json, const char *key,
                          char *out, size_t outSize) {
    char pat[64];
    _snprintf(pat, sizeof(pat) - 1, "\"%s\"", key);
    pat[sizeof(pat) - 1] = 0;
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p && *p != ':') ++p;
    if (*p != ':') return false;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p != '"') return false;
    ++p;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outSize) {
        if (*p == '\\' && p[1]) ++p;
        out[i++] = *p++;
    }
    out[i] = 0;
    return *p == '"';
}

// SHA-256 of an entire file, written as 64 lowercase hex chars + nul into
// outHex. Returns false on any failure (file missing, advapi32 missing the
// SHA-256 algorithm, read error). PROV_RSA_AES is the smallest provider
// that exposes SHA-256; available on XP SP3+ and every supported Windows
// after.
static bool computeFileSha256Hex(const char *path, char *outHex, size_t outSize) {
    if (outSize < 65) return false;

    HMODULE advapi = LoadLibraryA("advapi32.dll");
    if (!advapi) return false;
    FnCryptAcquireContextA acquireCtx = (FnCryptAcquireContextA)GetProcAddress(advapi, "CryptAcquireContextA");
    FnCryptReleaseContext  releaseCtx = (FnCryptReleaseContext) GetProcAddress(advapi, "CryptReleaseContext");
    FnCryptCreateHash      createHash = (FnCryptCreateHash)     GetProcAddress(advapi, "CryptCreateHash");
    FnCryptHashData        hashData   = (FnCryptHashData)       GetProcAddress(advapi, "CryptHashData");
    FnCryptGetHashParam    getHashParam = (FnCryptGetHashParam) GetProcAddress(advapi, "CryptGetHashParam");
    FnCryptDestroyHash     destroyHash = (FnCryptDestroyHash)   GetProcAddress(advapi, "CryptDestroyHash");
    if (!acquireCtx || !releaseCtx || !createHash || !hashData || !getHashParam || !destroyHash) {
        FreeLibrary(advapi);
        return false;
    }

    HANDLE hF = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hF == INVALID_HANDLE_VALUE) { FreeLibrary(advapi); return false; }

    LocalHCRYPTPROV hProv = 0;
    LocalHCRYPTHASH hHash = 0;
    bool ok = false;
    if (acquireCtx(&hProv, NULL, NULL, LOCAL_PROV_RSA_AES, LOCAL_CRYPT_VERIFYCONTEXT)) {
        if (createHash(hProv, LOCAL_CALG_SHA_256, 0, 0, &hHash)) {
            BYTE buf[64 * 1024];
            DWORD got = 0;
            ok = true;
            while (ReadFile(hF, buf, sizeof(buf), &got, NULL) && got > 0) {
                if (!hashData(hHash, buf, got, 0)) { ok = false; break; }
            }
            if (ok) {
                BYTE digest[32];
                DWORD digestLen = sizeof(digest);
                if (getHashParam(hHash, LOCAL_HP_HASHVAL, digest, &digestLen, 0)
                        && digestLen == 32) {
                    static const char hex[] = "0123456789abcdef";
                    int i;
                    for (i = 0; i < 32; ++i) {
                        outHex[i * 2]     = hex[(digest[i] >> 4) & 0xF];
                        outHex[i * 2 + 1] = hex[digest[i] & 0xF];
                    }
                    outHex[64] = 0;
                } else {
                    ok = false;
                }
            }
            destroyHash(hHash);
        }
        releaseCtx(hProv, 0);
    }
    CloseHandle(hF);
    FreeLibrary(advapi);
    return ok;
}

static void applyTimeouts(HINTERNET hI, DWORD millis) {
    InternetSetOptionA(hI, INTERNET_OPTION_CONNECT_TIMEOUT, &millis, sizeof(millis));
    InternetSetOptionA(hI, INTERNET_OPTION_RECEIVE_TIMEOUT, &millis, sizeof(millis));
    InternetSetOptionA(hI, INTERNET_OPTION_SEND_TIMEOUT, &millis, sizeof(millis));
}

static DWORD urlFlags(const char *url) {
    DWORD f = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE
            | INTERNET_FLAG_PRAGMA_NOCACHE | INTERNET_FLAG_KEEP_CONNECTION;
    if (strncmp(url, "https://", 8) == 0) f |= INTERNET_FLAG_SECURE;
    return f;
}

// HTTPS GET into a heap-allocated nul-terminated buffer. Caller frees.
// Returns NULL on any failure (offline, 404, TLS error). Callers fall
// through and just launch the game; an update check should never block play.
static char *httpGet(const char *url, DWORD *outSize) {
    HINTERNET hI = InternetOpenA("ZuluLauncher",
        INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hI) return NULL;
    applyTimeouts(hI, 10000);

    HINTERNET hU = InternetOpenUrlA(hI, url, NULL, 0, urlFlags(url), 0);
    if (!hU) { InternetCloseHandle(hI); return NULL; }

    DWORD cap = 4096;
    DWORD len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { InternetCloseHandle(hU); InternetCloseHandle(hI); return NULL; }

    for (;;) {
        if (len + 4096 + 1 > cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                InternetCloseHandle(hU); InternetCloseHandle(hI);
                return NULL;
            }
            buf = nb;
        }
        DWORD got = 0;
        if (!InternetReadFile(hU, buf + len, 4096, &got)) {
            free(buf);
            InternetCloseHandle(hU); InternetCloseHandle(hI);
            return NULL;
        }
        if (got == 0) break;
        len += got;
    }
    buf[len] = 0;
    if (outSize) *outSize = len;
    InternetCloseHandle(hU);
    InternetCloseHandle(hI);
    return buf;
}

// Streams url -> filePath. Deletes the file on any failure so we don't
// leave a half-written .exe in %TEMP%.
static bool httpDownloadToFile(const char *url, const char *filePath) {
    HINTERNET hI = InternetOpenA("ZuluLauncher",
        INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hI) return false;
    applyTimeouts(hI, 30000);

    HINTERNET hU = InternetOpenUrlA(hI, url, NULL, 0, urlFlags(url), 0);
    if (!hU) { InternetCloseHandle(hI); return false; }

    HANDLE hF = CreateFileA(filePath, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hF == INVALID_HANDLE_VALUE) {
        InternetCloseHandle(hU); InternetCloseHandle(hI);
        return false;
    }

    char chunk[16 * 1024];
    for (;;) {
        DWORD got = 0;
        if (!InternetReadFile(hU, chunk, sizeof(chunk), &got)) {
            CloseHandle(hF); DeleteFileA(filePath);
            InternetCloseHandle(hU); InternetCloseHandle(hI);
            return false;
        }
        if (got == 0) break;
        DWORD written = 0;
        if (!WriteFile(hF, chunk, got, &written, NULL) || written != got) {
            CloseHandle(hF); DeleteFileA(filePath);
            InternetCloseHandle(hU); InternetCloseHandle(hI);
            return false;
        }
    }
    CloseHandle(hF);
    InternetCloseHandle(hU);
    InternetCloseHandle(hI);
    return true;
}

static void getInstallDir(char *out, DWORD outSize) {
    GetModuleFileNameA(NULL, out, outSize);
    char *lastSlash = strrchr(out, '\\');
    if (lastSlash) *lastSlash = 0;
}

// Returns the substring of GetCommandLine() AFTER the launcher's own exe.
// Shortcuts will pass "-mod Zulu.big" (or whatever the user adds); we
// forward that verbatim to the game.
static const char *extractArgsAfterExe(const char *cmdLine) {
    const char *p = cmdLine;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '"') {
        ++p;
        while (*p && *p != '"') ++p;
        if (*p == '"') ++p;
    } else {
        while (*p && *p != ' ' && *p != '\t') ++p;
    }
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

// Start the game. When waitHandle is non-NULL the caller gets the process
// handle and owns closing it; otherwise we fire and forget.
static bool launchGameEx(const char *gameExe, const char *extraArgs, HANDLE *waitHandle) {
    char cmd[4096];
    if (extraArgs && *extraArgs) {
        _snprintf(cmd, sizeof(cmd) - 1, "\"%s\" %s", gameExe, extraArgs);
    } else {
        _snprintf(cmd, sizeof(cmd) - 1, "\"%s\"", gameExe);
    }
    cmd[sizeof(cmd) - 1] = 0;

    char workDir[MAX_PATH];
    strncpy(workDir, gameExe, sizeof(workDir));
    workDir[sizeof(workDir) - 1] = 0;
    char *lastSlash = strrchr(workDir, '\\');
    if (lastSlash) *lastSlash = 0;

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL,
        workDir, &si, &pi);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    if (waitHandle) {
        *waitHandle = pi.hProcess;
    } else {
        CloseHandle(pi.hProcess);
    }
    return true;
}

static bool launchGame(const char *gameExe, const char *extraArgs) {
    return launchGameEx(gameExe, extraArgs, NULL);
}

// ---------------------------------------------------------------------------
// Replay Theater
//
// Watching a replay recorded by an older release means running with that
// release's data mounted, and the game picks its data once at startup, before
// any INI is parsed -- so it cannot be swapped from inside a running game.
// This mode does the swapping from outside: ask which replay, work out which
// archive it needs from ReplayData\versions.csv, and start the game with that
// archive as its -mod. See installer/replay_data_versions.csv.
//
// It deliberately parses the replay header and the version map itself rather
// than sharing the engine's code: the launcher links none of the engine, which
// is what keeps it small and quick to start.
// ---------------------------------------------------------------------------

static const char *kUserDataLeafDefault = "Command and Conquer Generals Zero Hour Data";
static const char *kZHRegistryPath =
    "SOFTWARE\\Electronic Arts\\EA Games\\Command and Conquer Generals Zero Hour";
static const char *kReplayDataFolder = "ReplayData";

static bool readRegistryString(HKEY root, const char *path, const char *key,
                               char *out, DWORD outSize) {
    HKEY handle;
    if (RegOpenKeyExA(root, path, 0, KEY_READ, &handle) != ERROR_SUCCESS) return false;
    DWORD type = 0;
    DWORD size = outSize;
    LONG rc = RegQueryValueExA(handle, key, NULL, &type, (LPBYTE)out, &size);
    RegCloseKey(handle);
    if (rc != ERROR_SUCCESS || type != REG_SZ) return false;
    out[outSize - 1] = 0;
    return true;
}

// Mirrors GlobalData::BuildUserDataPathFromRegistry so the launcher and the
// game agree on where the data lives, including OneDrive/Group Policy folder
// redirection (SHGetKnownFolderPath where available, as the game does).
// Returns a path with a trailing backslash.
static bool getUserDataDir(char *out, DWORD outSize) {
    typedef HRESULT (WINAPI *PFN_SHGetKnownFolderPath)(const GUID&, DWORD, HANDLE, PWSTR*);
    static const GUID kFolderIdDocuments =
        { 0xFDD39AD0, 0x238F, 0x46AF, { 0xAD, 0xB4, 0x6C, 0x85, 0x48, 0x03, 0x69, 0xC7 } };

    char documents[MAX_PATH];
    documents[0] = 0;

    HMODULE shell32 = GetModuleHandleA("shell32.dll");
    if (!shell32) shell32 = LoadLibraryA("shell32.dll");
    PFN_SHGetKnownFolderPath pSHGetKnownFolderPath = NULL;
    if (shell32) {
        pSHGetKnownFolderPath =
            (PFN_SHGetKnownFolderPath)GetProcAddress(shell32, "SHGetKnownFolderPath");
    }

    if (pSHGetKnownFolderPath) {
        PWSTR wide = NULL;
        if (SUCCEEDED(pSHGetKnownFolderPath(kFolderIdDocuments, 0, NULL, &wide)) && wide) {
            WideCharToMultiByte(CP_ACP, 0, wide, -1, documents, sizeof(documents), NULL, NULL);
            documents[sizeof(documents) - 1] = 0;
            CoTaskMemFree(wide);
        }
    }
    if (documents[0] == 0) {
        // Pre-Vista, and the same fallback the game uses.
        readRegistryString(HKEY_CURRENT_USER,
            "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders",
            "Personal", documents, sizeof(documents));
    }
    if (documents[0] == 0) return false;

    char leaf[MAX_PATH];
    if (!readRegistryString(HKEY_CURRENT_USER, kZHRegistryPath, "UserDataLeafName",
                            leaf, sizeof(leaf))
        && !readRegistryString(HKEY_LOCAL_MACHINE, kZHRegistryPath, "UserDataLeafName",
                               leaf, sizeof(leaf))) {
        strncpy(leaf, kUserDataLeafDefault, sizeof(leaf));
        leaf[sizeof(leaf) - 1] = 0;
    }

    const size_t len = strlen(documents);
    const char *sep = (len > 0 && documents[len - 1] == '\\') ? "" : "\\";
    _snprintf(out, outSize - 1, "%s%s%s\\", documents, sep, leaf);
    out[outSize - 1] = 0;
    return true;
}

// Read a NUL-terminated UTF-16LE string at *offset, narrowing to ASCII.
// Advances *offset past the terminator. 'out' may be NULL to just skip.
static bool readWideStringField(const unsigned char *buf, DWORD size, DWORD *offset,
                                char *out, DWORD outSize) {
    DWORD wrote = 0;
    for (;;) {
        if (*offset + 2 > size) return false;
        const unsigned short ch =
            (unsigned short)(buf[*offset] | (buf[*offset + 1] << 8));
        *offset += 2;
        if (ch == 0) break;
        if (out && wrote + 1 < outSize) out[wrote++] = (ch < 0x80) ? (char)ch : '?';
    }
    if (out && outSize > 0) out[wrote < outSize ? wrote : outSize - 1] = 0;
    return true;
}

// Pull the version string and iniCRC out of a .rep header. Layout mirrors
// RecorderClass::readReplayHeader: "GENREP", two int32 times, uint32 frame
// count, two bools, MAX_SLOTS(8) discon bools, the replay name, a 16-byte
// SYSTEMTIME, then the version string, version time string, version number,
// exe CRC and ini CRC.
static bool readReplayHeaderInfo(const char *path, char *versionOut, DWORD versionSize,
                                 DWORD *iniCrcOut) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return false;

    unsigned char buf[4096];
    DWORD size = 0;
    const BOOL read = ReadFile(file, buf, sizeof(buf), &size, NULL);
    CloseHandle(file);
    if (!read || size < 64) return false;

    if (memcmp(buf, "GENREP", 6) != 0) return false;

    DWORD offset = 6 + 4 + 4 + 4 + 1 + 1 + 8;   // times, frames, flags, discons
    if (!readWideStringField(buf, size, &offset, NULL, 0)) return false;   // replay name
    offset += 16;                                                          // SYSTEMTIME
    if (!readWideStringField(buf, size, &offset, versionOut, versionSize)) return false;
    if (!readWideStringField(buf, size, &offset, NULL, 0)) return false;   // version time

    if (offset + 12 > size) return false;
    offset += 4;   // version number
    offset += 4;   // exe CRC
    // Assemble through unsigned int: shifting an unsigned char left by 24
    // promotes to signed int first, so a CRC with the top bit set would come
    // out negative and sign-extend on the way into DWORD.
    *iniCrcOut = (DWORD)(((unsigned int)buf[offset])
        | ((unsigned int)buf[offset + 1] << 8)
        | ((unsigned int)buf[offset + 2] << 16)
        | ((unsigned int)buf[offset + 3] << 24));
    return true;
}

enum ReplayDataKind {
    kReplayDataNoMap,     // versions.csv itself is missing
    kReplayDataUnknown,   // map read fine, but no row matched this replay
    kReplayDataDefault,   // this release's own Zulu.big
    kReplayDataNone,      // retail base data; no -mod
    kReplayDataArchive    // mount the named archive
};

static void trimAscii(char *s) {
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') ++start;
    size_t len = strlen(start);
    while (len > 0) {
        const char c = start[len - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        --len;
    }
    memmove(s, start, len);
    s[len] = 0;
}

// Reader for ReplayData\versions.csv. Only the first three fields matter here;
// git_ref and note are build-time documentation.
// 'userDataDir' is the folder the installer puts ReplayData in, and ends with a
// backslash (getUserDataDir).
// csvPathOut receives the path consulted, so callers can name it when the map
// is missing -- "this version isn't listed" and "the map isn't installed" look
// identical to the user otherwise, and they have very different fixes.
static ReplayDataKind resolveReplayData(const char *userDataDir, const char *version,
                                        DWORD iniCrc, char *bigOut, DWORD bigSize,
                                        char *csvPathOut, DWORD csvPathSize) {
    char csvPath[MAX_PATH];
    _snprintf(csvPath, sizeof(csvPath) - 1, "%s%s\\versions.csv", userDataDir, kReplayDataFolder);
    csvPath[sizeof(csvPath) - 1] = 0;
    if (csvPathOut) {
        strncpy(csvPathOut, csvPath, csvPathSize);
        csvPathOut[csvPathSize - 1] = 0;
    }

    FILE *csv = fopen(csvPath, "rb");
    if (!csv) return kReplayDataNoMap;

    char crcMatch[128];
    crcMatch[0] = 0;
    char match[128];
    match[0] = 0;

    char line[1024];
    while (fgets(line, sizeof(line), csv)) {
        char kind[64], value[128], big[128];
        kind[0] = value[0] = big[0] = 0;

        // Split off the first three comma-separated fields.
        const char *p = line;
        int field = 0;
        const char *fieldStart = line;
        for (;; ++p) {
            if (*p != ',' && *p != 0 && *p != '\n') continue;
            char *dest = (field == 0) ? kind : (field == 1) ? value : (field == 2) ? big : NULL;
            if (dest) {
                const size_t cap = (field == 0) ? sizeof(kind) : sizeof(value);
                size_t len = (size_t)(p - fieldStart);
                if (len > cap - 1) len = cap - 1;
                memcpy(dest, fieldStart, len);
                dest[len] = 0;
                trimAscii(dest);
            }
            ++field;
            if (*p == 0 || *p == '\n' || field > 2) break;
            fieldStart = p + 1;
        }

        if (kind[0] == 0 || kind[0] == '#') continue;
        if (_stricmp(kind, "match_kind") == 0) continue;
        if (big[0] == 0) continue;

        if (_stricmp(kind, "version") == 0 && _stricmp(value, version) == 0) {
            strncpy(match, big, sizeof(match));
            match[sizeof(match) - 1] = 0;
            break;   // exact version match wins outright
        }
        if (_stricmp(kind, "inicrc") == 0 && crcMatch[0] == 0) {
            if ((DWORD)strtoul(value, NULL, 16) == iniCrc) {
                strncpy(crcMatch, big, sizeof(crcMatch));
                crcMatch[sizeof(crcMatch) - 1] = 0;
            }
        }
    }
    fclose(csv);

    // iniCRC is only the fallback: it identifies dev and prerelease builds,
    // which all report a non-semantic version like "Version 1.04".
    if (match[0] == 0) {
        if (crcMatch[0] == 0) return kReplayDataUnknown;
        strncpy(match, crcMatch, sizeof(match));
        match[sizeof(match) - 1] = 0;
    }

    if (_stricmp(match, "@default") == 0) return kReplayDataDefault;
    if (_stricmp(match, "@none") == 0) return kReplayDataNone;

    strncpy(bigOut, match, bigSize);
    bigOut[bigSize - 1] = 0;
    return kReplayDataArchive;
}

// Ask for a replay under the user's Replays folder. Returns false when the
// user cancels.
static bool pickReplay(const char *replaysDir, char *out, DWORD outSize) {
    char file[MAX_PATH];
    file[0] = 0;

    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = "Replays (*.rep)\0*.rep\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file);
    ofn.lpstrInitialDir = replaysDir;
    ofn.lpstrTitle = "Zulu Replay Theater - choose a replay";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (!GetOpenFileNameA(&ofn)) return false;

    strncpy(out, file, outSize);
    out[outSize - 1] = 0;
    return true;
}

static int runReplayTheater(const char *gameExe) {
    char userDataDir[MAX_PATH];
    if (!getUserDataDir(userDataDir, sizeof(userDataDir))) {
        MessageBoxA(NULL,
            "Could not locate your Zero Hour user data folder.",
            kAppName, MB_OK | MB_ICONERROR);
        return 1;
    }

    char replaysDir[MAX_PATH];
    _snprintf(replaysDir, sizeof(replaysDir) - 1, "%sReplays", userDataDir);
    replaysDir[sizeof(replaysDir) - 1] = 0;

    for (;;) {
        char replayPath[MAX_PATH];
        if (!pickReplay(replaysDir, replayPath, sizeof(replayPath))) {
            return 0;   // user closed the picker; done
        }

        // -watchReplay is resolved against the Replays folder, the same as the
        // in-game replay list, so anything outside it has no name we can pass.
        const size_t replaysLen = strlen(replaysDir);
        if (_strnicmp(replayPath, replaysDir, replaysLen) != 0
            || (replayPath[replaysLen] != '\\' && replayPath[replaysLen] != '/')) {
            MessageBoxA(NULL,
                "That replay is outside your Replays folder.\n\n"
                "Move or copy it into the Replays folder and try again.",
                kAppName, MB_OK | MB_ICONWARNING);
            continue;
        }
        const char *relativePath = replayPath + replaysLen + 1;

        char version[128];
        version[0] = 0;
        DWORD iniCrc = 0;
        if (!readReplayHeaderInfo(replayPath, version, sizeof(version), &iniCrc)) {
            MessageBoxA(NULL,
                "That file does not look like a Zero Hour replay.",
                kAppName, MB_OK | MB_ICONWARNING);
            continue;
        }

        char big[128];
        big[0] = 0;
        char csvPath[MAX_PATH];
        csvPath[0] = 0;
        const ReplayDataKind kind =
            resolveReplayData(userDataDir, version, iniCrc, big, sizeof(big),
                              csvPath, sizeof(csvPath));

        if (kind == kReplayDataNoMap) {
            char msg[MAX_PATH + 512];
            _snprintf(msg, sizeof(msg) - 1,
                "The replay data map is missing, so Replay Theater cannot tell which "
                "version's data this replay needs.\n\n"
                "Looked for:\n%s\n\n"
                "That folder ships with Zulu; reinstalling will restore it.",
                csvPath);
            msg[sizeof(msg) - 1] = 0;
            MessageBoxA(NULL, msg, kAppName, MB_OK | MB_ICONWARNING);
            continue;
        }

        if (kind == kReplayDataUnknown) {
            char msg[512];
            _snprintf(msg, sizeof(msg) - 1,
                "This replay reports version \"%s\", which is not in the replay data map.\n\n"
                "It will be played with the current version's data, so it may go out of sync.\n\n"
                "Watch it anyway?",
                version);
            msg[sizeof(msg) - 1] = 0;
            if (MessageBoxA(NULL, msg, kAppName,
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
                continue;
            }
        }

        char args[MAX_PATH * 3];
        if (kind == kReplayDataArchive) {
            // Absolute, and quoted: a relative -mod would be resolved against
            // the user data directory (parseMod), which happens to be right,
            // but only as long as the launcher and the game agree on where
            // that is. We already resolved it once; don't ask twice. Quoted
            // because Documents paths have spaces in them.
            _snprintf(args, sizeof(args) - 1,
                "-mod \"%s%s\\%s\" -watchReplay \"%s\" -replaytheater",
                userDataDir, kReplayDataFolder, big, relativePath);
        } else if (kind == kReplayDataNone) {
            // Retail data: no -mod at all, not even our own Zulu.big.
            _snprintf(args, sizeof(args) - 1,
                "-watchReplay \"%s\" -replaytheater", relativePath);
        } else {
            _snprintf(args, sizeof(args) - 1,
                "-mod Zulu.big -watchReplay \"%s\" -replaytheater", relativePath);
        }
        args[sizeof(args) - 1] = 0;

        HANDLE gameProcess = NULL;
        if (!launchGameEx(gameExe, args, &gameProcess)) {
            MessageBoxA(NULL,
                "Could not launch generalszh_zulu.exe.",
                kAppName, MB_OK | MB_ICONERROR);
            return 1;
        }

        // Wait it out, then offer the picker again. The game quits itself when
        // the replay ends (see RecorderClass::stopPlayback), so this is how the
        // theater loops without ever leaving a mismatched-data client sitting
        // on the main menu.
        WaitForSingleObject(gameProcess, INFINITE);
        CloseHandle(gameProcess);
    }
}

int APIENTRY WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    char installDir[MAX_PATH];
    getInstallDir(installDir, sizeof(installDir));

    char gameExe[MAX_PATH];
    _snprintf(gameExe, sizeof(gameExe) - 1, "%s\\%s", installDir, kGameExeName);
    gameExe[sizeof(gameExe) - 1] = 0;

    const char *fwdArgs = extractArgsAfterExe(GetCommandLineA());

    // Sweep the previous launcher an update moved aside. The installer renames
    // the running exe out of the way so the new one can be written (see
    // Zulu.nsi), and cannot delete it while that process still holds it -- by
    // now it does not, because that process is gone and this is its
    // replacement.
    //
    // Best-effort only, and it fails on the default install path: we run
    // unelevated and Program Files is not ours to delete from. It succeeds for
    // the many installs that live somewhere writable, and the installer clears
    // the leftover on its next run either way, so nothing accumulates beyond
    // one stale 45 KB file between updates.
    {
        char oldLauncher[MAX_PATH];
        _snprintf(oldLauncher, sizeof(oldLauncher) - 1, "%s\\%s.old",
            installDir, kLauncherExeName);
        oldLauncher[sizeof(oldLauncher) - 1] = 0;
        DeleteFileA(oldLauncher);
    }

    if (GetFileAttributesA(gameExe) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(NULL,
            "Could not find generalszh_zulu.exe next to the launcher.\n"
            "Please reinstall Zulu.",
            kAppName, MB_OK | MB_ICONERROR);
        return 1;
    }

    // Replay Theater shortcut. Skips the update check on purpose: someone who
    // opened this wants to watch a replay, not sit through an install, and an
    // update mid-session would swap the data out from under the loop.
    // Matched as a prefix because shortcuts routinely carry a trailing space.
    if (_strnicmp(fwdArgs, "-replaytheater", 14) == 0) {
        return runReplayTheater(gameExe);
    }

    const bool devBuild = isDevBuild();
    const char *manifestUrl = devBuild ? kLatestJsonURLDev : kLatestJsonURLRelease;
    const char *installerLeaf = devBuild ? kInstallerLeafDev : kInstallerLeafRelease;

    SemVer installed = {0, 0, 0};
    bool haveInstalled = getFileVersion(gameExe, installed);

    SemVer latest = {0, 0, 0};
    bool haveLatest = false;
    char latestUrl[2048]; latestUrl[0] = 0;
    char latestVersion[64]; latestVersion[0] = 0;
    // "exe_sha256" is the hash of the game exe *inside* the published
    // installer, which is what the dev gate can compare against what is on
    // disk. The manifest also carries "sha256", the hash of the installer
    // itself -- deliberately not used here: hashing the installed
    // generalszh_zulu.exe and comparing it to the hash of Zulu-Installer-Dev.exe
    // compares two different files, never matches, and made every dev launch
    // claim a new build was available.
    char latestExeSha[80]; latestExeSha[0] = 0;
    bool haveLatestExeSha = false;

    char *json = httpGet(manifestUrl, NULL);
    if (json) {
        if (jsonGetString(json, "version", latestVersion, sizeof(latestVersion)) &&
            jsonGetString(json, "url", latestUrl, sizeof(latestUrl))) {
            haveLatest = parseSemVer(latestVersion, latest);
        }
        haveLatestExeSha = jsonGetString(json, "exe_sha256", latestExeSha, sizeof(latestExeSha));
        free(json);
    }

    const bool urlOk = (latestUrl[0] != 0) &&
        (strncmp(latestUrl, kAllowedURLPrefix, strlen(kAllowedURLPrefix)) == 0);

    // Dev gate: compare the game exe we have against the game exe the latest
    // dev installer would lay down, since dev builds don't bump semver between
    // rebuilds. A manifest without "exe_sha256" (published before that field
    // existed) leaves the gate closed rather than prompting on every launch --
    // the next dev build republishes the manifest and it starts working.
    // Release gate: strict ">" semver comparison so we never downgrade.
    char installedSha[80]; installedSha[0] = 0;
    bool needUpdate = false;
    if (devBuild) {
        bool haveInstalledSha = computeFileSha256Hex(gameExe, installedSha, sizeof(installedSha));
        needUpdate = haveLatestExeSha && haveInstalledSha && urlOk &&
                     (_stricmp(latestExeSha, installedSha) != 0);
    } else {
        needUpdate = haveInstalled && haveLatest && urlOk &&
                     semVerCompare(latest, installed) > 0;
    }

    if (needUpdate) {
        char msg[512];
        if (devBuild) {
            _snprintf(msg, sizeof(msg) - 1,
                "A new Zulu dev build is available.\n\n"
                "Installed game exe SHA: %.12s...\n"
                "Latest game exe SHA:    %.12s...\n\n"
                "Download and install the update now?",
                installedSha, latestExeSha);
        } else {
            _snprintf(msg, sizeof(msg) - 1,
                "A newer Zulu release is available.\n\n"
                "Installed:  %u.%u.%u\n"
                "Latest:     %s\n\n"
                "Download and install the update now?",
                installed.major, installed.minor, installed.build,
                latestVersion);
        }
        msg[sizeof(msg) - 1] = 0;
        int rc = MessageBoxA(NULL, msg, kAppName,
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);
        if (rc == IDYES) {
            char tempDir[MAX_PATH];
            GetTempPathA(sizeof(tempDir), tempDir);
            char installerPath[MAX_PATH];
            _snprintf(installerPath, sizeof(installerPath) - 1, "%s%s",
                tempDir, installerLeaf);
            installerPath[sizeof(installerPath) - 1] = 0;

            HCURSOR oldCursor = SetCursor(LoadCursorA(NULL, IDC_WAIT));
            bool ok = httpDownloadToFile(latestUrl, installerPath);
            SetCursor(oldCursor);

            if (ok) {
                // /S = NSIS silent; /D=<dir> must be the LAST arg, unquoted,
                // and is honored even in silent mode. The installer's
                // manifest already requests admin, so this triggers UAC.
                char params[MAX_PATH + 64];
                _snprintf(params, sizeof(params) - 1, "/S /D=%s", installDir);
                params[sizeof(params) - 1] = 0;
                SHELLEXECUTEINFOA sei;
                ZeroMemory(&sei, sizeof(sei));
                sei.cbSize = sizeof(sei);
                sei.fMask = SEE_MASK_NOCLOSEPROCESS;
                sei.lpVerb = "open";
                sei.lpFile = installerPath;
                sei.lpParameters = params;
                sei.lpDirectory = tempDir;
                sei.nShow = SW_SHOWNORMAL;
                if (ShellExecuteExA(&sei) && sei.hProcess) {
                    // Wait for the elevated installer to finish so we
                    // can re-launch the (now-updated) game exe with
                    // the same args the user passed to *this* launcher
                    // invocation. The NSIS script's post-install Exec
                    // is intentionally removed: it only knew about the
                    // shortcut's hardcoded LAUNCHARGS and would drop
                    // anything extra (e.g. -zulu_debug) the user added
                    // on the command line.
                    //
                    // This process is holding ZuluLauncher.exe open the
                    // whole time, so the installer cannot write over it
                    // and does not try: it renames ours aside and writes
                    // the new one to the free name (see Zulu.nsi). Our
                    // in-memory image stays valid across that rename, and
                    // the .old file is swept at the next start.
                    SetCursor(LoadCursorA(NULL, IDC_WAIT));
                    WaitForSingleObject(sei.hProcess, INFINITE);
                    SetCursor(LoadCursorA(NULL, IDC_ARROW));
                    DWORD exitCode = 1;
                    GetExitCodeProcess(sei.hProcess, &exitCode);
                    CloseHandle(sei.hProcess);

                    if (exitCode == 0) {
                        // Install succeeded. Fall through to
                        // launchGame below; the path to the game exe
                        // is unchanged across the in-place upgrade so
                        // fwdArgs flows straight into the new binary.
                    } else {
                        MessageBoxA(NULL,
                            "Zulu installer reported a failure. The "
                            "game will be launched at the previously "
                            "installed version.",
                            kAppName, MB_OK | MB_ICONWARNING);
                    }
                } else {
                    MessageBoxA(NULL,
                        "Could not start the Zulu installer. The game will be "
                        "launched at the installed version.",
                        kAppName, MB_OK | MB_ICONWARNING);
                }
            } else {
                MessageBoxA(NULL,
                    "Update download failed. The game will be launched at "
                    "the installed version.",
                    kAppName, MB_OK | MB_ICONWARNING);
            }
            // Fall through and launch the existing installed version.
        }
    }

    if (!launchGame(gameExe, fwdArgs)) {
        MessageBoxA(NULL,
            "Could not launch generalszh_zulu.exe.",
            kAppName, MB_OK | MB_ICONERROR);
        return 1;
    }
    return 0;
}
