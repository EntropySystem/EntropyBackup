// Entropy Backup — native Windows 11 project backup utility.
// Win32 + Direct2D + DirectWrite, frameless "Entropy Glass" UI, incremental hard-linked snapshots.
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <oleauto.h>
#include <bcrypt.h>
#include <taskschd.h>
#include <vss.h>
#include <vswriter.h>
#include <vsbackup.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <future>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using Microsoft::WRL::ComPtr;

static const wchar_t* APP_VERSION = L"1.9.0";
static const wchar_t* CLS = L"EntropyBackupGlassWindow";
static const float DW = 1480.f, DH = 950.f;   // design canvas, scaled uniformly to the window
static const float PI = 3.14159265f;
static const UINT WM_APP_JOBDONE = WM_APP + 1, WM_APP_FRAME = WM_APP + 2;
static const UINT_PTR T_ANIM = 1, T_HIST = 2, T_FX = 3;
static const COLORREF FIELD_GDI = RGB(0x17, 0x2B, 0x45);
static const UINT32 FIELD_HEX = 0x172B45;

// ---------------------------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------------------------
static std::string U8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring W8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::wstring Lower(std::wstring s) { if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size()); return s; }
static std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    return (a.back() == L'\\' || a.back() == L'/') ? a + b : a + L"\\" + b;
}
// Extended-length path so deep project trees (node_modules etc.) never hit MAX_PATH.
static std::wstring XP(const std::wstring& p) {
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}
static std::wstring NormPath(std::wstring p) {
    while (!p.empty() && (p.back() == L' ' || p.back() == L'"')) p.pop_back();
    while (!p.empty() && (p.front() == L' ' || p.front() == L'"')) p.erase(p.begin());
    if (p.empty()) return p;
    std::vector<wchar_t> buf(32768);
    DWORD n = GetFullPathNameW(p.c_str(), (DWORD)buf.size(), buf.data(), nullptr);
    if (n && n < buf.size()) p = buf.data();
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}
static bool IsUnder(const std::wstring& child, const std::wstring& parent) {
    std::wstring c = Lower(child), p = Lower(parent);
    if (c == p) return true;
    if (!p.empty() && p.back() != L'\\') p += L'\\';
    return c.rfind(p, 0) == 0;
}
static bool IsDir(const std::wstring& p) {
    DWORD a = GetFileAttributesW(XP(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool Exists(const std::wstring& p) { return GetFileAttributesW(XP(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
static bool MakeDirs(const std::wstring& p) {
    std::wstring lp = XP(p);
    size_t start = 7;  // past "\\?\C:\"
    if (lp.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        size_t s = lp.find(L'\\', 8);
        s = (s == std::wstring::npos) ? s : lp.find(L'\\', s + 1);
        if (s == std::wstring::npos) return IsDir(p);
        start = s + 1;
    }
    for (size_t i = lp.find(L'\\', start);; i = lp.find(L'\\', i + 1)) {
        CreateDirectoryW((i == std::wstring::npos ? lp : lp.substr(0, i)).c_str(), nullptr);
        if (i == std::wstring::npos) break;
    }
    return IsDir(p);
}
// Deletes one file without clearing its read-only flag for good. Snapshot files are hard links and every
// link shares one set of attributes, so clearing the flag would also strip it from older snapshots.
static bool DeleteOne(const std::wstring& p) {
    if (DeleteFileW(XP(p).c_str())) return true;
    HANDLE h = CreateFileW(XP(p).c_str(), DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD ex = 0x1 | 0x2 | 0x10;  // FileDispositionInfoEx: DELETE | POSIX_SEMANTICS | IGNORE_READONLY_ATTRIBUTE (Windows 10 1809+)
    BOOL ok = SetFileInformationByHandle(h, (FILE_INFO_BY_HANDLE_CLASS)21, &ex, sizeof(ex));
    FILE_BASIC_INFO bi{};
    if (!ok && GetFileInformationByHandleEx(h, FileBasicInfo, &bi, sizeof(bi))) {  // older Windows: clear, delete, put the flag back
        DWORD attrs = bi.FileAttributes;
        bi.FileAttributes = (attrs & ~FILE_ATTRIBUTE_READONLY) ? attrs & ~FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL;
        FILE_DISPOSITION_INFO di{TRUE};
        if (SetFileInformationByHandle(h, FileBasicInfo, &bi, sizeof(bi))) {
            ok = SetFileInformationByHandle(h, FileDispositionInfo, &di, sizeof(di));
            bi.FileAttributes = attrs;
            SetFileInformationByHandle(h, FileBasicInfo, &bi, sizeof(bi));
        }
    }
    CloseHandle(h);
    return ok != FALSE;
}
static void RemoveTree(const std::wstring& p) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(XP(Join(p, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            std::wstring c = Join(p, n);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryW(XP(c).c_str());
                else RemoveTree(c);
            } else DeleteOne(c);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    SetFileAttributesW(XP(p).c_str(), FILE_ATTRIBUTE_DIRECTORY);
    RemoveDirectoryW(XP(p).c_str());
}
static bool WriteAll(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(XP(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr);
    CloseHandle(h);
    return ok && w == data.size();
}
static bool ReadAll(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(XP(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD r = 0;
    BOOL ok = out.empty() || ReadFile(h, out.data(), (DWORD)out.size(), &r, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}
static std::wstring SysMsg(DWORD e) {
    wchar_t* b = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0, (LPWSTR)&b, 0, nullptr);
    std::wstring s = b ? b : L"Unknown error";
    if (b) LocalFree(b);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L'.')) s.pop_back();
    return s;
}
static std::wstring Num(uint64_t v) {
    std::wstring s = std::to_wstring(v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, L",");
    return s;
}
static std::wstring Bytes(uint64_t b) {
    const wchar_t* u[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double d = (double)b;
    int i = 0;
    while (d >= 1024 && i < 4) { d /= 1024; i++; }
    wchar_t o[32];
    swprintf_s(o, i ? L"%.1f %s" : L"%.0f %s", d, u[i]);
    return o;
}
static std::wstring Dur(double s) {
    int t = (int)(s + 0.5);
    wchar_t o[32];
    swprintf_s(o, L"%02d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    return o;
}
static std::wstring JsonEsc(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'\\' || c == L'"') { o += L'\\'; o += c; }
        else if (c == L'\n') o += L"\\n";
        else if (c == L'\r') o += L"\\r";
        else if (c == L'\t') o += L"\\t";
        else if (c < 0x20) { wchar_t b[8]; swprintf_s(b, L"\\u%04x", (unsigned)c); o += b; }
        else o += c;
    }
    return o;
}
static std::wstring JStr(const std::string& z, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = z.find(k);
    if (p == std::string::npos) return {};
    p = z.find(':', p + k.size());
    if (p == std::string::npos) return {};
    p = z.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos || z[p] != '"') return {};
    std::wstring out;
    std::string seg;
    for (++p; p < z.size(); ++p) {
        char c = z[p];
        if (c == '"') break;
        if (c == '\\' && p + 1 < z.size()) {
            char n = z[++p];
            if (n == 'u' && p + 4 < z.size()) {
                out += W8(seg); seg.clear();
                out += (wchar_t)strtoul(z.substr(p + 1, 4).c_str(), nullptr, 16);
                p += 4;
            } else seg += n == 'n' ? '\n' : n == 't' ? '\t' : n == 'r' ? '\r' : n;
        } else seg += c;
    }
    return out + W8(seg);
}
static double JNum(const std::string& z, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = z.find(k);
    if (p == std::string::npos) return 0;
    p = z.find(':', p + k.size());
    return p == std::string::npos ? 0 : strtod(z.c_str() + p + 1, nullptr);
}
// Case-insensitive wildcard match supporting * and ?.
static bool Wild(const wchar_t* p, const wchar_t* s) {
    const wchar_t *star = nullptr, *ss = nullptr;
    while (*s) {
        if (*p == L'?' || (*p != L'*' && towlower(*p) == towlower(*s))) { p++; s++; }
        else if (*p == L'*') { star = p++; ss = s; }
        else if (star) { p = star + 1; s = ++ss; }
        else return false;
    }
    while (*p == L'*') p++;
    return !*p;
}
static std::wstring IdFromTime(const SYSTEMTIME& t) {
    wchar_t b[64];
    swprintf_s(b, L"%04d-%02d-%02d_%02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}
static void FormatId(const std::wstring& id, std::wstring& date, std::wstring& time) {
    static const wchar_t* mon[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    int Y, M, D, h, m, s;
    if (swscanf_s(id.c_str(), L"%d-%d-%d_%d-%d-%d", &Y, &M, &D, &h, &m, &s) == 6 && M >= 1 && M <= 12) {
        wchar_t b[64];
        swprintf_s(b, L"%s %d, %d", mon[M - 1], D, Y);
        date = b;
        int hh = h % 12 ? h % 12 : 12;
        swprintf_s(b, L"%d:%02d %s", hh, m, h < 12 ? L"AM" : L"PM");
        time = b;
    } else { date = id; time.clear(); }
}

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------
static const wchar_t* DEFAULT_EXCLUDES = L".vs\r\nnode_modules\r\n*.tmp\r\n*.obj\r\n*.pdb\r\n*.ilk\r\nThumbs.db\r\ndesktop.ini";
struct Settings {
    std::wstring from, to, bgImage, excludes = DEFAULT_EXCLUDES;
    bool full = false, hardLinks = true, preserve = true, versioned = true, openAfter = false;
    bool remember = true, skipHidden = false, overwrite = false, banner = true;
    int tint = 55, frost = 100;  // glass look: tint strength and blur amount, 0..100
    // Two-colour gradient outline drawn around every section.
    bool outline = false, outSpan = false;
    UINT32 outA = 0xF2CF7A, outB = 0x5CCBFF;  // 0xRRGGBB
    int outStyle = 2, outWidth = 30, outGlow = 40;
    // Animated glow: calm effects layered on the outline (bit 0 pulse, 1 flow, 2 shimmer, 3 drift) and their speed.
    bool glowAnim = false;
    int glowFx = 3, glowSpeed = 30, flowDir = 0;  // flowDir: 0 clockwise, 1 counter-clockwise (when no job is running)
    // Automation: schedule (0 off, 1 hourly, 2 daily, 3 weekly; day 0 = Sunday), retention (0 = no limit) and integrity.
    int schedMode = 2, schedHour = 2, schedMin = 0, schedDay = 1, keepCount = 0, keepDays = 0;
    bool shadow = false, checksums = false, verifyAfter = false;
    // Firebox mode: every backup also goes to a second, independent destination (ideally off-site).
    bool fireOn = false;
    std::wstring fire;
};
static Settings cfg;
static std::wstring g_appDir, g_exeDir;

static std::wstring IniPath() { return Join(g_appDir, L"settings.ini"); }
static std::wstring IniGet(const wchar_t* k, const wchar_t* def) {
    std::vector<wchar_t> b(32768);
    GetPrivateProfileStringW(L"EntropyBackup", k, def, b.data(), (DWORD)b.size(), IniPath().c_str());
    return b.data();
}
static bool IniBool(const wchar_t* k, bool def) { return IniGet(k, def ? L"1" : L"0") == L"1"; }
static void IniSet(const wchar_t* k, const std::wstring& v) { WritePrivateProfileStringW(L"EntropyBackup", k, v.c_str(), IniPath().c_str()); }
static void LoadSettings() {
    cfg.remember = IniBool(L"RememberFolders", true);
    if (cfg.remember) { cfg.from = IniGet(L"From", L""); cfg.to = IniGet(L"To", L""); cfg.fire = IniGet(L"Firebox", L""); }
    cfg.fireOn = IniBool(L"FireboxMode", false);
    cfg.full = IniBool(L"FullCopy", false);
    cfg.hardLinks = IniBool(L"HardLinks", true);
    cfg.preserve = IniBool(L"PreserveMetadata", true);
    cfg.versioned = IniBool(L"Versioned", true);
    cfg.openAfter = IniBool(L"OpenAfter", false);
    cfg.skipHidden = IniBool(L"SkipHidden", false);
    cfg.overwrite = IniBool(L"RestoreOverwrite", false);
    cfg.banner = IniBool(L"HeaderBanner", true);
    cfg.tint = std::clamp((int)GetPrivateProfileIntW(L"EntropyBackup", L"GlassTint", 55, IniPath().c_str()), 0, 100);
    cfg.frost = std::clamp((int)GetPrivateProfileIntW(L"EntropyBackup", L"GlassFrost", 100, IniPath().c_str()), 0, 100);
    auto iniInt = [](const wchar_t* k, int def) { return (int)GetPrivateProfileIntW(L"EntropyBackup", k, def, IniPath().c_str()); };
    cfg.outline = IniBool(L"Outline", false);
    cfg.outSpan = IniBool(L"OutlineSpanWindow", false);
    cfg.outA = (UINT32)iniInt(L"OutlineColorA", 0xF2CF7A) & 0xFFFFFF;
    cfg.outB = (UINT32)iniInt(L"OutlineColorB", 0x5CCBFF) & 0xFFFFFF;
    cfg.outStyle = std::clamp(iniInt(L"OutlineStyle", 2), 0, 5);
    cfg.outWidth = std::clamp(iniInt(L"OutlineWidth", 30), 0, 100);
    cfg.outGlow = std::clamp(iniInt(L"OutlineGlow", 40), 0, 100);
    cfg.glowAnim = IniBool(L"GlowAnimate", false);
    cfg.glowFx = std::clamp(iniInt(L"GlowEffects", 3), 1, 15);
    cfg.glowSpeed = std::clamp(iniInt(L"GlowSpeed", 30), 0, 100);
    cfg.flowDir = std::clamp(iniInt(L"FlowDirection", 0), 0, 1);
    cfg.bgImage = IniGet(L"BackgroundImage", L"");
    cfg.schedMode = std::clamp(iniInt(L"ScheduleMode", 2), 0, 3);
    cfg.schedHour = std::clamp(iniInt(L"ScheduleHour", 2), 0, 23);
    cfg.schedMin = std::clamp(iniInt(L"ScheduleMinute", 0), 0, 59);
    cfg.schedDay = std::clamp(iniInt(L"ScheduleDay", 1), 0, 6);
    cfg.keepCount = std::clamp(iniInt(L"KeepCount", 0), 0, 99999);
    cfg.keepDays = std::clamp(iniInt(L"KeepDays", 0), 0, 99999);
    cfg.shadow = IniBool(L"ShadowCopy", false);
    cfg.checksums = IniBool(L"Checksums", false);
    cfg.verifyAfter = IniBool(L"VerifyAfterBackup", false);
    std::string ex;
    if (ReadAll(Join(g_appDir, L"excludes.txt"), ex)) cfg.excludes = W8(ex);
}
static void SaveSettings() {
    if (!Exists(IniPath())) {  // UTF-16 file so the profile API keeps non-ASCII paths intact
        HANDLE h = CreateFileW(IniPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) { DWORD w; WORD bom = 0xFEFF; WriteFile(h, &bom, 2, &w, nullptr); CloseHandle(h); }
    }
    IniSet(L"RememberFolders", cfg.remember ? L"1" : L"0");
    IniSet(L"From", cfg.remember ? cfg.from : L"");
    IniSet(L"To", cfg.remember ? cfg.to : L"");
    IniSet(L"Firebox", cfg.remember ? cfg.fire : L"");
    IniSet(L"FireboxMode", cfg.fireOn ? L"1" : L"0");
    IniSet(L"FullCopy", cfg.full ? L"1" : L"0");
    IniSet(L"HardLinks", cfg.hardLinks ? L"1" : L"0");
    IniSet(L"PreserveMetadata", cfg.preserve ? L"1" : L"0");
    IniSet(L"Versioned", cfg.versioned ? L"1" : L"0");
    IniSet(L"OpenAfter", cfg.openAfter ? L"1" : L"0");
    IniSet(L"SkipHidden", cfg.skipHidden ? L"1" : L"0");
    IniSet(L"RestoreOverwrite", cfg.overwrite ? L"1" : L"0");
    IniSet(L"HeaderBanner", cfg.banner ? L"1" : L"0");
    IniSet(L"GlassTint", std::to_wstring(cfg.tint));
    IniSet(L"GlassFrost", std::to_wstring(cfg.frost));
    IniSet(L"Outline", cfg.outline ? L"1" : L"0");
    IniSet(L"OutlineSpanWindow", cfg.outSpan ? L"1" : L"0");
    IniSet(L"OutlineColorA", std::to_wstring(cfg.outA));
    IniSet(L"OutlineColorB", std::to_wstring(cfg.outB));
    IniSet(L"OutlineStyle", std::to_wstring(cfg.outStyle));
    IniSet(L"OutlineWidth", std::to_wstring(cfg.outWidth));
    IniSet(L"OutlineGlow", std::to_wstring(cfg.outGlow));
    IniSet(L"GlowAnimate", cfg.glowAnim ? L"1" : L"0");
    IniSet(L"GlowEffects", std::to_wstring(cfg.glowFx));
    IniSet(L"GlowSpeed", std::to_wstring(cfg.glowSpeed));
    IniSet(L"FlowDirection", std::to_wstring(cfg.flowDir));
    IniSet(L"BackgroundImage", cfg.bgImage);
    IniSet(L"ScheduleMode", std::to_wstring(cfg.schedMode));
    IniSet(L"ScheduleHour", std::to_wstring(cfg.schedHour));
    IniSet(L"ScheduleMinute", std::to_wstring(cfg.schedMin));
    IniSet(L"ScheduleDay", std::to_wstring(cfg.schedDay));
    IniSet(L"KeepCount", std::to_wstring(cfg.keepCount));
    IniSet(L"KeepDays", std::to_wstring(cfg.keepDays));
    IniSet(L"ShadowCopy", cfg.shadow ? L"1" : L"0");
    IniSet(L"Checksums", cfg.checksums ? L"1" : L"0");
    IniSet(L"VerifyAfterBackup", cfg.verifyAfter ? L"1" : L"0");
    WriteAll(Join(g_appDir, L"excludes.txt"), U8(cfg.excludes));
}
static std::vector<std::wstring> Patterns(const std::wstring& text) {
    std::vector<std::wstring> v;
    size_t a = 0;
    while (a <= text.size()) {
        size_t b = text.find(L'\n', a);
        std::wstring l = text.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a);
        while (!l.empty() && (l.back() == L'\r' || l.back() == L' ' || l.back() == L'\t')) l.pop_back();
        while (!l.empty() && (l.front() == L' ' || l.front() == L'\t')) l.erase(l.begin());
        std::replace(l.begin(), l.end(), L'/', L'\\');
        while (!l.empty() && l.back() == L'\\') l.pop_back();
        if (!l.empty() && l[0] != L'#') v.push_back(l);
        if (b == std::wstring::npos) break;
        a = b + 1;
    }
    return v;
}

// ---------------------------------------------------------------------------------------------
// Runtime log
// ---------------------------------------------------------------------------------------------
static void LogRuntime(const wchar_t* stage, HRESULT hr = S_OK) {
    if (g_appDir.empty()) return;
    wchar_t b[512];
    SYSTEMTIME t; GetLocalTime(&t);
    swprintf_s(b, L"%04d-%02d-%02d %02d:%02d:%02d | %s | HRESULT=0x%08lX\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, stage, (unsigned long)hr);
    HANDLE h = CreateFileW(Join(g_appDir, L"runtime.log").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) { std::string s = U8(b); DWORD w; WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr); CloseHandle(h); }
}
// backup.log: one line per finished job, so scheduled and scripted runs leave a trail. --log redirects it.
static std::wstring g_logPath;
static void LogEvent(const std::wstring& line) {
    std::wstring path = !g_logPath.empty() ? g_logPath : g_appDir.empty() ? L"" : Join(g_appDir, L"backup.log");
    if (path.empty()) return;
    wchar_t b[32];
    SYSTEMTIME t; GetLocalTime(&t);
    swprintf_s(b, L"%04d-%02d-%02d %02d:%02d:%02d  ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    HANDLE h = CreateFileW(XP(path).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) { std::string s = U8(b + line + L"\r\n"); DWORD w; WriteFile(h, s.data(), (DWORD)s.size(), &w, nullptr); CloseHandle(h); }
}

// ---------------------------------------------------------------------------------------------
// Snapshot history
// ---------------------------------------------------------------------------------------------
struct Snap {
    std::wstring dir, id, date, time, desc, source, type, created;
    uint64_t files = 0, copied = 0, reused = 0, bytes = 0, bytesTotal = 0, failed = 0;
    double seconds = 0;
    bool mirror = false, shadow = false, sums = false, verified = false, firebox = false;
    uint64_t vChecked = 0, vBad = 0;  // last verification: files checked, mismatched or missing
    std::wstring vWhen;
};
static std::vector<Snap> snaps;
static int selSnap = 0, cardScroll = 0, listScroll = 0;
static bool viewFire = false;  // Version History shows the Firebox instead of the primary backup folder

static bool FireboxSet() { return cfg.fireOn && !cfg.fire.empty(); }
static std::wstring ViewDest() { return viewFire && FireboxSet() ? cfg.fire : cfg.to; }
static std::wstring VersionsRoot() { std::wstring d = ViewDest(); return d.empty() ? L"" : Join(d, L"EntropyBackup_Versions"); }
static void RefreshHistory() {
    std::wstring keep = (selSnap >= 0 && selSnap < (int)snaps.size()) ? snaps[selSnap].dir : L"";
    snaps.clear();
    std::wstring root = VersionsRoot();
    if (root.empty() || !IsDir(root)) { selSnap = 0; return; }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(XP(Join(root, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring n = fd.cFileName;
        bool isSnap = n.rfind(L"snapshot_", 0) == 0, isMirror = n == L"mirror_latest";
        if (!isSnap && !isMirror) continue;
        Snap s;
        s.dir = Join(root, n);
        s.mirror = isMirror;
        std::string z;
        ReadAll(Join(s.dir, L"backup.json"), z);
        s.desc = JStr(z, "description");
        s.source = JStr(z, "source");
        s.type = JStr(z, "type");
        s.created = JStr(z, "created");
        s.files = (uint64_t)JNum(z, "files");
        s.copied = (uint64_t)JNum(z, "copied");
        s.reused = (uint64_t)JNum(z, "reused");
        s.bytes = (uint64_t)JNum(z, "bytes_written");
        s.bytesTotal = (uint64_t)JNum(z, "bytes_total");
        s.failed = (uint64_t)JNum(z, "failed");
        s.seconds = JNum(z, "duration_seconds");
        s.shadow = z.find("\"shadow_copy\": true") != std::string::npos;
        s.firebox = z.find("\"firebox\": true") != std::string::npos;
        s.sums = z.find("\"checksums\": true") != std::string::npos;
        std::string v;
        if (ReadAll(Join(s.dir, L"verify.json"), v)) {
            s.verified = true;
            s.vChecked = (uint64_t)JNum(v, "checked");
            s.vBad = (uint64_t)(JNum(v, "mismatched") + JNum(v, "missing"));
            std::wstring d, t;
            std::wstring iso = JStr(v, "verified_iso");
            for (auto& c : iso) { if (c == L'T') c = L'_'; else if (c == L':') c = L'-'; }
            FormatId(iso, d, t);
            s.vWhen = d + L" " + t;
        }
        if (isSnap) s.id = n.substr(9);
        else {
            std::wstring iso = JStr(z, "created_iso");
            for (auto& c : iso) { if (c == L'T') c = L'_'; else if (c == L':') c = L'-'; }
            s.id = iso.empty() ? L"0000-00-00_00-00-00" : iso;
        }
        FormatId(s.id, s.date, s.time);
        snaps.push_back(s);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(snaps.begin(), snaps.end(), [](const Snap& a, const Snap& b) { return a.id > b.id; });
    selSnap = 0;
    for (int i = 0; i < (int)snaps.size(); i++) if (snaps[i].dir == keep) selSnap = i;
}

// ---------------------------------------------------------------------------------------------
// Backup / restore engine (runs on a worker thread)
// ---------------------------------------------------------------------------------------------
static const wchar_t* IDLE_MESSAGE = L"Choose a project and a backup destination.";
struct JobState {
    std::atomic<int> kind{0};   // 0 backup, 1 restore
    std::atomic<int> phase{0};  // 0 idle, 1 scanning, 2 copying, 3 finalizing, 4 done, 5 failed, 6 cancelled
    std::atomic<uint64_t> scanned{0}, copied{0}, linked{0}, skipped{0}, bytesWritten{0}, workDone{0}, workTotal{0};
    std::atomic<uint64_t> warnings{0};  // skipped files, checksum problems, shadow copy fallback, a destination not saved
    std::atomic<int> legs{1}, leg{0}, failedLegs{0};  // backup destinations (2 in Firebox mode), the one being written, those not saved
    std::atomic<bool> cancel{false};
    std::mutex m;
    std::chrono::steady_clock::time_point t0, t1;
    std::wstring current, message = IDLE_MESSAGE, resultDir, title;
};
static JobState job;
static std::thread worker;
static HWND g_hwnd{};
static bool Running() { int p = job.phase; return p >= 1 && p <= 3; }
static void SetCurrent(const std::wstring& s) { std::lock_guard l(job.m); job.current = s; }

struct FileItem { std::wstring rel; uint64_t size, mtime, ctime; bool link = false, failed = false; std::string sha; };
struct DirItem { std::wstring rel; FILETIME c, a, w; };
struct ScanOpts { std::vector<std::wstring> patterns; bool skipHidden = false; std::vector<std::wstring> skipDirs; };
struct Fail { std::wstring msg; };
struct Cancelled {};
static const uint64_t FILE_COST = 16384;  // per-file overhead used to weight progress

static bool Excluded(const std::wstring& name, const std::wstring& rel, const ScanOpts& o) {
    for (auto& p : o.patterns) {
        if (p.find(L'\\') != std::wstring::npos) { if (Wild(p.c_str(), rel.c_str())) return true; }
        else if (Wild(p.c_str(), name.c_str())) return true;
    }
    return false;
}
static void Scan(const std::wstring& root, const std::wstring& rel, const ScanOpts& o, std::vector<FileItem>& files, std::vector<DirItem>& dirs) {
    std::wstring dir = rel.empty() ? root : Join(root, rel);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(XP(Join(dir, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> subs;
    do {
        if (job.cancel) break;
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        std::wstring r = rel.empty() ? n : rel + L"\\" + n;
        if (o.skipHidden && (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
        if (Excluded(n, r, o)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // junctions/symlinked dirs: avoid loops
            if (!o.skipDirs.empty() && std::find(o.skipDirs.begin(), o.skipDirs.end(), Lower(Join(root, r))) != o.skipDirs.end()) continue;
            dirs.push_back({r, fd.ftCreationTime, fd.ftLastAccessTime, fd.ftLastWriteTime});
            subs.push_back(r);
        } else {
            FileItem f{r, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow,
                       ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime,
                       ((uint64_t)fd.ftCreationTime.dwHighDateTime << 32) | fd.ftCreationTime.dwLowDateTime};
            files.push_back(f);
            if ((++job.scanned & 127) == 0) SetCurrent(r);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (auto& s : subs) { if (job.cancel) return; Scan(root, s, o, files, dirs); }
}
static FILETIME FT(uint64_t v) { return FILETIME{(DWORD)(v & 0xFFFFFFFF), (DWORD)(v >> 32)}; }
static void SetTimes(const std::wstring& p, const FILETIME* c, const FILETIME* a, const FILETIME* w, bool dir) {
    HANDLE h = CreateFileW(XP(p).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, dir ? FILE_FLAG_BACKUP_SEMANTICS : 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFileTime(h, c, a, w);
    CloseHandle(h);
}
struct CopyCtx { uint64_t base; };
static DWORD CALLBACK CopyProgress(LARGE_INTEGER, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID ctx) {
    job.workDone = ((CopyCtx*)ctx)->base + (uint64_t)done.QuadPart;
    return job.cancel ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
}
// manifest.tsv lines are size<TAB>mtime<TAB>path (v1) or size<TAB>mtime<TAB>sha256<TAB>path (v2, "-" = no checksum).
struct ManEntry { std::wstring rel; uint64_t size = 0, mtime = 0; std::string sha; };
static bool LoadManifest(const std::wstring& path, std::vector<ManEntry>& out) {
    std::string z;
    if (!ReadAll(path, z)) return false;
    size_t a = 0;
    while (a < z.size()) {
        size_t b = z.find('\n', a);
        if (b == std::string::npos) b = z.size();
        if (z[a] != '#') {
            std::string line = z.substr(a, b - a);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<std::string> f;  // names cannot contain tabs, so the last field is always the path
            for (size_t p = 0;;) {
                size_t t = f.size() < 3 ? line.find('\t', p) : std::string::npos;
                if (t == std::string::npos) { f.push_back(line.substr(p)); break; }
                f.push_back(line.substr(p, t - p));
                p = t + 1;
            }
            if (f.size() >= 3) {
                ManEntry e;
                e.size = _strtoui64(f[0].c_str(), nullptr, 10);
                e.mtime = _strtoui64(f[1].c_str(), nullptr, 10);
                if (f.size() == 4 && f[2] != "-") e.sha = f[2];
                e.rel = W8(f.back());
                if (!e.rel.empty()) out.push_back(std::move(e));
            }
        }
        a = b + 1;
    }
    return true;
}
static std::wstring FindPrevious(const std::wstring& root, bool versioned) {
    std::wstring best;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(XP(Join(root, L"snapshot_*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        do { if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && std::wstring(fd.cFileName) > best) best = fd.cFileName; } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::wstring snap = best.empty() ? L"" : Join(root, best), mir = Join(root, L"mirror_latest");
    bool hasMirror = IsDir(mir);
    if (!versioned) return hasMirror ? mir : snap;
    return !snap.empty() ? snap : (hasMirror ? mir : L"");
}

// ---------------------------------------------------------------------------------------------
// SHA-256 checksums (Windows CNG)
// ---------------------------------------------------------------------------------------------
class Hasher {
    BCRYPT_ALG_HANDLE alg{};
    BYTE* buf{};
    static const DWORD BUF = 1 << 20;  // page-aligned by VirtualAlloc, so unbuffered reads work
public:
    Hasher() {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) alg = nullptr;
        buf = (BYTE*)VirtualAlloc(nullptr, BUF, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    ~Hasher() {
        if (buf) VirtualFree(buf, 0, MEM_RELEASE);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    Hasher(const Hasher&) = delete;
    Hasher& operator=(const Hasher&) = delete;
    // Hex SHA-256 of a file, or "" if it cannot be read. `fromDisk` bypasses the file cache so a verification
    // reads what is really stored. Progress is reported as job.workDone = base + bytes read.
    std::string File(const std::wstring& path, bool fromDisk, uint64_t base) {
        if (!alg || !buf) return {};
        DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, fl = FILE_FLAG_SEQUENTIAL_SCAN;
        HANDLE f = CreateFileW(XP(path).c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, fl | (fromDisk ? FILE_FLAG_NO_BUFFERING : 0), nullptr);
        if (f == INVALID_HANDLE_VALUE && fromDisk) f = CreateFileW(XP(path).c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, fl, nullptr);
        if (f == INVALID_HANDLE_VALUE) return {};
        BCRYPT_HASH_HANDLE h{};
        UCHAR d[32];
        bool ok = BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0));
        DWORD r = 0;
        while (ok && !job.cancel && (ok = ReadFile(f, buf, BUF, &r, nullptr) != FALSE) && r) {
            ok = BCRYPT_SUCCESS(BCryptHashData(h, buf, r, 0));
            base += r;
            job.workDone = base;
        }
        ok = ok && !job.cancel && BCRYPT_SUCCESS(BCryptFinishHash(h, d, sizeof(d), 0));
        if (h) BCryptDestroyHash(h);
        CloseHandle(f);
        if (!ok) return {};
        static const char* hx = "0123456789abcdef";
        std::string s;
        for (UCHAR c : d) { s += hx[c >> 4]; s += hx[c & 15]; }
        return s;
    }
};

// ---------------------------------------------------------------------------------------------
// Volume Shadow Copy: a frozen, point-in-time view of the source volume, so open and locked files copy too
// ---------------------------------------------------------------------------------------------
static HRESULT VssWait(HRESULT hr, IVssAsync** pa) {
    IVssAsync* a = *pa;
    if (SUCCEEDED(hr) && a) {
        hr = a->Wait();
        HRESULT st = S_OK;
        if (SUCCEEDED(hr) && SUCCEEDED(a->QueryStatus(&st, nullptr)) && st != VSS_S_ASYNC_FINISHED) hr = FAILED(st) ? st : E_FAIL;
    }
    if (a) a->Release();
    *pa = nullptr;
    return hr;
}
class Shadow {
    ComPtr<IVssBackupComponents> bc;
    std::wstring device, mount;  // \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN and the volume root it mirrors (C:\)
public:
    // Shadow-copies the volume holding `path`. Returns "" on success, otherwise why not (the backup then reads live files).
    std::wstring Create(const std::wstring& path) {
        if (path.rfind(L"\\\\", 0) == 0) return L"network folders cannot be shadow-copied";
        wchar_t mp[MAX_PATH + 1] = {}, vol[64] = {};
        if (!GetVolumePathNameW(path.c_str(), mp, MAX_PATH) || !GetVolumeNameForVolumeMountPointW(mp, vol, 64)) return SysMsg(GetLastError());
        using CreateFn = HRESULT(STDAPICALLTYPE*)(IVssBackupComponents**);
        HMODULE lib = LoadLibraryW(L"vssapi.dll");
        auto create = lib ? (CreateFn)(void*)GetProcAddress(lib, "CreateVssBackupComponentsInternal") : nullptr;
        if (!create) return L"the Volume Shadow Copy service is not available";
        IVssAsync* a = nullptr;
        VSS_ID set{}, id{};
        HRESULT hr = create(&bc);
        if (SUCCEEDED(hr)) hr = bc->InitializeForBackup(nullptr);
        if (SUCCEEDED(hr)) hr = bc->SetContext(VSS_CTX_BACKUP);
        if (SUCCEEDED(hr)) hr = bc->SetBackupState(false, false, VSS_BT_COPY, false);  // copy backup: other backup tools' history is untouched
        if (SUCCEEDED(hr)) hr = VssWait(bc->GatherWriterMetadata(&a), &a);
        if (SUCCEEDED(hr)) hr = bc->StartSnapshotSet(&set);
        if (SUCCEEDED(hr)) hr = bc->AddToSnapshotSet(vol, GUID_NULL, &id);
        if (SUCCEEDED(hr)) hr = VssWait(bc->PrepareForBackup(&a), &a);
        if (SUCCEEDED(hr)) hr = VssWait(bc->DoSnapshotSet(&a), &a);
        VSS_SNAPSHOT_PROP p{};
        if (SUCCEEDED(hr)) hr = bc->GetSnapshotProperties(id, &p);
        if (FAILED(hr)) {
            if (bc) bc->AbortBackup();
            bc.Reset();
            if (hr == E_ACCESSDENIED) return L"administrator rights are required";
            wchar_t b[48];
            swprintf_s(b, L"VSS error 0x%08lX", (unsigned long)hr);
            return b;
        }
        device = p.m_pwszSnapshotDeviceObject ? p.m_pwszSnapshotDeviceObject : L"";
        for (VSS_PWSZ s : {p.m_pwszSnapshotDeviceObject, p.m_pwszOriginalVolumeName, p.m_pwszOriginatingMachine, p.m_pwszServiceMachine, p.m_pwszExposedName, p.m_pwszExposedPath})
            CoTaskMemFree(s);
        mount = mp;
        return {};
    }
    // The same path inside the shadow copy.
    std::wstring Map(const std::wstring& live) const { return Join(device, live.size() > mount.size() ? live.substr(mount.size()) : L""); }
    // A non-persistent backup shadow is deleted once its components object is released.
    void Close() {
        if (!bc) return;
        IVssAsync* a = nullptr;
        VssWait(bc->BackupComplete(&a), &a);
        bc.Reset();
    }
    ~Shadow() { Close(); }
};

// Exclusive lock on a versions folder, so a scheduled run and the app never write the same destination at once.
struct VersionsLock {
    HANDLE h;
    explicit VersionsLock(const std::wstring& root) {
        h = CreateFileW(XP(Join(root, L".lock")).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        DWORD e = GetLastError();
        if (h == INVALID_HANDLE_VALUE)
            throw Fail{e == ERROR_SHARING_VIOLATION ? L"Another backup or verification is already using this backup folder." : L"Cannot lock the backup folder: " + SysMsg(e)};
    }
    ~VersionsLock() { CloseHandle(h); }
    VersionsLock(const VersionsLock&) = delete;
    VersionsLock& operator=(const VersionsLock&) = delete;
};

// Retention: removes versioned snapshots beyond the newest `keep`, or older than `days` (0 = no limit).
// The newest snapshot always stays. Unchanged files are hard links, so removing one never affects the others.
static int Prune(const std::wstring& root, int keep, int days) {
    if (keep <= 0 && days <= 0) return 0;
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(XP(Join(root, L"snapshot_*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do { if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) names.push_back(fd.cFileName); } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.rbegin(), names.rend());  // newest first
    SYSTEMTIME st;
    GetLocalTime(&st);
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    const uint64_t now = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime, day = 864000000000ULL;
    int removed = 0;
    for (size_t i = 1; i < names.size(); i++) {
        bool drop = keep > 0 && (int)i >= keep;
        int Y, M, D, hh, mm, ss;
        if (!drop && days > 0 && swscanf_s(names[i].c_str() + 9, L"%d-%d-%d_%d-%d-%d", &Y, &M, &D, &hh, &mm, &ss) == 6) {
            SYSTEMTIME t{(WORD)Y, (WORD)M, 0, (WORD)D, (WORD)hh, (WORD)mm, (WORD)ss, 0};
            if (SystemTimeToFileTime(&t, &ft)) drop = now > (((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) + (uint64_t)days * day;
        }
        if (!drop) continue;
        SetCurrent(L"Removing old snapshot " + names[i].substr(9));
        RemoveTree(Join(root, names[i]));
        if (!IsDir(Join(root, names[i]))) removed++;
    }
    return removed;
}

struct VerifyResult { uint64_t checked = 0, ok = 0, bad = 0, missing = 0; std::vector<std::wstring> problems; };
// Re-reads a snapshot from disk and compares every file with the SHA-256 recorded when it was backed up.
// The outcome is saved as verify.json next to backup.json. Returns false when the snapshot has no manifest.
static bool VerifySnapshot(const std::wstring& snapDir, VerifyResult& v) {
    std::vector<ManEntry> man;
    if (!LoadManifest(Join(snapDir, L"manifest.tsv"), man)) return false;
    bool own = job.kind == 2;  // a standalone verification drives the progress counters
    uint64_t base = job.workDone, total = 0;
    for (auto& e : man) if (!e.sha.empty()) total += e.size + FILE_COST;
    job.workTotal = base + total + 1;
    if (own) job.phase = 2;
    Hasher hs;
    std::wstring files = Join(snapDir, L"files");
    for (auto& e : man) {
        if (job.cancel) throw Cancelled{};
        if (e.sha.empty()) continue;
        v.checked++;
        if (own) job.scanned++;
        SetCurrent(L"Verifying " + e.rel);
        std::wstring p = Join(files, e.rel);
        bool exists = Exists(p);
        std::string h = exists ? hs.File(p, true, base) : std::string();
        if (job.cancel) throw Cancelled{};
        std::wstring problem;
        if (h.empty()) { v.missing++; problem = e.rel + (exists ? L" (unreadable)" : L" (missing)"); }
        else if (h != e.sha) { v.bad++; problem = e.rel + L" (checksum mismatch)"; }
        else v.ok++;
        if (!problem.empty() && v.problems.size() < 200) v.problems.push_back(problem);
        if (own) { (problem.empty() ? job.copied : job.linked)++; job.bytesWritten += e.size; }
        base += e.size + FILE_COST;
        job.workDone = base;
    }
    if (!v.checked) return true;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t iso[32];
    swprintf_s(iso, L"%04d-%02d-%02dT%02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring j = L"{\n  \"verified_iso\": \"" + std::wstring(iso) + L"\",\n";
    j += L"  \"checked\": " + std::to_wstring(v.checked) + L",\n  \"ok\": " + std::to_wstring(v.ok) + L",\n";
    j += L"  \"mismatched\": " + std::to_wstring(v.bad) + L",\n  \"missing\": " + std::to_wstring(v.missing) + L",\n  \"problems\": [";
    for (size_t i = 0; i < v.problems.size(); i++) j += (i ? L",\n    \"" : L"\n    \"") + JsonEsc(v.problems[i]) + L"\"";
    j += v.problems.empty() ? L"]\n}\n" : L"\n  ]\n}\n";
    WriteAll(Join(snapDir, L"verify.json"), U8(j));
    return true;
}
static std::wstring VerifySummary(const VerifyResult& v) {
    uint64_t n = v.bad + v.missing;
    if (!n) return L"Verified: all " + Num(v.checked) + L" files match their checksums.";
    return L"Verification found " + Num(n) + (n == 1 ? L" problem" : L" problems") + L" in " + Num(v.checked) + L" files (" + Num(v.bad) +
           L" changed, " + Num(v.missing) + L" missing or unreadable). See verify.json in the snapshot.";
}

struct BackupParams {
    std::wstring src, dst, desc;
    bool full, hardLinks, preserve, versioned, skipHidden;
    std::vector<std::wstring> patterns;
    bool shadow, checksums, verify;
    int keep, keepDays;
    std::wstring fire;  // Firebox: a second, independent destination ("" = Firebox mode off)
};
// Why `fire` cannot be used as the Firebox next to the backup folder `to`, or "" if the pair is fine.
static std::wstring FireboxConflict(const std::wstring& to, const std::wstring& fire) {
    std::wstring a = NormPath(to), b = NormPath(fire);
    if (Lower(a) == Lower(b)) return L"The Firebox must be a different folder from the backup folder.";
    if (IsUnder(b, Join(a, L"EntropyBackup_Versions")) || IsUnder(a, Join(b, L"EntropyBackup_Versions")))
        return L"The Firebox cannot be inside the other destination's versions folder.";
    return {};
}
// True when both folders are on the same local volume, so a Firebox there would not survive losing that drive.
// Network paths are skipped: asking an unreachable server would stall the window.
static bool SameDrive(const std::wstring& a, const std::wstring& b) {
    auto serial = [](const std::wstring& p, DWORD& s) {
        wchar_t vol[MAX_PATH + 1] = {};
        if (p.empty() || p.rfind(L"\\\\", 0) == 0 || !GetVolumePathNameW(p.c_str(), vol, MAX_PATH) || GetDriveTypeW(vol) == DRIVE_REMOTE) return false;
        return GetVolumeInformationW(vol, nullptr, 0, &s, nullptr, nullptr, nullptr, 0) != FALSE;
    };
    DWORD x = 0, y = 0;
    return serial(NormPath(a), x) && serial(NormPath(b), y) && x == y;
}
static void FinishJob(int phase, const std::wstring& msg, const std::wstring& dir = L"") {
    std::wstring title;
    {
        std::lock_guard l(job.m);
        job.message = msg;
        job.resultDir = dir;
        job.current.clear();
        job.t1 = std::chrono::steady_clock::now();
        title = job.title;
    }
    LogEvent(title + L" | " + (phase == 4 ? (job.warnings ? L"WARNINGS" : L"OK") : phase == 6 ? L"CANCELLED" : L"FAILED") + L" | " + msg);
    job.phase = phase;
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_JOBDONE, 0, 0);
}
// One destination of a backup. With Firebox mode on there are two, and each gets its own complete,
// independent snapshot chain: neither is ever copied from the other, so either one can be lost.
struct Leg {
    std::wstring name, dst, root, id, prev, work, final, error;
    std::unique_ptr<VersionsLock> lock;
    std::vector<FileItem> files;  // the scanned files, classified against this destination's previous snapshot
    bool ok = false;
    uint64_t failed = 0, copied = 0, linked = 0, written = 0;
    std::vector<std::wstring> failedNames;
};
// Locks the destination, clears interrupted runs and reserves the snapshot folder. Throws Fail.
static void PrepareLeg(Leg& L, const std::wstring& src, const SYSTEMTIME& start, bool versioned) {
    if (L.dst.empty()) throw Fail{L"No folder was chosen."};
    L.root = Join(L.dst, L"EntropyBackup_Versions");
    if (Lower(src) == Lower(L.dst)) throw Fail{L"Source and backup folders must be different."};
    if (IsUnder(src, L.root)) throw Fail{L"The source folder is inside the backup folder."};
    if (!MakeDirs(L.root)) throw Fail{L"Cannot create the backup folder: " + SysMsg(GetLastError())};
    L.lock = std::make_unique<VersionsLock>(L.root);
    {  // leftovers of an interrupted run (power loss, killed process) are never valid snapshots
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(XP(Join(L.root, L"incomplete_*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (h != INVALID_HANDLE_VALUE) {
            do { if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(Join(L.root, fd.cFileName)); } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    SYSTEMTIME now = start;
    L.id = IdFromTime(now);
    while (versioned && IsDir(Join(L.root, L"snapshot_" + L.id))) {  // IDs have one-second resolution: wait for a free one
        Sleep(200);
        GetLocalTime(&now);
        L.id = IdFromTime(now);
    }
    L.prev = FindPrevious(L.root, versioned);
    L.work = Join(L.root, L"incomplete_" + L.id);
    if (!MakeDirs(Join(L.work, L"files"))) throw Fail{L"Cannot write to the backup folder: " + SysMsg(GetLastError())};
}
// Marks the files unchanged since this destination's previous snapshot. Returns the bytes this leg will copy.
static uint64_t ClassifyLeg(Leg& L, const BackupParams& P) {
    bool incremental = !P.full && !L.prev.empty();
    std::vector<ManEntry> prevMan;
    bool haveManifest = incremental && LoadManifest(Join(L.prev, L"manifest.tsv"), prevMan);
    std::unordered_map<std::wstring, const ManEntry*> man;
    for (auto& e : prevMan) man[Lower(e.rel)] = &e;
    uint64_t copyBytes = 0;
    for (auto& f : L.files) {
        const ManEntry* pe = nullptr;
        if (incremental) {
            if (haveManifest) {
                auto it = man.find(Lower(f.rel));
                pe = it == man.end() ? nullptr : it->second;
                f.link = pe && pe->size == f.size && pe->mtime == f.mtime;
            } else {  // legacy snapshot without manifest: compare against the stored copy
                WIN32_FILE_ATTRIBUTE_DATA a{};
                if (GetFileAttributesExW(XP(Join(Join(L.prev, L"files"), f.rel)).c_str(), GetFileExInfoStandard, &a))
                    f.link = (((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow) == f.size &&
                             (((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime) == f.mtime;
            }
        }
        if (!f.link || !P.hardLinks) copyBytes += f.size;
        if (P.checksums && f.link && pe && !pe->sha.empty()) f.sha = pe->sha;  // an unchanged file keeps the checksum it was stored with
    }
    return copyBytes;
}
// Copies or links every file into the leg's incomplete_ folder. `srcSha` caches source checksums, so a file is
// hashed once even when both destinations need it. Throws Fail (this leg only) or Cancelled (the whole job).
static void CopyLeg(Leg& L, const BackupParams& P, const std::wstring& from, const std::vector<DirItem>& dirs, std::vector<std::string>& srcSha, uint64_t& base) {
    std::wstring filesDir = Join(L.work, L"files"), prevFiles = L.prev.empty() ? L"" : Join(L.prev, L"files");
    uint64_t c0 = job.copied, l0 = job.linked, w0 = job.bytesWritten;
    for (auto& d : dirs) CreateDirectoryW(XP(Join(filesDir, d.rel)).c_str(), nullptr);
    bool linksWork = true;
    Hasher hs;
    for (size_t i = 0; i < L.files.size(); i++) {
        FileItem& f = L.files[i];
        if (job.cancel) throw Cancelled{};
        SetCurrent(f.rel);
        std::wstring out = Join(filesDir, f.rel), in = Join(from, f.rel);
        if (P.checksums && f.sha.empty()) {
            if (srcSha[i].empty()) {
                srcSha[i] = hs.File(in, false, base);
                if (job.cancel) throw Cancelled{};
                base += f.size;
            }
            f.sha = srcSha[i];
        }
        bool done = false;
        if (f.link) {
            std::wstring old = Join(prevFiles, f.rel);
            if (P.hardLinks && linksWork) {
                if (CreateHardLinkW(XP(out).c_str(), XP(old).c_str(), nullptr)) done = true;
                else { DWORD e = GetLastError(); if (e == ERROR_INVALID_FUNCTION || e == ERROR_NOT_SUPPORTED) linksWork = false; }
            } else if (!P.hardLinks) {  // unchanged: copy locally from the previous snapshot
                CopyCtx c{base};
                done = CopyFileExW(XP(old).c_str(), XP(out).c_str(), CopyProgress, &c, nullptr, 0) != FALSE;
                if (done) job.bytesWritten += f.size;
            }
            if (done) job.linked++;
        }
        if (!done) {
            CopyCtx c{base};
            if (CopyFileExW(XP(in).c_str(), XP(out).c_str(), CopyProgress, &c, nullptr, 0)) {
                job.copied++;
                job.bytesWritten += f.size;
                done = true;
                if (P.preserve) { FILETIME ct = FT(f.ctime); SetTimes(out, &ct, nullptr, nullptr, false); }
            } else {
                DWORD e = GetLastError();
                if (e == ERROR_REQUEST_ABORTED || job.cancel) throw Cancelled{};
                if (e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL) throw Fail{L"The backup drive is full."};
                f.failed = true;
                L.failed++;
                if (L.failedNames.size() < 200) L.failedNames.push_back(f.rel + L" (" + SysMsg(e) + L")");
            }
        }
        if (done && !P.preserve && !f.link) {
            SYSTEMTIME st; GetSystemTime(&st);
            FILETIME n; SystemTimeToFileTime(&st, &n);
            SetFileAttributesW(XP(out).c_str(), FILE_ATTRIBUTE_NORMAL);
            SetTimes(out, &n, &n, &n, false);
        }
        base += (f.link && P.hardLinks ? 0 : f.size) + FILE_COST;
        job.workDone = base;
    }
    L.copied = job.copied - c0;
    L.linked = job.linked - l0;
    L.written = job.bytesWritten - w0;
}
// Directory times, manifest, backup.json, then the atomic rename that makes the snapshot real. Throws Fail.
static void FinalizeLeg(Leg& L, const BackupParams& P, const std::wstring& src, const std::vector<DirItem>& dirs, const SYSTEMTIME& now, bool shadowed, bool firebox) {
    std::wstring filesDir = Join(L.work, L"files");
    if (P.preserve)
        for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) SetTimes(Join(filesDir, it->rel), &it->c, &it->a, &it->w, true);
    std::string manifest = P.checksums ? "# Entropy Backup manifest v2: size<TAB>mtime<TAB>sha256<TAB>path\n"
                                       : "# Entropy Backup manifest v1: size<TAB>mtime<TAB>path\n";
    manifest.reserve(L.files.size() * (P.checksums ? 130 : 64));
    uint64_t totalBytes = 0;
    for (auto& f : L.files) {
        totalBytes += f.size;
        if (!f.failed)
            manifest += std::to_string(f.size) + "\t" + std::to_string(f.mtime) + "\t" + (P.checksums ? (f.sha.empty() ? "-" : f.sha) + "\t" : "") + U8(f.rel) + "\n";
    }
    WriteAll(Join(L.work, L"manifest.tsv"), manifest);

    double secs;
    { std::lock_guard l(job.m); secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - job.t0).count(); }
    static const wchar_t* mon[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    wchar_t human[64], iso[32];
    swprintf_s(human, L"%s %d, %d  %d:%02d %s", mon[now.wMonth - 1], now.wDay, now.wYear, now.wHour % 12 ? now.wHour % 12 : 12, now.wMinute, now.wHour < 12 ? L"AM" : L"PM");
    swprintf_s(iso, L"%04d-%02d-%02dT%02d:%02d:%02d", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    std::wstring j = L"{\n";
    j += L"  \"version\": 3,\n";
    j += L"  \"app\": \"Entropy Backup " + std::wstring(APP_VERSION) + L"\",\n";
    j += L"  \"created\": \"" + std::wstring(human) + L"\",\n";
    j += L"  \"created_iso\": \"" + std::wstring(iso) + L"\",\n";
    j += L"  \"source\": \"" + JsonEsc(src) + L"\",\n";
    j += L"  \"description\": \"" + JsonEsc(P.desc) + L"\",\n";
    j += L"  \"type\": \"" + std::wstring(P.full ? L"full" : L"incremental") + L"\",\n";
    j += L"  \"firebox\": " + std::wstring(firebox ? L"true" : L"false") + L",\n";
    j += L"  \"hard_links\": " + std::wstring(P.hardLinks && !P.full ? L"true" : L"false") + L",\n";
    j += L"  \"shadow_copy\": " + std::wstring(shadowed ? L"true" : L"false") + L",\n";
    j += L"  \"checksums\": " + std::wstring(P.checksums ? L"true" : L"false") + L",\n";
    j += L"  \"previous\": \"" + JsonEsc(L.prev.empty() ? L"" : L.prev.substr(L.prev.find_last_of(L'\\') + 1)) + L"\",\n";
    j += L"  \"files\": " + std::to_wstring(L.files.size() - L.failed) + L",\n";
    j += L"  \"copied\": " + std::to_wstring(L.copied) + L",\n";
    j += L"  \"reused\": " + std::to_wstring(L.linked) + L",\n";
    j += L"  \"bytes_written\": " + std::to_wstring(L.written) + L",\n";
    j += L"  \"bytes_total\": " + std::to_wstring(totalBytes) + L",\n";
    wchar_t sec[32]; swprintf_s(sec, L"%.1f", secs);
    j += L"  \"duration_seconds\": " + std::wstring(sec) + L",\n";
    j += L"  \"excludes\": [";
    for (size_t i = 0; i < P.patterns.size(); i++) j += (i ? L", \"" : L"\"") + JsonEsc(P.patterns[i]) + L"\"";
    j += L"],\n  \"failed\": " + std::to_wstring(L.failed) + L",\n  \"failed_files\": [";
    for (size_t i = 0; i < L.failedNames.size(); i++) j += (i ? L",\n    \"" : L"\n    \"") + JsonEsc(L.failedNames[i]) + L"\"";
    j += L.failedNames.empty() ? L"]\n}\n" : L"\n  ]\n}\n";
    WriteAll(Join(L.work, L"backup.json"), U8(j));

    L.final = Join(L.root, P.versioned ? L"snapshot_" + L.id : L"mirror_latest");
    if (!P.versioned && IsDir(L.final)) RemoveTree(L.final);
    if (!MoveFileExW(XP(L.work).c_str(), XP(L.final).c_str(), 0)) throw Fail{L"Could not finalize the snapshot: " + SysMsg(GetLastError())};
    L.work.clear();
    L.ok = true;
}
static void BackupWorker(BackupParams P) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // for the Volume Shadow Copy service
    std::vector<Leg> legs(P.fire.empty() ? 1 : 2);
    auto dropLeg = [](Leg& L, const std::wstring& why) {
        L.error = why;
        if (!L.work.empty()) RemoveTree(L.work);
        L.work.clear();
        L.lock.reset();
    };
    try {
        std::wstring src = NormPath(P.src);
        if (!IsDir(src)) throw Fail{L"The source folder does not exist."};
        legs[0].name = L"Primary";
        legs[0].dst = NormPath(P.dst);
        if (legs.size() == 2) {
            std::wstring why = FireboxConflict(P.dst, P.fire);
            if (!why.empty()) throw Fail{why};
            legs[1].name = L"Firebox";
            legs[1].dst = NormPath(P.fire);
        }
        job.legs = (int)legs.size();

        // 0. Reserve every destination. One that cannot be reached (an unplugged drive, an offline NAS) is
        //    reported, and the backup still goes to the other: that is the point of keeping two.
        SYSTEMTIME now; GetLocalTime(&now);
        size_t ready = 0;
        for (auto& L : legs) {
            try { PrepareLeg(L, src, now, P.versioned); ready++; }
            catch (const Fail& f) { dropLeg(L, f.msg); }
        }
        if (!ready) throw Fail{legs.size() == 1 ? legs[0].error : L"Primary: " + legs[0].error + L" Firebox: " + legs[1].error};

        // 1. Optional shadow copy, then one scan shared by every destination (from the shadow copy when there is one)
        Shadow shadow;
        std::wstring from = src, notes;
        if (P.shadow) {
            SetCurrent(L"Creating shadow copy...");
            std::wstring why = shadow.Create(src);
            if (why.empty()) from = shadow.Map(src);
            else { notes = L" Shadow copy unavailable (" + why + L"), so open files may have been skipped."; job.warnings++; }
        }
        ScanOpts o;
        o.patterns = P.patterns;
        o.skipHidden = P.skipHidden;
        for (auto& L : legs)  // never back up our own versions folders
            if (!L.root.empty() && IsUnder(L.root, src)) o.skipDirs.push_back(Lower(from == src ? L.root : shadow.Map(L.root)));
        std::vector<FileItem> files;
        std::vector<DirItem> dirs;
        Scan(from, L"", o, files, dirs);
        if (job.cancel) throw Cancelled{};

        // 2. Classify unchanged files against each destination's own previous snapshot
        uint64_t total = 0;
        std::vector<char> needHash(files.size());
        for (auto& L : legs) {
            if (!L.lock) continue;
            L.files = files;
            total += ClassifyLeg(L, P) + files.size() * FILE_COST;
            for (size_t i = 0; i < files.size(); i++) if (P.checksums && L.files[i].sha.empty()) needHash[i] = 1;
        }
        for (size_t i = 0; i < files.size(); i++) if (needHash[i]) total += files[i].size;
        job.workTotal = total + 1;
        job.phase = 2;

        // 3. Copy / link into each destination in turn. A failure there costs only that destination.
        std::vector<std::string> srcSha(files.size());
        uint64_t base = 0;
        for (size_t k = 0; k < legs.size(); k++) {
            if (!legs[k].lock) continue;
            job.leg = (int)k;
            try { CopyLeg(legs[k], P, from, dirs, srcSha, base); }
            catch (const Fail& f) { dropLeg(legs[k], f.msg); }
        }
        bool shadowed = from != src;
        shadow.Close();

        // 4. Finalize: directory times, manifest, metadata, atomic rename
        job.phase = 3;
        SetCurrent(L"");
        for (size_t k = 0; k < legs.size(); k++) {
            if (!legs[k].lock) continue;
            job.leg = (int)k;
            try { FinalizeLeg(legs[k], P, src, dirs, now, shadowed, k == 1); }
            catch (const Fail& f) { dropLeg(legs[k], f.msg); }
        }

        // 5. Per destination: summary, optional verification of what was written, then retention
        std::wstring msg, dir;
        size_t saved = 0;
        for (size_t k = 0; k < legs.size(); k++) {
            Leg& L = legs[k];
            job.leg = (int)k;
            std::wstring pre = legs.size() == 1 ? L"" : L.name + L": ";
            if (!L.ok) {
                msg += (msg.empty() ? L"" : L"  ") + pre + L"NOT SAVED. " + L.error;
                job.warnings++;
                continue;
            }
            saved++;
            if (dir.empty()) dir = L.final;
            // One destination keeps the full sentence; with two, each gets a compact clause so both fit the status panel.
            bool brief = legs.size() > 1;
            std::wstring m = brief ? Num(L.files.size() - L.failed) + L" files, " + Bytes(L.written) + L" written"
                                   : Num(L.files.size() - L.failed) + L" files protected, " + Num(L.linked) + L" unchanged reused, " + Bytes(L.written) + L" written.";
            if (L.failed) { m += brief ? L", " + Num(L.failed) + L" unreadable skipped" : L" " + Num(L.failed) + L" locked/unreadable files skipped."; job.warnings += L.failed; }
            if (P.verify && P.checksums) {
                try {
                    VerifyResult v;
                    VerifySnapshot(L.final, v);
                    uint64_t bad = v.bad + v.missing;
                    m += brief ? (bad ? L", " + Num(bad) + L" FAILED verification" : std::wstring(L", verified")) : L" " + VerifySummary(v);
                    job.warnings += bad;
                } catch (const Cancelled&) {
                    m += brief ? L", verification cancelled" : L" Verification was cancelled; the snapshot was kept.";
                    job.warnings++;
                }
            }
            if (P.versioned)
                if (int n = Prune(L.root, P.keep, P.keepDays))
                    m += brief ? L", " + Num(n) + L" old removed" : L" Removed " + Num(n) + (n == 1 ? L" old snapshot." : L" old snapshots.");
            msg += (msg.empty() ? L"" : L"  ") + pre + m + (brief ? L"." : L"");
        }
        msg += notes;
        job.failedLegs = (int)(legs.size() - saved);
        if (saved) FinishJob(4, msg, dir);
        else FinishJob(5, msg);
    } catch (const Fail& f) {
        for (auto& L : legs) dropLeg(L, f.msg);
        FinishJob(5, f.msg);
    } catch (const Cancelled&) {
        for (auto& L : legs) dropLeg(L, L"");
        FinishJob(6, L"Cancelled. No partial snapshot was kept.");
    } catch (const std::exception& e) {
        for (auto& L : legs) dropLeg(L, L"");
        FinishJob(5, W8(e.what()));
    }
    for (auto& L : legs) L.lock.reset();
    if (SUCCEEDED(co)) CoUninitialize();
}
static void RestoreWorker(std::wstring snapDir, std::wstring target, bool overwrite) {
    try {
        std::wstring from = Join(snapDir, L"files");
        target = NormPath(target);
        if (!IsDir(from)) throw Fail{L"This snapshot has no files folder."};
        if (IsUnder(target, snapDir)) throw Fail{L"Choose a restore target outside the snapshot."};
        if (!MakeDirs(target)) throw Fail{L"Cannot create the restore folder: " + SysMsg(GetLastError())};
        std::vector<FileItem> files;
        std::vector<DirItem> dirs;
        Scan(from, L"", ScanOpts{}, files, dirs);
        if (job.cancel) throw Cancelled{};
        uint64_t total = 0;
        for (auto& f : files) total += f.size;
        job.workTotal = total + files.size() * FILE_COST + 1;
        job.phase = 2;
        for (auto& d : dirs) CreateDirectoryW(XP(Join(target, d.rel)).c_str(), nullptr);
        uint64_t base = 0, failed = 0;
        for (auto& f : files) {
            if (job.cancel) throw Cancelled{};
            SetCurrent(f.rel);
            std::wstring out = Join(target, f.rel);
            if (Exists(out) && !overwrite) job.skipped++;
            else {
                if (overwrite) SetFileAttributesW(XP(out).c_str(), FILE_ATTRIBUTE_NORMAL);
                CopyCtx c{base};
                if (CopyFileExW(XP(Join(from, f.rel)).c_str(), XP(out).c_str(), CopyProgress, &c, nullptr, 0)) { job.copied++; job.bytesWritten += f.size; }
                else {
                    DWORD e = GetLastError();
                    if (e == ERROR_REQUEST_ABORTED || job.cancel) throw Cancelled{};
                    if (e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL) throw Fail{L"The target drive is full."};
                    failed++;
                }
            }
            base += f.size + FILE_COST;
            job.workDone = base;
        }
        std::wstring msg = Num(job.copied.load()) + L" files restored to " + target + L".";
        if (job.skipped) msg += L" " + Num(job.skipped.load()) + L" existing files left untouched.";
        if (failed) msg += L" " + Num(failed) + L" files could not be written.";
        FinishJob(4, msg, target);
    } catch (const Fail& f) {
        FinishJob(5, f.msg);
    } catch (const Cancelled&) {
        FinishJob(6, L"Restore cancelled. Files already restored were kept.");
    } catch (const std::exception& e) {
        FinishJob(5, W8(e.what()));
    }
}
static void VerifyWorker(std::wstring snapDir, VerifyResult* out) {
    try {
        VersionsLock lock(snapDir.substr(0, snapDir.find_last_of(L'\\')));
        VerifyResult v;
        if (!VerifySnapshot(snapDir, v)) throw Fail{L"This snapshot has no manifest to verify against."};
        if (!v.checked) throw Fail{L"This snapshot has no checksums. Turn on checksums on the Automation page and later snapshots will record them."};
        job.warnings = v.bad + v.missing;
        if (out) *out = v;
        FinishJob(4, VerifySummary(v), snapDir);
    } catch (const Fail& f) {
        FinishJob(5, f.msg);
    } catch (const Cancelled&) {
        FinishJob(6, L"Verification cancelled.");
    } catch (const std::exception& e) {
        FinishJob(5, W8(e.what()));
    }
}

// ---------------------------------------------------------------------------------------------
// Video background: Media Foundation decodes a looping, muted clip on its own thread (at most 30 fps)
// and hands the newest frame to the window, which draws it as the backdrop and re-blurs the glass.
// ---------------------------------------------------------------------------------------------
static bool IsVideoFile(const std::wstring& f) {
    std::wstring e = Lower(f.substr(f.find_last_of(L'.') == std::wstring::npos ? f.size() : f.find_last_of(L'.')));
    for (auto x : {L".mp4", L".m4v", L".mov", L".wmv", L".avi", L".mkv"}) if (e == x) return true;
    return false;
}
// Built-in backgrounds: three looping videos embedded in the exe (src/backgrounds.rc, made by tools/bggen.cpp).
// Settings store them as "builtin:N"; on first use each is unpacked to %APPDATA%\EntropyBackup\backgrounds.
struct Preset { const wchar_t* name; const wchar_t* file; int res; };
static const Preset PRESETS[3] = {{L"Nebula Lattice", L"nebula-lattice.mp4", 201},
                                  {L"Tidal Fractal", L"tidal-fractal.mp4", 202},
                                  {L"Sacred Geometry", L"sacred-geometry.mp4", 203}};
static int PresetIndex(const std::wstring& bg) {  // 0 = none, 1..3 = built-in video
    if (bg.rfind(L"builtin:", 0) != 0) return 0;
    int i = _wtoi(bg.c_str() + 8);
    return i >= 1 && i <= 3 ? i : 0;
}
static std::wstring PresetFile(int i) {
    const Preset& pr = PRESETS[i - 1];
    std::wstring dir = Join(g_appDir, L"backgrounds"), f = Join(dir, pr.file);
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(pr.res), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const void* data = g ? LockResource(g) : nullptr;
    DWORD n = r ? SizeofResource(nullptr, r) : 0;
    if (!data || !n) return {};
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &a) && a.nFileSizeHigh == 0 && a.nFileSizeLow == n) return f;  // already unpacked
    MakeDirs(dir);
    std::wstring tmp = f + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    DWORD w = 0;
    BOOL ok = WriteFile(h, data, n, &w, nullptr) && w == n;
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), f.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp.c_str()); return {}; }
    return f;
}
// The file to show: a custom image or video, or an unpacked built-in video ("" = the built-in mountain scene).
static std::wstring BackgroundFile() {
    int i = PresetIndex(cfg.bgImage);
    return i ? PresetFile(i) : cfg.bgImage;
}
class VideoBg {
    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    bool stop = false, paused = false, fresh = false;
    std::vector<BYTE> frame;  // newest frame, BGRX, top-down, tightly packed
    UINT fw = 0, fh = 0;
    void Run(std::wstring path, std::promise<std::wstring>* opened);
public:
    std::wstring file;
    bool Active() const { return th.joinable(); }
    // Starts playing `path`. Returns "" on success, otherwise why it cannot be played.
    std::wstring Open(const std::wstring& path) {
        Close();
        std::promise<std::wstring> p;
        auto f = p.get_future();
        stop = paused = fresh = false;
        th = std::thread(&VideoBg::Run, this, path, &p);
        std::wstring why = f.get();
        if (!why.empty()) Close();
        else file = path;
        return why;
    }
    void Close() {
        { std::lock_guard l(m); stop = true; }
        cv.notify_all();
        if (th.joinable()) th.join();
        file.clear();
        frame.clear();
        fw = fh = 0;
    }
    void Pause(bool p) {
        { std::lock_guard l(m); paused = p; }
        cv.notify_all();
    }
    // Moves the newest frame into `out` if there is one the window has not drawn yet.
    bool Take(std::vector<BYTE>& out, UINT& w, UINT& h) {
        std::lock_guard l(m);
        if (!fresh) return false;
        out.swap(frame);
        w = fw; h = fh;
        fresh = false;
        return true;
    }
    ~VideoBg() { Close(); }
};
void VideoBg::Run(std::wstring path, std::promise<std::wstring>* opened) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    {
        ComPtr<IMFSourceReader> rd;
        ComPtr<IMFAttributes> at;
        ComPtr<IMFMediaType> want, cur;
        UINT32 w = 0, h = 0;
        LONG stride = 0;
        auto readFormat = [&]() -> bool {
            cur.Reset();
            if (FAILED(rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)) || FAILED(MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h)) || !w || !h) return false;
            stride = (LONG)MFGetAttributeUINT32(cur.Get(), MF_MT_DEFAULT_STRIDE, w * 4);
            return true;
        };
        HRESULT hr = MFCreateAttributes(&at, 1);
        if (SUCCEEDED(hr)) hr = at->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);  // colour conversion to RGB32
        if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromURL(path.c_str(), at.Get(), &rd);
        if (SUCCEEDED(hr)) hr = rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);  // no audio: the backdrop is silent
        if (SUCCEEDED(hr)) hr = rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        if (SUCCEEDED(hr)) hr = MFCreateMediaType(&want);
        if (SUCCEEDED(hr)) hr = want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (SUCCEEDED(hr)) hr = rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want.Get());
        if (SUCCEEDED(hr) && !readFormat()) hr = E_FAIL;
        if (FAILED(hr)) {
            wchar_t b[96];
            swprintf_s(b, L"Windows cannot play this video (0x%08lX). Try an H.264 .mp4.", (unsigned long)hr);
            opened->set_value(b);
        } else {
            opened->set_value(L"");
            using clock = std::chrono::steady_clock;
            const LONGLONG MIN_GAP = 330000;  // 33 ms in 100 ns units: at most 30 frames per second
            clock::time_point t0{};
            LONGLONG ts0 = -1, last = -1;
            std::vector<BYTE> buf;
            for (;;) {
                {
                    std::unique_lock l(m);
                    if (paused) { cv.wait(l, [&] { return stop || !paused; }); ts0 = -1; }  // resume: restart the clock
                    if (stop) break;
                }
                DWORD flags = 0;
                LONGLONG ts = 0;
                ComPtr<IMFSample> sm;
                if (FAILED(rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sm))) break;
                if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {  // loop
                    PROPVARIANT pv{};
                    pv.vt = VT_I8;
                    if (FAILED(rd->SetCurrentPosition(GUID_NULL, pv))) break;
                    ts0 = last = -1;
                    continue;
                }
                if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) if (!readFormat()) break;
                if (!sm) continue;
                if (last >= 0 && ts - last < MIN_GAP && ts > last) continue;
                last = ts;
                auto now = clock::now();
                if (ts0 < 0 || now > t0 + std::chrono::microseconds((ts - ts0) / 10) + std::chrono::milliseconds(250)) { t0 = now; ts0 = ts; }  // start, or fell behind
                {
                    std::unique_lock l(m);
                    if (cv.wait_until(l, t0 + std::chrono::microseconds((ts - ts0) / 10), [&] { return stop || paused; }) && stop) break;
                }
                ComPtr<IMFMediaBuffer> mb;
                if (FAILED(sm->ConvertToContiguousBuffer(&mb))) continue;
                buf.resize((size_t)w * h * 4);
                ComPtr<IMF2DBuffer> b2;
                BYTE* scan0 = nullptr;
                LONG pitch = 0;
                BYTE* raw = nullptr;
                DWORD len = 0;
                bool locked2d = SUCCEEDED(mb.As(&b2)) && SUCCEEDED(b2->Lock2D(&scan0, &pitch));
                if (!locked2d) {
                    if (FAILED(mb->Lock(&raw, nullptr, &len))) continue;
                    pitch = stride;
                    scan0 = stride < 0 ? raw + (size_t)(h - 1) * (size_t)(-stride) : raw;  // bottom-up rows
                    if ((size_t)len < (size_t)std::abs(stride) * h) { mb->Unlock(); continue; }
                }
                for (UINT y = 0; y < h; y++) memcpy(buf.data() + (size_t)y * w * 4, scan0 + (ptrdiff_t)y * pitch, (size_t)w * 4);
                if (locked2d) b2->Unlock2D(); else mb->Unlock();
                {
                    std::lock_guard l(m);
                    frame.swap(buf);
                    fw = w; fh = h;
                    fresh = true;
                }
                if (g_hwnd) PostMessageW(g_hwnd, WM_APP_FRAME, 0, 0);
            }
        }
    }
    MFShutdown();
    if (SUCCEEDED(co)) CoUninitialize();
}
static VideoBg video;

// ---------------------------------------------------------------------------------------------
// Direct2D resources and drawing primitives
// ---------------------------------------------------------------------------------------------
static ComPtr<ID2D1Factory> d2f;
static ComPtr<IDWriteFactory> dwf;
static ComPtr<IWICImagingFactory> wic;
static ComPtr<ID2D1HwndRenderTarget> rt;
static ComPtr<ID2D1StrokeStyle> roundSt;
static ComPtr<ID2D1Bitmap> bgFull, bgBlur, bgImageBmp, signatureBmp, videoBmp;
static ComPtr<ID2D1BitmapRenderTarget> bgRT[6];  // backdrop at full size, then the blur chain: 1/4, 1/12, 1/24, 1/10, 1/4
static ComPtr<ID2D1BitmapBrush> blurBrush, sharpBrush;
static UINT bgW = 0, bgH = 0;
static bool bgDirty = true;
static ID2D1RenderTarget* R = nullptr;
static float gs = 1, gox = 0, goy = 0;  // design -> pixel transform
static UINT g_dpi = 96;

static const DWRITE_FONT_WEIGHT REG = DWRITE_FONT_WEIGHT_NORMAL, SEMI = DWRITE_FONT_WEIGHT_SEMI_BOLD, BOLD = DWRITE_FONT_WEIGHT_BOLD;
static const DWRITE_TEXT_ALIGNMENT LEFT = DWRITE_TEXT_ALIGNMENT_LEADING, CENTER = DWRITE_TEXT_ALIGNMENT_CENTER, RIGHT = DWRITE_TEXT_ALIGNMENT_TRAILING;
static const DWRITE_PARAGRAPH_ALIGNMENT VMID = DWRITE_PARAGRAPH_ALIGNMENT_CENTER, VTOP = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;

static D2D1_COLOR_F C(UINT32 rgb, float a = 1.f) { return D2D1::ColorF(rgb, a); }
static D2D1_RECT_F RC(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
static D2D1_POINT_2F P2(float x, float y) { return D2D1::Point2F(x, y); }
static D2D1_RECT_F Inflate(D2D1_RECT_F r, float d) { return RC(r.left - d, r.top - d, r.right + d, r.bottom + d); }
static ComPtr<ID2D1SolidColorBrush> SB(D2D1_COLOR_F c) { ComPtr<ID2D1SolidColorBrush> b; R->CreateSolidColorBrush(c, &b); return b; }
static ComPtr<ID2D1GradientStopCollection> Stops(std::initializer_list<D2D1_GRADIENT_STOP> s) {
    ComPtr<ID2D1GradientStopCollection> c;
    R->CreateGradientStopCollection(s.begin(), (UINT32)s.size(), &c);
    return c;
}
static ComPtr<ID2D1LinearGradientBrush> LG(D2D1_POINT_2F a, D2D1_POINT_2F b, std::initializer_list<D2D1_GRADIENT_STOP> s) {
    ComPtr<ID2D1LinearGradientBrush> g;
    R->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), Stops(s).Get(), &g);
    return g;
}
static ComPtr<ID2D1RadialGradientBrush> RG(D2D1_POINT_2F c, float rx, float ry, std::initializer_list<D2D1_GRADIENT_STOP> s) {
    ComPtr<ID2D1RadialGradientBrush> g;
    R->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(c, P2(0, 0), rx, ry), Stops(s).Get(), &g);
    return g;
}
static void FillRR(D2D1_RECT_F r, float rad, ID2D1Brush* b) { R->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), b); }
static void StrokeRR(D2D1_RECT_F r, float rad, ID2D1Brush* b, float w) { R->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), b, w); }
static void Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float w) { R->DrawLine(P2(x1, y1), P2(x2, y2), SB(c).Get(), w, roundSt.Get()); }
template <class F> static ComPtr<ID2D1PathGeometry> Geo(F f) {
    ComPtr<ID2D1PathGeometry> g;
    d2f->CreatePathGeometry(&g);
    ComPtr<ID2D1GeometrySink> s;
    g->Open(&s);
    f(s.Get());
    s->Close();
    return g;
}
static void Poly(ID2D1GeometrySink* k, std::initializer_list<D2D1_POINT_2F> pts, bool closed, bool filled = false) {
    auto it = pts.begin();
    k->BeginFigure(*it, filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    for (++it; it != pts.end(); ++it) k->AddLine(*it);
    k->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
}
static void ArcFig(ID2D1GeometrySink* k, float cx, float cy, float r, float deg0, float deg1) {
    float a0 = deg0 * PI / 180, a1 = deg1 * PI / 180;
    k->BeginFigure(P2(cx + r * cosf(a0), cy + r * sinf(a0)), D2D1_FIGURE_BEGIN_HOLLOW);
    k->AddArc(D2D1::ArcSegment(P2(cx + r * cosf(a1), cy + r * sinf(a1)), D2D1::SizeF(r, r), 0, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                               deg1 - deg0 > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    k->EndFigure(D2D1_FIGURE_END_OPEN);
}

static IDWriteTextFormat* Fmt(float size, DWRITE_FONT_WEIGHT wt, DWRITE_TEXT_ALIGNMENT al, bool wrap, DWRITE_PARAGRAPH_ALIGNMENT pa) {
    static std::map<std::wstring, ComPtr<IDWriteTextFormat>> cache;
    wchar_t key[96];
    swprintf_s(key, L"%.2f|%d|%d|%d|%d", size, (int)wt, (int)al, (int)wrap, (int)pa);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.Get();
    ComPtr<IDWriteTextFormat> f;
    dwf->CreateTextFormat(size >= 24 ? L"Segoe UI Variable Display" : L"Segoe UI Variable Text", nullptr, wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    if (!f) dwf->CreateTextFormat(L"Segoe UI", nullptr, wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    f->SetTextAlignment(al);
    f->SetParagraphAlignment(pa);
    f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING t{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    ComPtr<IDWriteInlineObject> sign;
    dwf->CreateEllipsisTrimmingSign(f.Get(), &sign);
    f->SetTrimming(&t, sign.Get());
    cache[key] = f;
    return f.Get();
}
static void Text(const std::wstring& s, D2D1_RECT_F r, float size, D2D1_COLOR_F c, DWRITE_FONT_WEIGHT wt = REG, DWRITE_TEXT_ALIGNMENT al = LEFT, bool wrap = false, DWRITE_PARAGRAPH_ALIGNMENT pa = VMID) {
    if (s.empty()) return;
    R->DrawTextW(s.c_str(), (UINT32)s.size(), Fmt(size, wt, al, wrap, pa), r, SB(c).Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
static float TW(const std::wstring& s, float size, DWRITE_FONT_WEIGHT wt = REG) {
    ComPtr<IDWriteTextLayout> l;
    dwf->CreateTextLayout(s.c_str(), (UINT32)s.size(), Fmt(size, wt, LEFT, false, VMID), 10000, 200, &l);
    if (!l) return 0;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

// ---------------------------------------------------------------------------------------------
// Vector icons (24-unit grid, scaled to size s)
// ---------------------------------------------------------------------------------------------
enum Ico { IC_NONE, IC_HOME, IC_HISTORY, IC_RESTORE, IC_GEAR, IC_EXCLUDE, IC_INFO, IC_FOLDER, IC_DRIVE, IC_DOC, IC_CUBE,
           IC_DB, IC_CHEVRON, IC_DOTS, IC_HISTHDR, IC_FOLDEROPEN, IC_STOP, IC_CHECK, IC_CALENDAR, IC_FLAME };
static void Icon(int ic, float x, float y, float s, D2D1_COLOR_F c) {
    float u = s / 24.f, w = std::max(1.f, 1.7f * u);
    auto p = [&](float a, float b) { return P2(x + a * u, y + b * u); };
    auto br = SB(c);
    auto stroke = [&](ID2D1Geometry* g, float width) { R->DrawGeometry(g, br.Get(), width, roundSt.Get()); };
    auto arrow = [&](float tx, float ty, float ang, float len) {
        for (float d : {0.65f, -0.65f}) Line(tx, ty, tx - len * cosf(ang + d), ty - len * sinf(ang + d), c, w);
    };
    switch (ic) {
    case IC_HOME:
        stroke(Geo([&](ID2D1GeometrySink* k) {
            Poly(k, {p(-10, 0), p(0, -9), p(10, 0)}, false);
            Poly(k, {p(-7, -2.5f), p(-7, 9), p(7, 9), p(7, -2.5f)}, false);
            Poly(k, {p(-2.5f, 9), p(-2.5f, 3.5f), p(2.5f, 3.5f), p(2.5f, 9)}, false);
        }).Get(), w);
        break;
    case IC_HISTORY:
    case IC_RESTORE: {
        float r = 9.5f * u, a0 = ic == IC_HISTORY ? 200.f : 215.f;
        stroke(Geo([&](ID2D1GeometrySink* k) { ArcFig(k, x, y, r, a0, a0 + 295); }).Get(), w);
        float ar = a0 * PI / 180;
        arrow(x + r * cosf(ar), y + r * sinf(ar), ar - PI / 2, 4.2f * u);
        if (ic == IC_HISTORY) stroke(Geo([&](ID2D1GeometrySink* k) { Poly(k, {p(0, -5), p(0, 0), p(3.5f, 2.5f)}, false); }).Get(), w);
        else R->FillEllipse(D2D1::Ellipse(P2(x, y), 2.f * u, 2.f * u), br.Get());
        break;
    }
    case IC_GEAR: {
        auto pol = [&](float a, float r) { return p(r * cosf(a), r * sinf(a)); };
        stroke(Geo([&](ID2D1GeometrySink* k) {
            for (int i = 0; i < 8; i++) {
                float a = i * PI / 4;
                D2D1_POINT_2F q[4] = {pol(a - 0.30f, 7.4f), pol(a - 0.14f, 10.2f), pol(a + 0.14f, 10.2f), pol(a + 0.30f, 7.4f)};
                if (!i) k->BeginFigure(q[0], D2D1_FIGURE_BEGIN_HOLLOW); else k->AddLine(q[0]);
                for (int j = 1; j < 4; j++) k->AddLine(q[j]);
            }
            k->EndFigure(D2D1_FIGURE_END_CLOSED);
        }).Get(), w);
        R->DrawEllipse(D2D1::Ellipse(P2(x, y), 3.3f * u, 3.3f * u), br.Get(), w);
        break;
    }
    case IC_EXCLUDE:
        StrokeRR(RC(x - 9 * u, y - 9 * u, x + 9 * u, y + 9 * u), 2.5f * u, br.Get(), w);
        Line(x - 4 * u, y - 4 * u, x + 4 * u, y + 4 * u, c, w);
        Line(x + 4 * u, y - 4 * u, x - 4 * u, y + 4 * u, c, w);
        break;
    case IC_INFO:
        R->DrawEllipse(D2D1::Ellipse(P2(x, y), 10 * u, 10 * u), br.Get(), w);
        Line(x, y - 0.5f * u, x, y + 5.5f * u, c, w * 1.1f);
        R->FillEllipse(D2D1::Ellipse(P2(x, y - 4.8f * u), 1.3f * u, 1.3f * u), br.Get());
        break;
    case IC_FOLDER:
        stroke(Geo([&](ID2D1GeometrySink* k) {
            Poly(k, {p(-10, -7), p(-3.5f, -7), p(-1.5f, -4.5f), p(10, -4.5f), p(10, 8), p(-10, 8)}, true);
            Poly(k, {p(-10, -1.5f), p(10, -1.5f)}, false);
        }).Get(), w);
        break;
    case IC_DRIVE:
        stroke(Geo([&](ID2D1GeometrySink* k) {
            Poly(k, {p(-7, -8), p(7, -8), p(10, -1), p(-10, -1)}, true);
            Poly(k, {p(-10, -1), p(-10, 8), p(10, 8), p(10, -1)}, false);
        }).Get(), w);
        R->FillEllipse(D2D1::Ellipse(p(5.5f, 3.5f), 1.1f * u, 1.1f * u), br.Get());
        R->FillEllipse(D2D1::Ellipse(p(1.8f, 3.5f), 1.1f * u, 1.1f * u), br.Get());
        break;
    case IC_DOC:
        stroke(Geo([&](ID2D1GeometrySink* k) {
            Poly(k, {p(-7, -10), p(3, -10), p(8, -5), p(8, 10), p(-7, 10)}, true);
            Poly(k, {p(3, -10), p(3, -5), p(8, -5)}, false);
            Poly(k, {p(-3.5f, -1), p(4.5f, -1)}, false);
            Poly(k, {p(-3.5f, 3), p(4.5f, 3)}, false);
            Poly(k, {p(-3.5f, 7), p(1.5f, 7)}, false);
        }).Get(), w);
        break;
    case IC_CUBE:
        stroke(Geo([&](ID2D1GeometrySink* k) {
            Poly(k, {p(0, -11), p(10, -5.5f), p(10, 5.5f), p(0, 11), p(-10, 5.5f), p(-10, -5.5f)}, true);
            Poly(k, {p(-10, -5.5f), p(0, 0), p(10, -5.5f)}, false);
            Poly(k, {p(0, 0), p(0, 11)}, false);
            Poly(k, {p(-5, -8.25f), p(5, -2.75f)}, false);
        }).Get(), w);
        break;
    case IC_DB: {
        float rx = 9 * u, ry = 3.4f * u;
        auto body = LG(p(-9, 0), p(9, 0), {{0, C(0x2D6FC4)}, {0.45f, C(0x5EA9F2)}, {1, C(0x245CA8)}});
        auto band = SB(C(0x194E96));
        for (int i = 2; i >= 0; i--) {
            float yt = y - 7.5f * u + i * 6 * u;
            R->FillEllipse(D2D1::Ellipse(P2(x, yt + 5 * u), rx, ry), band.Get());
            R->FillRectangle(RC(x - rx, yt, x + rx, yt + 5 * u), body.Get());
            R->FillEllipse(D2D1::Ellipse(P2(x, yt), rx, ry), i == 0 ? SB(C(0xA6D6FF)).Get() : static_cast<ID2D1Brush*>(body.Get()));
        }
        R->DrawEllipse(D2D1::Ellipse(P2(x, y - 7.5f * u), rx, ry), SB(C(0xDDF0FF, .8f)).Get(), 0.8f * u);
        break;
    }
    case IC_CHEVRON:
        stroke(Geo([&](ID2D1GeometrySink* k) { Poly(k, {p(-3, -6.5f), p(3.5f, 0), p(-3, 6.5f)}, false); }).Get(), w * 1.1f);
        break;
    case IC_DOTS:
        for (float dx : {-6.5f, 0.f, 6.5f}) R->FillEllipse(D2D1::Ellipse(p(dx, 0), 1.8f * u, 1.8f * u), br.Get());
        break;
    case IC_HISTHDR:
        R->DrawEllipse(D2D1::Ellipse(P2(x, y), 11 * u, 11 * u), br.Get(), w);
        StrokeRR(RC(x - 5 * u, y - 5 * u, x + 5 * u, y + 5 * u), 1.5f * u, br.Get(), w);
        break;
    case IC_FOLDEROPEN:
        stroke(Geo([&](ID2D1GeometrySink* k) {
            Poly(k, {p(8, -1.5f), p(8, -5.5f), p(-2.5f, -5.5f), p(-4.5f, -8), p(-11, -8), p(-11, 8)}, false);
            Poly(k, {p(-11, 8), p(-7, -1.5f), p(12, -1.5f), p(8, 8)}, true);
        }).Get(), w);
        break;
    case IC_STOP:
        FillRR(RC(x - 6 * u, y - 6 * u, x + 6 * u, y + 6 * u), 2 * u, br.Get());
        break;
    case IC_CHECK:
        stroke(Geo([&](ID2D1GeometrySink* k) { Poly(k, {p(-5.5f, 0.5f), p(-1.5f, 4.5f), p(6, -4.5f)}, false); }).Get(), 2.4f * u);
        break;
    case IC_FLAME: {  // Firebox: a flame with an inner tongue
        auto bez = [&](ID2D1GeometrySink* k, float a, float b, float c, float d, float e, float f) { k->AddBezier(D2D1::BezierSegment(p(a, b), p(c, d), p(e, f))); };
        stroke(Geo([&](ID2D1GeometrySink* k) {
            k->BeginFigure(p(0, 10.5f), D2D1_FIGURE_BEGIN_HOLLOW);
            bez(k, -6.5f, 10.5f, -9, 5, -7.5f, 0);
            bez(k, -6.2f, -4, -2.2f, -5, -1.6f, -10.5f);
            bez(k, 3.5f, -7.5f, 8.2f, -2.5f, 7.6f, 3);
            bez(k, 7.1f, 7.6f, 3.8f, 10.5f, 0, 10.5f);
            k->EndFigure(D2D1_FIGURE_END_CLOSED);
            k->BeginFigure(p(0, 10.5f), D2D1_FIGURE_BEGIN_HOLLOW);
            bez(k, -2.8f, 10.5f, -4, 7.5f, -3, 5);
            bez(k, -2.2f, 3, -0.4f, 2.5f, 0.3f, 0);
            bez(k, 2.4f, 2, 3.8f, 4.5f, 3.2f, 7);
            bez(k, 2.8f, 9, 1.5f, 10.5f, 0, 10.5f);
            k->EndFigure(D2D1_FIGURE_END_CLOSED);
        }).Get(), w);
        break;
    }
    case IC_CALENDAR:
        R->DrawRoundedRectangle(D2D1::RoundedRect(RC(x - 8.5f * u, y - 6.5f * u, x + 8.5f * u, y + 8.5f * u), 2.5f * u, 2.5f * u), br.Get(), w);
        stroke(Geo([&](ID2D1GeometrySink* k) {
            Poly(k, {p(-8.5f, -1.5f), p(8.5f, -1.5f)}, false);
            Poly(k, {p(-4, -9), p(-4, -4.5f)}, false);
            Poly(k, {p(4, -9), p(4, -4.5f)}, false);
            Poly(k, {p(-3.5f, 3.5f), p(-0.5f, 6), p(4, 1)}, false);
        }).Get(), w);
        break;
    }
}

// Backup-target cube: the three inner edges double as hour, minute and second hands,
// the outer corners as the 12 / 4 / 8 o'clock marks.
static void DrawClockCube(float x, float y, float R0) {
    auto v = [&](int i, float k = 1.f) { float a = (-90 + 60.f * i) * PI / 180; return P2(x + R0 * k * cosf(a), y + R0 * k * sinf(a)); };
    D2D1_POINT_2F c = P2(x, y);
    float u = R0 / 30;
    auto face = [&](D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F d, UINT32 col) {
        auto g = Geo([&](ID2D1GeometrySink* k) { Poly(k, {c, a, b, d}, true, true); });
        R->FillGeometry(g.Get(), SB(C(col)).Get());
    };
    auto hex = Geo([&](ID2D1GeometrySink* k) { Poly(k, {v(0), v(1), v(2), v(3), v(4), v(5)}, true, true); });
    for (int i = 3; i >= 1; i--) R->DrawGeometry(hex.Get(), SB(C(0xF2CF7A, 0.10f)).Get(), 4.f * u * i, roundSt.Get());
    face(v(5), v(0), v(1), 0x2B3D5C);   // top
    face(v(3), v(4), v(5), 0x162641);   // left
    face(v(1), v(2), v(3), 0x0D1A2E);   // right
    for (int i : {5, 1, 3}) R->DrawLine(c, v(i), SB(C(0xF2CF7A, 0.30f)).Get(), 1.4f * u, roundSt.Get());
    R->DrawGeometry(hex.Get(), LG(v(5), v(2), {{0, C(0xFBE3A0)}, {1, C(0xD3A445)}}).Get(), 3.2f * u, roundSt.Get());
    for (int i : {0, 2, 4}) R->FillEllipse(D2D1::Ellipse(v(i), 2.6f * u, 2.6f * u), SB(C(0xFFF0C4)).Get());
    R->DrawLine(c, v(5, 0.60f), SB(C(0xF7DA90)).Get(), 5.2f * u, roundSt.Get());                 // hour   -> 10 o'clock
    R->DrawLine(c, v(1, 0.86f), SB(C(0xF7DA90)).Get(), 3.6f * u, roundSt.Get());                 // minute -> 2 o'clock
    R->DrawLine(v(0, 0.22f), v(3, 0.94f), SB(C(0x5CCBFF)).Get(), 2.0f * u, roundSt.Get());       // second -> 6 o'clock, with tail
    R->FillEllipse(D2D1::Ellipse(c, 3.4f * u, 3.4f * u), SB(C(0xF7DA90)).Get());
    R->FillEllipse(D2D1::Ellipse(c, 1.5f * u, 1.5f * u), SB(C(0x5CCBFF)).Get());
}

// Hexagonal Entropy logo: stacked documents, folder, drive and a sync arrow.
static void DrawLogo(float cx, float cy, float r) {
    auto hex = [&](float rr) {
        return Geo([&](ID2D1GeometrySink* k) {
            k->BeginFigure(P2(cx, cy - rr), D2D1_FIGURE_BEGIN_FILLED);
            for (int i = 1; i < 6; i++) { float a = (-90 + 60.f * i) * PI / 180; k->AddLine(P2(cx + rr * cosf(a), cy + rr * sinf(a))); }
            k->EndFigure(D2D1_FIGURE_END_CLOSED);
        });
    };
    auto outer = hex(r), inner = hex(r * 0.87f);
    for (int i = 4; i >= 1; i--) R->DrawGeometry(outer.Get(), SB(C(0x2FB0FF, 0.08f)).Get(), r * 0.06f * i, roundSt.Get());
    R->FillGeometry(outer.Get(), RG(P2(cx, cy - r * 0.25f), r * 1.15f, r * 1.15f, {{0, C(0x3C8CF2)}, {0.55f, C(0x1A4FB4)}, {1, C(0x0A2466)}}).Get());
    R->DrawGeometry(outer.Get(), LG(P2(cx - r, cy - r), P2(cx + r, cy + r), {{0, C(0xB8F0FF)}, {0.5f, C(0x45B8FF)}, {1, C(0x1E78E0)}}).Get(), r * 0.055f, roundSt.Get());
    R->DrawGeometry(inner.Get(), SB(C(0x8FD8FF, 0.35f)).Get(), r * 0.018f, roundSt.Get());
    float u = r / 100;
    auto q = [&](float x, float y) { return P2(cx + x * u, cy + y * u); };
    auto rr = [&](float l, float t, float rgt, float b) { return RC(cx + l * u, cy + t * u, cx + rgt * u, cy + b * u); };
    // documents
    FillRR(rr(-30, -54, 18, -2), 4 * u, SB(C(0xC9DDF3)).Get());
    FillRR(rr(-40, -46, 8, 4), 4 * u, LG(q(0, -46), q(0, 4), {{0, C(0xFFFFFF)}, {1, C(0xD5E5F7)}}).Get());
    for (int i = 0; i < 4; i++) Line(cx - 32 * u, cy + (-36 + i * 8) * u, cx + (i == 3 ? -12 : 0) * u, cy + (-36 + i * 8) * u, C(0x7EA6D3), 2.2f * u);
    // folder
    auto folder = Geo([&](ID2D1GeometrySink* k) {
        Poly(k, {q(-52, -22), q(-30, -22), q(-24, -15), q(14, -15), q(14, 34), q(-52, 34)}, true, true);
    });
    R->FillGeometry(folder.Get(), LG(q(0, -22), q(0, 34), {{0, C(0x9ED0FF)}, {1, C(0x3E86DA)}}).Get());
    FillRR(rr(-56, -6, 18, 38), 5 * u, LG(q(0, -6), q(0, 38), {{0, C(0xE6F3FF)}, {1, C(0x8DBDF0)}}).Get());
    // sync arrow: from the project down into the time cube
    D2D1_POINT_2F a0 = q(4, -40), c1 = q(30, -46), c2 = q(44, -28), a1 = q(41, -10);
    auto arc = Geo([&](ID2D1GeometrySink* k) {
        k->BeginFigure(a0, D2D1_FIGURE_BEGIN_HOLLOW);
        k->AddBezier(D2D1::BezierSegment(c1, c2, a1));
        k->EndFigure(D2D1_FIGURE_END_OPEN);
    });
    R->DrawGeometry(arc.Get(), SB(C(0x0B3E8E)).Get(), 12 * u, roundSt.Get());
    R->DrawGeometry(arc.Get(), LG(a0, a1, {{0, C(0x7FD3FF)}, {1, C(0x2AA0FF)}}).Get(), 7 * u, roundSt.Get());
    float dx = a1.x - c2.x, dy = a1.y - c2.y, dl = sqrtf(dx * dx + dy * dy);
    dx /= dl; dy /= dl;
    auto head = Geo([&](ID2D1GeometrySink* k) {
        Poly(k, {P2(a1.x + dx * 13 * u, a1.y + dy * 13 * u), P2(a1.x - dy * 11 * u, a1.y + dx * 11 * u), P2(a1.x + dy * 11 * u, a1.y - dx * 11 * u)}, true, true);
    });
    R->FillGeometry(head.Get(), SB(C(0x2AA0FF)).Get());
    R->DrawGeometry(head.Get(), SB(C(0x0B3E8E)).Get(), 1.5f * u, roundSt.Get());
    DrawClockCube(cx + 34 * u, cy + 34 * u, 27 * u);
}

// ---------------------------------------------------------------------------------------------
// Background scene (procedural mountain dusk, or a user-chosen image) + frosted-glass source
// ---------------------------------------------------------------------------------------------
static std::vector<float> Ridge(unsigned seed, int n, float rough) {
    std::mt19937 g(seed);
    std::uniform_real_distribution<float> u(-1, 1);
    std::vector<float> v((size_t)n + 1);
    v[0] = u(g) * 0.5f; v[n] = u(g) * 0.5f;
    float amp = 1;
    for (int step = n; step > 1; step /= 2) {
        for (int i = 0; i + step <= n; i += step) v[i + step / 2] = (v[i] + v[i + step]) / 2 + u(g) * amp;
        amp *= rough;
    }
    auto [mn, mx] = std::minmax_element(v.begin(), v.end());
    float lo = *mn, hi = *mx;
    for (auto& x : v) x = (x - lo) / (hi - lo + 1e-6f);
    return v;
}
static void MountainLayer(float w, float h, unsigned seed, float base, float amp, float rough, float envEnd,
                          UINT32 top, UINT32 bottom, UINT32 lit, float litAmt, float snow) {
    const int N = 256;
    auto v = Ridge(seed, N, rough);
    std::vector<D2D1_POINT_2F> pts(N + 1);
    float minY = h;
    for (int i = 0; i <= N; i++) {
        float t = i / (float)N, env = 1;
        if (envEnd > 0) env = t < envEnd ? 1.f : std::max(0.12f, 1.f - (t - envEnd) / 0.28f);
        pts[i] = P2(t * w, h * (base - amp * v[i] * env));
        minY = std::min(minY, pts[i].y);
    }
    auto ridge = Geo([&](ID2D1GeometrySink* k) {
        k->BeginFigure(P2(0, h), D2D1_FIGURE_BEGIN_FILLED);
        for (auto& p : pts) k->AddLine(p);
        k->AddLine(P2(w, h));
        k->EndFigure(D2D1_FIGURE_END_CLOSED);
    });
    R->FillGeometry(ridge.Get(), LG(P2(0, minY), P2(0, h), {{0, C(top)}, {1, C(bottom)}}).Get());
    // Light from the right: faces sloping down to the right catch the sunset (slope smoothed to avoid banding).
    float depth = h * 0.16f, dx = w / N;
    R->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);  // adjacent strips must not leave AA seams
    for (int i = 0; i < N; i++) {
        float sum = 0;
        int cnt = 0;
        for (int j = std::max(0, i - 4); j <= std::min(N - 1, i + 4); j++, cnt++) sum += (pts[j + 1].y - pts[j].y) / dx;
        float L = sum / cnt * 1.4f + 0.1f;
        if (L <= 0) continue;
        float a = std::min(1.f, L) * litAmt, yTop = std::min(pts[i].y, pts[i + 1].y);
        auto q = Geo([&](ID2D1GeometrySink* k) {
            Poly(k, {P2(pts[i].x, pts[i].y), P2(pts[i + 1].x, pts[i + 1].y), P2(pts[i + 1].x, yTop + depth), P2(pts[i].x, yTop + depth)}, true, true);
        });
        R->FillGeometry(q.Get(), LG(P2(0, yTop), P2(0, yTop + depth), {{0, C(lit, a)}, {0.5f, C(lit, a * 0.45f)}, {1, C(lit, 0)}}).Get());
    }
    R->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (snow > 0) R->FillGeometry(ridge.Get(), LG(P2(0, minY), P2(0, minY + h * 0.09f), {{0, C(0xF6F2EA, snow)}, {1, C(0xF6F2EA, 0)}}).Get());
    R->FillGeometry(ridge.Get(), LG(P2(0, minY), P2(0, h), {{0, C(bottom, 0)}, {0.45f, C(bottom, 0.35f)}, {1, C(bottom, 0.96f)}}).Get());
}
static void PaintScene(float w, float h) {
    if (ID2D1Bitmap* img = video.Active() && videoBmp ? videoBmp.Get() : bgImageBmp.Get()) {  // cover the window, centred
        D2D1_SIZE_F s = img->GetSize();
        float k = std::max(w / s.width, h / s.height);
        float iw = s.width * k, ih = s.height * k;
        R->DrawBitmap(img, RC((w - iw) / 2, (h - ih) / 2, (w + iw) / 2, (h + ih) / 2), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        return;
    }
    R->FillRectangle(RC(0, 0, w, h), LG(P2(0, 0), P2(0, h), {{0, C(0x0F1B2E)}, {0.28f, C(0x2A4061)}, {0.48f, C(0x6E7F98)},
                                                             {0.60f, C(0xC9A27E)}, {0.70f, C(0x8C7264)}, {1, C(0x20242F)}}).Get());
    R->FillEllipse(D2D1::Ellipse(P2(w * 0.64f, h * 0.57f), w * 0.46f, h * 0.38f),
                   RG(P2(w * 0.64f, h * 0.57f), w * 0.46f, h * 0.38f, {{0, C(0xFFE6B8, 0.9f)}, {0.3f, C(0xF7BD80, 0.4f)}, {1, C(0xF7BD80, 0)}}).Get());
    std::mt19937 g(1337);
    auto U = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(g); };
    for (int i = 0; i < 56; i++) {
        float x = U(-0.1f, 1.1f) * w, yy = U(0.0f, 0.5f), rx = U(0.06f, 0.2f) * w, ry = rx * U(0.22f, 0.42f);
        UINT32 col = yy > 0.34f ? 0xF4C997 : 0xC8D3E4;
        float a = U(0.08f, 0.24f);
        R->FillEllipse(D2D1::Ellipse(P2(x, yy * h), rx, ry), RG(P2(x, yy * h), rx, ry, {{0, C(col, a)}, {0.6f, C(col, a * 0.5f)}, {1, C(col, 0)}}).Get());
    }
    MountainLayer(w, h, 11, 0.68f, 0.17f, 0.56f, 0, 0x6A7D9A, 0x45536D, 0xF4C590, 0.30f, 0.30f);
    MountainLayer(w, h, 23, 0.82f, 0.26f, 0.52f, 0, 0x4A4F67, 0x22273A, 0xEA9C5E, 0.45f, 0.40f);
    MountainLayer(w, h, 5, 1.04f, 0.58f, 0.50f, 0.24f, 0x3A3546, 0x10131D, 0xF0A262, 0.62f, 0.65f);
    for (int i = 0; i < 30; i++) {
        float x = U(0.40f, 1.12f) * w, yy = U(0.80f, 1.04f) * h, rx = U(0.08f, 0.2f) * w, ry = rx * U(0.18f, 0.32f);
        float a = U(0.18f, 0.38f);
        R->FillEllipse(D2D1::Ellipse(P2(x, yy), rx, ry), RG(P2(x, yy), rx, ry, {{0, C(0xE4EAF2, a)}, {1, C(0xE4EAF2, 0)}}).Get());
    }
    R->FillRectangle(RC(0, 0, w, h), RG(P2(w / 2, h / 2), w * 0.75f, h * 0.75f, {{0, C(0x050B16, 0)}, {0.7f, C(0x050B16, 0)}, {1, C(0x050B16, 0.5f)}}).Get());
}
// Draws the scene (src == nullptr) or a scaled copy of `src` into a backdrop target.
static void RenderInto(ID2D1BitmapRenderTarget* t, ID2D1Bitmap* src) {
    D2D1_SIZE_F z = t->GetSize();
    t->BeginDraw();
    if (src) t->DrawBitmap(src, RC(0, 0, z.width, z.height), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    else { ID2D1RenderTarget* old = R; R = t; PaintScene(z.width, z.height); R = old; }
    t->EndDraw();
}
// The backdrop and its frosted copy. The targets are kept between frames, so a video background only
// redraws into them: the full-size frame, then a progressive downsample whose bilinear upsample is the blur.
static void EnsureBackground() {
    D2D1_SIZE_U sz = rt->GetPixelSize();
    if (!bgRT[0] || sz.width != bgW || sz.height != bgH) {
        bgW = sz.width; bgH = sz.height;
        float w = (float)std::max(1u, sz.width), h = (float)std::max(1u, sz.height);
        const float div[6] = {1, 4, 12, 24, 10, 4};
        for (auto& t : bgRT) t.Reset();
        bgFull.Reset(); bgBlur.Reset(); blurBrush.Reset(); sharpBrush.Reset();
        for (int i = 0; i < 6; i++)
            if (FAILED(rt->CreateCompatibleRenderTarget(D2D1::SizeF(std::max(1.f, floorf(w / div[i])), std::max(1.f, floorf(h / div[i]))), &bgRT[i]))) { for (auto& t : bgRT) t.Reset(); return; }
        bgRT[0]->GetBitmap(&bgFull);
        bgRT[5]->GetBitmap(&bgBlur);
        rt->CreateBitmapBrush(bgFull.Get(), D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR), &sharpBrush);
        rt->CreateBitmapBrush(bgBlur.Get(), D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR), &blurBrush);
        bgDirty = true;
    }
    if (!bgDirty) return;
    bgDirty = false;
    RenderInto(bgRT[0].Get(), nullptr);
    for (int i = 1; i < 6; i++) {
        ComPtr<ID2D1Bitmap> prev;
        bgRT[i - 1]->GetBitmap(&prev);
        RenderInto(bgRT[i].Get(), prev.Get());
    }
}
static ComPtr<IWICFormatConverter> LoadWic(const std::wstring& file) {
    if (!wic || file.empty()) return {};
    ComPtr<IWICBitmapDecoder> d;
    if (FAILED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &d))) return {};
    ComPtr<IWICBitmapFrameDecode> f;
    if (FAILED(d->GetFrame(0, &f))) return {};
    ComPtr<IWICFormatConverter> c;
    wic->CreateFormatConverter(&c);
    if (!c || FAILED(c->Initialize(f.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut))) return {};
    return c;
}
static void UpdateFx();
// Starts or stops the background video to match the settings. Returns why a video cannot be played, or "".
static std::wstring SyncVideo() {
    std::wstring f = BackgroundFile();
    if (PresetIndex(cfg.bgImage) && f.empty()) { video.Close(); videoBmp.Reset(); return L"The built-in video could not be unpacked."; }
    if (!IsVideoFile(f)) { if (video.Active()) video.Close(); videoBmp.Reset(); return {}; }
    if (video.Active() && Lower(video.file) == Lower(f)) return {};
    videoBmp.Reset();
    std::wstring why = video.Open(f);
    UpdateFx();
    return why;
}
static void LoadImages() {
    bgImageBmp.Reset(); signatureBmp.Reset(); videoBmp.Reset();
    if (!rt) return;
    if (!PresetIndex(cfg.bgImage) && !IsVideoFile(cfg.bgImage)) if (auto c = LoadWic(cfg.bgImage)) rt->CreateBitmapFromWicBitmap(c.Get(), &bgImageBmp);
    for (auto& dir : {g_exeDir, g_appDir})
        if (!signatureBmp) if (auto c = LoadWic(Join(dir, L"signature.png"))) rt->CreateBitmapFromWicBitmap(c.Get(), &signatureBmp);
    bgDirty = true;
}
static HICON MakeIcon(int px) {
    ComPtr<IWICBitmap> bmp;
    if (!wic || FAILED(wic->CreateBitmap(px, px, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) return nullptr;
    ComPtr<ID2D1RenderTarget> t;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(d2f->CreateWicBitmapRenderTarget(bmp.Get(), props, &t))) return nullptr;
    ID2D1RenderTarget* old = R;
    R = t.Get();
    t->BeginDraw();
    t->Clear(C(0, 0));
    DrawLogo(px / 2.f, px / 2.f, px * 0.42f);
    t->EndDraw();
    R = old;
    std::vector<BYTE> pix((size_t)px * px * 4);
    bmp->CopyPixels(nullptr, px * 4, (UINT)pix.size(), pix.data());
    for (size_t i = 0; i < pix.size(); i += 4) {
        BYTE a = pix[i + 3];
        if (a && a < 255) for (int k = 0; k < 3; k++) pix[i + k] = (BYTE)std::min(255, pix[i + k] * 255 / a);
    }
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi); bi.bV5Width = px; bi.bV5Height = -px; bi.bV5Planes = 1; bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS; bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00; bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    memcpy(bits, pix.data(), pix.size());
    std::vector<BYTE> maskBits((size_t)((px + 15) / 16 * 2) * px, 0);
    HBITMAP mask = CreateBitmap(px, px, 1, 1, maskBits.data());
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return ic;
}

// ---------------------------------------------------------------------------------------------
// Scheduled backups: a Windows Task Scheduler task, so they also run while the app is closed
// ---------------------------------------------------------------------------------------------
static const wchar_t* TASK_NAME = L"Entropy Backup";
static const CLSID CLSID_TaskSched = {0x0f87369f, 0xa4e5, 0x4cfc, {0xbd, 0x3e, 0x73, 0xe6, 0x15, 0x45, 0x72, 0xdd}};
static const wchar_t* DAY_NAMES[7] = {L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday"};
struct TaskInfo { bool exists = false, enabled = false; std::wstring summary, next, last; LONG lastResult = 0; };
static TaskInfo taskInfo;

struct Bstr {
    BSTR b;
    explicit Bstr(const std::wstring& s) : b(SysAllocString(s.c_str())) {}
    ~Bstr() { SysFreeString(b); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    operator BSTR() const { return b; }
};
// Quotes one argument so CommandLineToArgvW reads it back unchanged (paths never contain quotes).
static std::wstring QuoteArg(const std::wstring& a) {
    std::wstring q = L"\"" + a;
    for (size_t i = a.size(); i > 0 && a[i - 1] == L'\\'; i--) q += L'\\';
    return q + L"\"";
}
static std::wstring XmlEsc(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) o += c == L'&' ? L"&amp;" : c == L'<' ? L"&lt;" : c == L'>' ? L"&gt;" : c == L'"' ? L"&quot;" : std::wstring(1, c);
    return o;
}
static std::wstring HexCode(LONG v) { wchar_t b[16]; swprintf_s(b, L"0x%08lX", (unsigned long)v); return b; }
static std::wstring ScheduleText() {
    wchar_t t[8];
    swprintf_s(t, L"%02d:%02d", cfg.schedHour, cfg.schedMin);
    if (cfg.schedMode == 1) return std::wstring(L"Every hour, starting at ") + t;
    if (cfg.schedMode == 3) return std::wstring(L"Every ") + DAY_NAMES[cfg.schedDay] + L" at " + t;
    return std::wstring(L"Every day at ") + t;
}
static std::wstring TaskResultText(LONG r) {
    switch (r) {
    case 0: return L"succeeded";
    case 1: return L"finished with warnings";
    case 2: return L"failed";
    case 3: return L"invalid arguments";
    case 4: return L"cancelled";
    case 0x41301: return L"running now";  // SCHED_S_TASK_RUNNING
    default: return L"result " + HexCode(r);
    }
}
static ComPtr<ITaskFolder> TaskRoot() {
    ComPtr<ITaskService> ts;
    ComPtr<ITaskFolder> f;
    VARIANT none{};
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskSched, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ts))) && SUCCEEDED(ts->Connect(none, none, none, none)))
        ts->GetFolder(Bstr(L"\\"), &f);
    return f;
}
static std::wstring OleDate(DATE d) {
    SYSTEMTIME t;
    if (!VariantTimeToSystemTime(d, &t) || t.wYear < 2000) return {};  // "never" is reported as 1999
    std::wstring date, time;
    FormatId(IdFromTime(t), date, time);
    return date + L" " + time;
}
static void RefreshTask() {
    taskInfo = {};
    auto f = TaskRoot();
    ComPtr<IRegisteredTask> t;
    if (!f || FAILED(f->GetTask(Bstr(TASK_NAME), &t))) return;
    taskInfo.exists = true;
    taskInfo.summary = IniGet(L"ScheduleSummary", L"A backup is scheduled.");
    VARIANT_BOOL en = VARIANT_FALSE;
    DATE d = 0;
    t->get_Enabled(&en);
    taskInfo.enabled = en != VARIANT_FALSE;
    if (SUCCEEDED(t->get_NextRunTime(&d))) taskInfo.next = OleDate(d);
    if (SUCCEEDED(t->get_LastTaskResult(&taskInfo.lastResult)) && taskInfo.lastResult != 0x41303 && SUCCEEDED(t->get_LastRunTime(&d)))  // 0x41303: has not run
        taskInfo.last = OleDate(d);
}
// Registers, updates or removes the task to match the Automation page. Returns a message for the page.
static std::wstring ApplySchedule() {
    auto f = TaskRoot();
    if (!f) return L"Task Scheduler is not available.";
    if (cfg.schedMode == 0) {
        HRESULT hr = f->DeleteTask(Bstr(TASK_NAME), 0);
        return SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ? L"Scheduled backups are off." : L"Could not remove the schedule (" + HexCode(hr) + L").";
    }
    if (cfg.from.empty() || cfg.to.empty()) return L"Set From and To on the Backup page first.";
    std::wstring from = NormPath(cfg.from), to = cfg.to;
    wchar_t exe[MAX_PATH], start[32];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf_s(start, L"%04d-%02d-%02dT%02d:%02d:00", t.wYear, t.wMonth, t.wDay, cfg.schedHour, cfg.schedMin);
    std::wstring trig = cfg.schedMode == 1
        ? L"<TimeTrigger><Repetition><Interval>PT1H</Interval></Repetition><StartBoundary>" + std::wstring(start) + L"</StartBoundary></TimeTrigger>"
        : L"<CalendarTrigger><StartBoundary>" + std::wstring(start) + L"</StartBoundary>" +
              (cfg.schedMode == 2 ? std::wstring(L"<ScheduleByDay><DaysInterval>1</DaysInterval></ScheduleByDay>")
                                  : L"<ScheduleByWeek><DaysOfWeek><" + std::wstring(DAY_NAMES[cfg.schedDay]) + L" /></DaysOfWeek><WeeksInterval>1</WeeksInterval></ScheduleByWeek>") +
              L"</CalendarTrigger>";
    std::wstring fire = FireboxSet() ? NormPath(cfg.fire) : L"";
    if (!fire.empty() && !FireboxConflict(to, fire).empty()) return FireboxConflict(to, fire);
    std::wstring args = L"backup --scheduled --from " + QuoteArg(from) + L" --to " + QuoteArg(to) + (fire.empty() ? L" --no-firebox" : L" --firebox " + QuoteArg(fire));
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?><Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">"
        L"<RegistrationInfo><Author>Entropy Backup</Author><Description>" +
        XmlEsc(L"Backs up " + from + L" to " + to + (fire.empty() ? L"" : L" and to the Firebox " + fire) + L". Other options come from the Entropy Backup settings.") +
        L"</Description></RegistrationInfo><Triggers>" + trig + L"</Triggers>"
        L"<Principals><Principal id=\"Author\"><LogonType>InteractiveToken</LogonType><RunLevel>" + (cfg.shadow ? L"HighestAvailable" : L"LeastPrivilege") +
        L"</RunLevel></Principal></Principals>"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><StartWhenAvailable>true</StartWhenAvailable>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>7</Priority></Settings>"
        L"<Actions Context=\"Author\"><Exec><Command>" + XmlEsc(exe) + L"</Command><Arguments>" + XmlEsc(args) + L"</Arguments></Exec></Actions></Task>";
    ComPtr<IRegisteredTask> reg;
    VARIANT none{};
    HRESULT hr = f->RegisterTask(Bstr(TASK_NAME), Bstr(xml), TASK_CREATE_OR_UPDATE, none, none, TASK_LOGON_INTERACTIVE_TOKEN, none, &reg);
    if (hr == E_ACCESSDENIED && cfg.shadow)
        return L"Saving a schedule that uses shadow copies needs administrator rights. Run the app as administrator once, or turn off shadow copy.";
    if (FAILED(hr)) return L"Could not save the schedule (" + HexCode(hr) + L").";
    IniSet(L"ScheduleSummary", ScheduleText() + L":  " + from + L"  →  " + to + (fire.empty() ? L"" : L"  +  Firebox " + fire));
    return L"Schedule saved.";
}
static std::wstring CliCommand(const std::wstring& from, const std::wstring& to, const std::wstring& fire) {
    std::wstring exe = Join(g_exeDir, L"EntropyBackupCLI.exe");
    if (!Exists(exe)) {
        wchar_t e[MAX_PATH];
        GetModuleFileNameW(nullptr, e, MAX_PATH);
        exe = e;
    }
    return QuoteArg(exe) + L" backup --from " + QuoteArg(from.empty() ? L"<project folder>" : from) + L" --to " + QuoteArg(to.empty() ? L"<backup folder>" : to) +
           (fire.empty() ? L"" : L" --firebox " + QuoteArg(fire));
}

// ---------------------------------------------------------------------------------------------
// UI state, hit regions and widgets
// ---------------------------------------------------------------------------------------------
enum Page { PG_BACKUP, PG_HISTORY, PG_RESTORE, PG_AUTO, PG_SETTINGS, PG_EXCLUDES, PG_ABOUT, PG_COUNT };
enum Id {
    ID_NONE = 0, ID_NAV = 1,
    ID_BROWSE_FROM = 20, ID_BROWSE_TO, ID_RADIO_INC, ID_RADIO_FULL, ID_CHK_LINK, ID_CHK_PRESERVE, ID_CHK_VERSIONED, ID_CHK_OPEN,
    ID_BACKUP, ID_CANCEL, ID_HIST_MENU, ID_OPEN_VERSIONS, ID_H_OPEN, ID_H_RESTORE, ID_R_BROWSE, ID_R_OVERWRITE, ID_R_GO, ID_R_EXPLORE,
    ID_S_REMEMBER, ID_S_HIDDEN, ID_S_BG_PICK, ID_S_BG_RESET, ID_S_BANNER, ID_S_TINT, ID_S_FROST, ID_S_LOOK_RESET, ID_S_OPENDIR, ID_X_DEFAULTS, ID_X_SAVE, ID_A_DATA,
    ID_CAP_MIN, ID_CAP_MAX, ID_CAP_CLOSE, ID_S_OUTLINE, ID_S_COLA, ID_S_COLB, ID_S_SWAP, ID_S_SPAN, ID_S_OWIDTH, ID_S_OGLOW,
    ID_H_VERIFY, ID_AU_APPLY, ID_AU_VSS, ID_AU_SUMS, ID_AU_VERIFY, ID_AU_COPY, ID_CHK_FIRE, ID_BROWSE_FIRE, ID_VIEW_PRIMARY, ID_VIEW_FIRE,
    ID_S_GLOWANIM, ID_S_GSPEED, ID_S_CW, ID_S_CCW,
    ID_S_STYLE = 900, ID_AU_FREQ = 920, ID_AU_DAY = 930, ID_S_FX = 940, ID_S_BGPRE = 950, ID_CARD = 1000, ID_LIST = 3000
};
enum { IDC_FROM = 101, IDC_TO, IDC_DESC, IDC_RTARGET, IDC_EXCL, IDC_TIME, IDC_KEEP, IDC_DAYS, IDC_FIRE };
static int page = PG_BACKUP, hoverId = 0, pressId = 0, dragId = 0;
static HWND editFrom{}, editTo{}, editDesc{}, editRestore{}, editExcl{}, editTime{}, editKeep{}, editDays{}, editFire{};
static std::wstring fireNote;  // warning under the Firebox field (same drive as the backup folder, or an invalid pair)
static HFONT editFont{};
static int editFontH = 0;
static HBRUSH fieldBrush{};
static bool trackingMouse = false;
static std::wstring flash;  // short-lived confirmation text on Settings / Excludes
struct Hot { D2D1_RECT_F px; int id; };
static std::vector<Hot> hots;
static D2D1_RECT_F zoneCards{}, zoneList{};

// Two layouts: default (no banner, pages shifted up to align with Version History) and the
// optional header-banner layout (Settings) where pages sit under the banner and the lower row is compact.
static float PageDy() { return cfg.banner ? 10.f : -82.f; }
static D2D1_RECT_F PagePanel() { return cfg.banner ? RC(334, 144, 1110, 746) : RC(334, 140, 1110, 768); }
static float pageDy = 0;
static D2D1_RECT_F ToPx(D2D1_RECT_F d) { return RC(gox + d.left * gs, goy + (d.top + pageDy) * gs, gox + d.right * gs, goy + (d.bottom + pageDy) * gs); }
static void AddHot(int id, D2D1_RECT_F d) { hots.push_back({ToPx(d), id}); }
static bool In(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
static int HitId(float x, float y) {
    for (auto it = hots.rbegin(); it != hots.rend(); ++it) if (In(it->px, x, y)) return it->id;
    return 0;
}
static void ComputeFit(float cw, float ch) {
    gs = std::max(0.1f, std::min(cw / DW, ch / DH));
    gox = (cw - DW * gs) / 2;
    goy = (ch - DH * gs) / 2;
}

static std::wstring EditText(HWND e) {
    std::vector<wchar_t> b((size_t)GetWindowTextLengthW(e) + 1);
    GetWindowTextW(e, b.data(), (int)b.size());
    return b.data();
}
enum { FX_PULSE = 1, FX_FLOW = 2, FX_SHIMMER = 4, FX_DRIFT = 8 };
static const std::chrono::steady_clock::time_point g_fx0 = std::chrono::steady_clock::now();
static double FxSeconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_fx0).count(); }
static D2D1_COLOR_F Mix(D2D1_COLOR_F x, D2D1_COLOR_F y, float k) { return D2D1::ColorF(x.r + (y.r - x.r) * k, x.g + (y.g - x.g) * k, x.b + (y.b - x.b) * k, x.a + (y.a - x.a) * k); }
static void Glass(D2D1_RECT_F r, float rad, bool gold) {
    for (int i = 1; i <= 5; i++) {
        float e = i * 2.4f;
        FillRR(RC(r.left - e * 0.5f, r.top - e * 0.2f + 2, r.right + e * 0.5f, r.bottom + e), rad + e, SB(C(0x02060D, 0.05f)).Get());
    }
    float frost = cfg.frost / 100.f, t = cfg.tint / 55.f;
    auto A = [&](float a) { return std::min(1.f, a * t); };
    if (sharpBrush && frost < 0.999f) FillRR(r, rad, sharpBrush.Get());
    if (blurBrush && frost > 0.001f) { blurBrush->SetOpacity(frost); FillRR(r, rad, blurBrush.Get()); }
    FillRR(r, rad, LG(P2(r.left, r.top), P2(r.right, r.bottom), {{0, C(0x2C4D76, A(0.40f))}, {0.55f, C(0x162B48, A(0.50f))}, {1, C(0x0C1A2E, A(0.60f))}}).Get());
    FillRR(r, rad, LG(P2(0, r.top), P2(0, r.top + std::min(150.f, (r.bottom - r.top) * 0.5f)), {{0, C(0xFFFFFF, 0.08f)}, {1, C(0xFFFFFF, 0)}}).Get());
    if (cfg.outline) {
        float w = 0.6f + cfg.outWidth / 100.f * 5.4f, glow = cfg.outGlow / 100.f;
        // Gradient geometry: either this section's box or the whole window, so one gradient flows across all sections.
        D2D1_RECT_F b = cfg.outSpan ? RC(0, -pageDy, DW, DH - pageDy) : r;
        D2D1_COLOR_F a = C(cfg.outA), z = C(cfg.outB);
        // Animated glow: slow, calm effects (the work matters more than the show). Periods are 4.5 to 12 s at
        // the default speed; the Speed slider scales them between 0.25x and 2x.
        int fx = cfg.glowAnim ? cfg.glowFx : 0;
        float T = (float)FxSeconds() * (0.25f + cfg.glowSpeed / 100.f * 1.75f);
        auto wave = [&](float period) { return 0.5f - 0.5f * cosf(T * 2 * PI / period); };  // 0..1, starts at 0
        if (fx & FX_DRIFT) { float k = wave(12); D2D1_COLOR_F a0 = a; a = Mix(a, z, k); z = Mix(z, a0, k); }
        float pulse = fx & FX_PULSE ? 1 - 0.65f * wave(4.5f) : 1.f;
        float mx = (b.left + b.right) / 2, my = (b.top + b.bottom) / 2;
        ComPtr<ID2D1Brush> br;  // Flow is drawn once around the window edge (DrawEdgeFlow), not per section
        switch (cfg.outStyle) {
        case 0: br = LG(P2(b.left, my), P2(b.right, my), {{0, a}, {1, z}}); break;                       // horizontal
        case 1: br = LG(P2(mx, b.top), P2(mx, b.bottom), {{0, a}, {1, z}}); break;                       // vertical
        case 2: br = LG(P2(b.left, b.top), P2(b.right, b.bottom), {{0, a}, {1, z}}); break;              // diagonal
        case 3: br = LG(P2(b.left, b.bottom), P2(b.right, b.top), {{0, a}, {1, z}}); break;              // reverse diagonal
        case 4: br = LG(P2(b.left, b.top), P2(b.right, b.bottom), {{0, a}, {0.5f, z}, {1, a}}); break;   // mirror A-B-A
        default: {                                                                                       // radial glow from centre
            float rr = sqrtf((b.right - b.left) * (b.right - b.left) + (b.bottom - b.top) * (b.bottom - b.top)) / 2;
            br = RG(P2(mx, my), rr, rr, {{0, a}, {0.55f, a}, {1, z}});
        }
        }
        if (glow > 0)
            for (int i = 3; i >= 1; i--) {
                br->SetOpacity(0.10f * glow * pulse * (4 - i) / 3.f);
                StrokeRR(r, rad, br.Get(), w + glow * pulse * 5.f * i);
            }
        br->SetOpacity(glow > 0 ? 1.f : 0.6f + 0.4f * pulse);
        StrokeRR(r, rad, br.Get(), w);
        if (fx & FX_SHIMMER) {  // a soft light travels slowly around each section's border
            float pw = r.right - r.left, ph = r.bottom - r.top, per = 2 * (pw + ph);
            float u = fmodf(T / 9.f + (r.left + r.top) / (DW + DH) * 0.5f, 1.f) * per;
            D2D1_POINT_2F q = u < pw ? P2(r.left + u, r.top) : u < pw + ph ? P2(r.right, r.top + u - pw)
                            : u < 2 * pw + ph ? P2(r.right - (u - pw - ph), r.bottom) : P2(r.left, r.bottom - (u - 2 * pw - ph));
            D2D1_COLOR_F hi = Mix(Mix(a, z, 0.5f), C(0xFFFFFF), 0.55f), hz = hi;
            hz.a = 0;
            auto sh = RG(q, 110, 110, {{0, hi}, {1, hz}});
            sh->SetOpacity(0.35f + 0.4f * glow);
            StrokeRR(r, rad, sh.Get(), w + 2.5f + glow * 6.f);
            sh->SetOpacity(0.9f);
            StrokeRR(r, rad, sh.Get(), w + 0.6f);
        }
    } else if (gold) {
        StrokeRR(r, rad, SB(C(0xF2CF7A, 0.10f)).Get(), 6);
        StrokeRR(r, rad, LG(P2(r.left, r.top), P2(r.right, r.bottom), {{0, C(0xF8DE98, 0.95f)}, {0.5f, C(0xD6B05A, 0.50f)}, {1, C(0xF0CC78, 0.85f)}}).Get(), 1.5f);
    } else {
        StrokeRR(r, rad, LG(P2(r.left, r.top), P2(r.right, r.bottom), {{0, C(0xD6E8F7, 0.40f)}, {0.6f, C(0x9CBEDC, 0.15f)}, {1, C(0xBFD6EA, 0.28f)}}).Get(), 1.2f);
    }
}
enum BtnStyle { BS_SECOND, BS_PRIMARY, BS_OUTLINE, BS_GHOST };
static void Button(int id, D2D1_RECT_F r, const std::wstring& label, int style, int ic = IC_NONE, bool chevron = false, bool enabled = true, float fs = 16) {
    bool hv = enabled && hoverId == id, pr = hv && pressId == id;
    float rad = style == BS_PRIMARY ? 14.f : 10.f;
    D2D1_COLOR_F tc = C(0xF4F7FB, enabled ? 1.f : 0.4f);
    if (pr) r = RC(r.left + 1, r.top + 1, r.right - 1, r.bottom - 1);
    if (style == BS_PRIMARY) {
        if (enabled) for (int i = 3; i >= 1; i--) StrokeRR(Inflate(r, i * 2.5f), rad + i * 2.5f, SB(C(0xF2CF7A, (hv ? 0.11f : 0.07f))).Get(), 2.5f);
        if (enabled) FillRR(r, rad, LG(P2(0, r.top), P2(0, r.bottom), {{0, C(hv ? 0xFAE3A6 : 0xF2D68E, 0.96f)}, {0.5f, C(hv ? 0xE6C26A : 0xD9B158, 0.94f)}, {1, C(0xB98E3C, 0.94f)}}).Get());
        else FillRR(r, rad, LG(P2(0, r.top), P2(0, r.bottom), {{0, C(0x8A8470, 0.6f)}, {1, C(0x5E5A4C, 0.6f)}}).Get());
        FillRR(RC(r.left + 2, r.top + 2, r.right - 2, (r.top + r.bottom) / 2), rad - 2, LG(P2(0, r.top), P2(0, (r.top + r.bottom) / 2), {{0, C(0xFFFFFF, 0.28f)}, {1, C(0xFFFFFF, 0.02f)}}).Get());
        StrokeRR(r, rad, SB(C(0xFFEBB0, enabled ? 0.95f : 0.35f)).Get(), 1.4f);
        tc = C(0x14213A, enabled ? 1.f : 0.6f);
    } else if (style == BS_SECOND) {
        FillRR(r, rad, SB(C(hv ? 0x2A4E76 : 0x1B3556, 0.85f)).Get());
        StrokeRR(r, rad, SB(C(0xE9C46E, enabled ? (hv ? 0.95f : 0.7f) : 0.25f)).Get(), 1.2f);
    } else if (style == BS_OUTLINE) {
        if (enabled) StrokeRR(r, rad, SB(C(0xF2CF7A, hv ? 0.22f : 0.12f)).Get(), 7);
        FillRR(r, rad, LG(P2(0, r.top), P2(0, r.bottom), {{0, C(hv ? 0x22406A : 0x172F52, 0.9f)}, {1, C(0x0E1E38, 0.9f)}}).Get());
        StrokeRR(r, rad, SB(C(0xF2D07E, enabled ? 0.95f : 0.3f)).Get(), 1.6f);
    } else {
        FillRR(r, rad, SB(C(0xFFFFFF, hv ? 0.14f : 0.06f)).Get());
        StrokeRR(r, rad, SB(C(0xFFFFFF, enabled ? 0.28f : 0.12f)).Get(), 1.1f);
    }
    float tw = TW(label, fs, SEMI), is = fs * 1.45f, iw = ic ? is + 10 : 0, cyy = (r.top + r.bottom) / 2;
    float x0 = (r.left + r.right) / 2 - (tw + iw) / 2 - (chevron ? 8 : 0);
    if (ic) Icon(ic, x0 + is / 2, cyy, is, ic == IC_DB ? C(0, 0) : (style == BS_PRIMARY ? tc : C(0xF2D07E, enabled ? 1.f : 0.4f)));
    Text(label, RC(x0 + iw, r.top, x0 + iw + tw + 4, r.bottom), fs, tc, SEMI);
    if (chevron) Icon(IC_CHEVRON, r.right - 26, cyy, fs * 1.15f, tc);
    if (enabled) AddHot(id, r);
}
static void Check(int id, float x, float y, const std::wstring& label, bool on, bool enabled = true, float fs = 15) {
    auto b = RC(x, y - 9, x + 18, y + 9);
    float a = enabled ? 1.f : 0.4f;
    if (on) {
        FillRR(b, 4, LG(P2(0, b.top), P2(0, b.bottom), {{0, C(0xF8DE98, a)}, {1, C(0xD2A84E, a)}}).Get());
        Icon(IC_CHECK, x + 9, y, 15, C(0x14213A, a));
    } else {
        FillRR(b, 4, SB(C(0x0E1C30, 0.6f * a)).Get());
        StrokeRR(b, 4, SB(C(0xC9D8E8, 0.55f * a)).Get(), 1.3f);
    }
    if (hoverId == id && enabled) StrokeRR(Inflate(b, 2.5f), 6, SB(C(0xF2CF7A, 0.55f)).Get(), 1);
    float tw = TW(label, fs);
    Text(label, RC(x + 28, y - 12, x + 30 + tw, y + 12), fs, C(0xDCE4EE, a));
    if (enabled) AddHot(id, RC(x - 4, y - 13, x + 32 + tw, y + 13));
}
static void Slider(int id, float x, float y, float w, const std::wstring& label, int val, bool en = true) {
    if (!en) R->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 0.4f), nullptr);
    Text(label, RC(362, y - 13, x - 12, y + 13), 15, C(0xDCE4EE));
    float k = val / 100.f, kx = x + w * k;
    FillRR(RC(x, y - 3, x + w, y + 3), 3, SB(C(0xFFFFFF, 0.14f)).Get());
    if (k > 0) FillRR(RC(x, y - 3, kx, y + 3), 3, LG(P2(x, 0), P2(x + w, 0), {{0, C(0xFBE3A0)}, {1, C(0xD9A944)}}).Get());
    if (hoverId == id || pressId == id) R->FillEllipse(D2D1::Ellipse(P2(kx, y), 15, 15), SB(C(0xF2CF7A, 0.15f)).Get());
    R->FillEllipse(D2D1::Ellipse(P2(kx, y), 9, 9), RG(P2(kx - 2, y - 2), 10, 10, {{0, C(0xFFF1C8)}, {1, C(0xD6AA4C)}}).Get());
    R->DrawEllipse(D2D1::Ellipse(P2(kx, y), 9, 9), SB(C(0xFFF4D6, 0.9f)).Get(), 1.2f);
    Text(std::to_wstring(val) + L"%", RC(x + w + 18, y - 13, x + w + 80, y + 13), 15, C(0xEBC56C), SEMI);
    if (!en) { R->PopLayer(); return; }
    AddHot(id, RC(x - 10, y - 15, x + w + 10, y + 15));
}
static void Radio(int id, float x, float y, const std::wstring& label, const std::wstring& sub, bool on) {
    float cx = x + 10;
    if (hoverId == id) R->FillEllipse(D2D1::Ellipse(P2(cx, y), 15, 15), SB(C(0xF2CF7A, 0.10f)).Get());
    R->DrawEllipse(D2D1::Ellipse(P2(cx, y), 10, 10), SB(on ? C(0xF2D07E) : C(0xC9D8E8, 0.7f)).Get(), 1.6f);
    if (on) R->FillEllipse(D2D1::Ellipse(P2(cx, y), 5.5f, 5.5f), RG(P2(cx - 1, y - 1), 6, 6, {{0, C(0xFFEDB8)}, {1, C(0xD6AA4C)}}).Get());
    float tw = TW(label, 17), sw = TW(sub, 16);
    Text(label, RC(x + 32, y - 14, x + 34 + tw, y + 14), 17, C(0xF0F4F8));
    Text(sub, RC(x + 42 + tw, y - 14, x + 44 + tw + sw, y + 14), 16, C(0xAFC0D2));
    AddHot(id, RC(x - 6, y - 16, x + 46 + tw + sw, y + 16));
}
static void Field(D2D1_RECT_F r, HWND e) {
    bool f = GetFocus() == e;
    FillRR(r, 9, SB(C(FIELD_HEX)).Get());
    StrokeRR(r, 9, SB(f ? C(0xF0CF7C, 0.9f) : C(0xB4D0EA, 0.30f)).Get(), f ? 1.5f : 1.1f);
}
static void LabelPair(const std::wstring& a, const std::wstring& b, float x, float y) {
    float w = TW(a, 18, SEMI);
    Text(a, RC(x, y - 14, x + w + 2, y + 14), 18, C(0xF4F7FA), SEMI);
    Text(b, RC(x + w + 10, y - 13, x + w + 300, y + 13), 15, C(0x9CB0C5));
}
static void ScrollBar(D2D1_RECT_F area, int first, int visible, int total) {
    if (total <= visible) return;
    float h = area.bottom - area.top, th = std::max(28.f, h * visible / total), ty = area.top + (h - th) * first / std::max(1, total - visible);
    FillRR(RC(area.right + 5, area.top, area.right + 8, area.bottom), 1.5f, SB(C(0xFFFFFF, 0.06f)).Get());
    FillRR(RC(area.right + 5, ty, area.right + 8, ty + th), 1.5f, SB(C(0xF2D07E, 0.6f)).Get());
}
// Snapshot card list shared by the right panel and the History / Restore pages.
static void SnapCards(D2D1_RECT_F area, int& scroll, int idBase, float ih, bool compact) {
    float gap = 8;
    int n = (int)snaps.size(), vis = std::max(1, (int)((area.bottom - area.top + gap) / (ih + gap)));
    scroll = std::clamp(scroll, 0, std::max(0, n - vis));
    R->PushAxisAlignedClip(Inflate(area, 2), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (int i = scroll; i < n; i++) {
        float y = area.top + (i - scroll) * (ih + gap);
        if (y > area.bottom) break;
        auto r = RC(area.left, y, area.right, y + ih);
        bool sel = i == selSnap, hv = hoverId == idBase + i;
        if (sel) {
            StrokeRR(r, 12, SB(C(0xF2CF7A, 0.12f)).Get(), 6);
            FillRR(r, 12, LG(P2(r.left, 0), P2(r.right, 0), {{0, C(0xE9C46E, 0.26f)}, {1, C(0xB08A3A, 0.10f)}}).Get());
            StrokeRR(r, 12, SB(C(0xF4D584, 0.95f)).Get(), 1.6f);
        } else {
            FillRR(r, 12, SB(C(hv ? 0x2A4A70 : 0x1B3352, hv ? 0.70f : 0.55f)).Get());
            StrokeRR(r, 12, SB(C(0x9CC3E2, hv ? 0.4f : 0.22f)).Get(), 1.1f);
        }
        const Snap& s = snaps[i];
        float tx = r.left + 58;
        Icon(IC_DB, r.left + 30, y + ih / 2, 30, C(0, 0));
        float dw = TW(s.date, 15, SEMI);
        Text(s.date, RC(tx, y + 8, tx + dw + 2, y + 32), 15, C(0xF6F8FB), SEMI);
        Text(s.time + (s.mirror ? L"  \u00B7  Mirror" : L""), RC(tx + dw + 14, y + 8, r.right - 36, y + 32), 15, C(0xD0DAE6));
        std::wstring d = s.desc.empty() ? (s.created.empty() && s.files == 0 ? L"Legacy snapshot" : L"No description") : s.desc;
        Text(d, RC(tx, y + 32, r.right - 38, y + ih - 6), compact ? 13.5f : 14, C(0xC3D0DE, s.desc.empty() ? 0.6f : 1.f), REG, LEFT, true, VTOP);
        Icon(IC_CHEVRON, r.right - 22, y + ih / 2, 18, C(0xE6EDF5, 0.85f));
        float vt = std::max(y, area.top), vb = std::min(y + ih, area.bottom);
        if (vb > vt) AddHot(idBase + i, RC(r.left, vt, r.right, vb));
    }
    R->PopAxisAlignedClip();
    ScrollBar(area, scroll, vis, n);
}

// ---------------------------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------------------------
static const wchar_t* NAV[PG_COUNT] = {L"Backup", L"History", L"Restore", L"Automation", L"Settings", L"Excludes", L"About"};
static const int NAV_ICON[PG_COUNT] = {IC_HOME, IC_HISTORY, IC_RESTORE, IC_CALENDAR, IC_GEAR, IC_EXCLUDE, IC_INFO};

static void DrawSidebar() {
    Glass(RC(14, 58, 318, 936), 24, false);
    DrawLogo(160, 162, 86);
    std::wstring t = L"Entropy Backup";
    ComPtr<IDWriteTextLayout> L;
    dwf->CreateTextLayout(t.c_str(), (UINT32)t.size(), Fmt(36, BOLD, CENTER, false, VMID), 304, 50, &L);
    if (L) {
        auto gold = SB(C(0xEBC56C));
        L->SetDrawingEffect(gold.Get(), DWRITE_TEXT_RANGE{8, 6});
        R->DrawTextLayout(P2(14, 256), L.Get(), SB(C(0xF4F7FB)).Get());
    }
    Text(L"Your Code. Always Within Reach.", RC(14, 304, 318, 330), 16, C(0xAAB9CC), REG, CENTER);
    for (int i = 0; i < PG_COUNT; i++) {
        float y = 352 + i * 56.f;
        auto r = RC(32, y, 300, y + 50);
        bool act = page == i, hv = hoverId == ID_NAV + i;
        if (act) {
            StrokeRR(r, 12, SB(C(0xF2CF7A, 0.12f)).Get(), 6);
            FillRR(r, 12, LG(P2(r.left, 0), P2(r.right, 0), {{0, C(0xF3D488, 0.62f)}, {0.6f, C(0xD8B35E, 0.36f)}, {1, C(0xB89045, 0.20f)}}).Get());
            StrokeRR(r, 12, SB(C(0xF6DA94, 0.95f)).Get(), 1.4f);
        } else if (hv) FillRR(r, 12, SB(C(0xFFFFFF, 0.07f)).Get());
        Icon(NAV_ICON[i], 72, y + 25, 26, act ? C(0xFFF4D6) : C(0xC9D4E1));
        Text(NAV[i], RC(108, y, 290, y + 50), 19, act ? C(0xFFFFFF) : C(0xD4DDE8), act ? SEMI : REG);
        AddHot(ID_NAV + i, r);
    }
    Text(L"Developed by", RC(44, 862, 240, 882), 14, C(0xA0B1C4));
    Text(L"Peter V. Odintsov", RC(44, 884, 260, 910), 17, C(0xF3F6FA));
    if (signatureBmp) {
        D2D1_SIZE_F s = signatureBmp->GetSize();
        float k = std::min(110 / s.width, 80 / s.height);
        R->DrawBitmap(signatureBmp.Get(), RC(300 - s.width * k, 918 - s.height * k, 300, 918));
    }
}
static void DrawHeader() {
    struct H { const wchar_t* t; const wchar_t* s; int ic; };
    static const H hs[PG_COUNT] = {{L"Project Backup", L"Incremental. Versioned. Simple.", IC_CUBE},
                            {L"Version History", L"Every snapshot, ready when you need it.", IC_HISTORY},
                            {L"Restore", L"Bring any version back in one step.", IC_RESTORE},
                            {L"Automation", L"Schedules, retention and integrity checks.", IC_CALENDAR},
                            {L"Settings", L"Tune how Entropy Backup works.", IC_GEAR},
                            {L"Excludes", L"Keep build output and clutter out of snapshots.", IC_EXCLUDE},
                            {L"About", L"Entropy Backup for Windows 11.", IC_INFO}};
    Glass(RC(334, 58, 1110, 140), 22, false);
    Icon(hs[page].ic, 382, 99, 42, C(0xEBC56C));
    Text(hs[page].t, RC(426, 62, 860, 102), 30, C(0xF7F9FC), SEMI);
    Text(hs[page].s, RC(428, 100, 860, 126), 16, C(0xB5C5D5));
    auto box = RC(874, 70, 1094, 128);
    FillRR(box, 12, SB(C(0x0B172A, 0.45f)).Get());
    StrokeRR(box, 12, SB(C(0xC9D9EA, 0.22f)).Get(), 1);
    Text(L"Keep your projects safe.\nFocus on building.", RC(892, 72, 1086, 126), 14.5f, C(0xD5DEE8), REG, LEFT, true, VMID);
}
// Rows of the Backup page. Firebox mode adds a third folder row, so the rows tighten to make room for it.
struct BackupRows { float from, to, fire, desc, fieldH, descH; };
static BackupRows Rows() { return cfg.fireOn ? BackupRows{170, 254, 338, 422, 40, 44} : BackupRows{176, 281, 0, 386, 44, 70}; }
static const UINT32 FLAME = 0xFFA45C;
static void DrawBackupPage() {
    Glass(PagePanel(), 22, false);
    D2D1_COLOR_F gold = C(0xEBC56C);
    BackupRows w = Rows();
    auto folderRow = [&](float y, int ic, D2D1_COLOR_F icc, const wchar_t* label, const wchar_t* sub, HWND e, int browse) {
        Icon(ic, 380, y + 20 + w.fieldH / 2 - 2, 32, icc);
        LabelPair(label, sub, 422, y);
        Field(RC(422, y + 20, 938, y + 20 + w.fieldH), e);
        Button(browse, RC(952, y + 20, 1086, y + 20 + w.fieldH), L"Browse", BS_SECOND, IC_FOLDER);
    };
    folderRow(w.from, IC_FOLDER, gold, L"From", L"(Source Directory)", editFrom, ID_BROWSE_FROM);
    folderRow(w.to, IC_DRIVE, gold, L"To", L"(Backup Directory)", editTo, ID_BROWSE_TO);
    Check(ID_CHK_FIRE, 930, w.to, L"Firebox mode", cfg.fireOn, true, 15);
    if (cfg.fireOn) {
        folderRow(w.fire, IC_FLAME, C(FLAME), L"Firebox", L"(Second, independent backup)", editFire, ID_BROWSE_FIRE);
        if (!fireNote.empty()) Text(fireNote, RC(700, w.fire - 12, 1086, w.fire + 12), 13.5f, C(FLAME), SEMI, RIGHT);
    }
    Icon(IC_DOC, 380, w.desc + 20 + std::min(w.descH, 50.f) / 2, 32, gold);
    LabelPair(L"Description", L"(optional)", 422, w.desc);
    Field(RC(422, w.desc + 20, 1086, w.desc + 20 + w.descH), editDesc);
    Text(std::to_wstring(GetWindowTextLengthW(editDesc)) + L" / 500", RC(900, w.desc + 22 + w.descH, 1086, w.desc + 44 + w.descH), 13, C(0x9CB0C5), REG, RIGHT);

    auto box = RC(356, 512, 1088, 732);
    FillRR(box, 14, SB(C(0x081426, 0.32f)).Get());
    StrokeRR(box, 14, SB(C(0xAACBEA, 0.18f)).Get(), 1);
    Icon(IC_RESTORE, 386, 548, 22, C(0xD6E0EA));
    Text(L"Backup Type", RC(408, 534, 600, 562), 17, C(0xEEF2F6), SEMI);
    Radio(ID_RADIO_INC, 404, 600, L"Incremental", L"(faster)", !cfg.full);
    Radio(ID_RADIO_FULL, 404, 652, L"Full Copy", L"(complete snapshot)", cfg.full);
    Line(724, 536, 724, 712, C(0xAACBEA, 0.18f), 1);
    Icon(IC_GEAR, 756, 548, 22, C(0xD6E0EA));
    Text(L"Options", RC(778, 534, 960, 562), 17, C(0xEEF2F6), SEMI);
    Check(ID_CHK_LINK, 754, 592, L"Use hard links for unchanged files", cfg.hardLinks && !cfg.full, !cfg.full);
    Check(ID_CHK_PRESERVE, 754, 625, L"Preserve file attributes and timestamps", cfg.preserve);
    Check(ID_CHK_VERSIONED, 754, 658, L"Create versioned snapshots", cfg.versioned);
    Check(ID_CHK_OPEN, 754, 691, L"Open destination after completion", cfg.openAfter);
}
static void DetailRow(float& y, const std::wstring& label, const std::wstring& val, int lines = 1) {
    Text(label, RC(720, y, 840, y + 22), 14, C(0x93A7BD));
    Text(val.empty() ? L"\u2014" : val, RC(846, y, 1088, y + 21.f * lines), 14.5f, C(0xE8EEF5), REG, LEFT, lines > 1, VTOP);
    y += 21.f * lines + 9;
}
static void EmptyList(D2D1_RECT_F area) {
    bool none = ViewDest().empty();
    Text(none ? L"No backup directory selected." : L"No snapshots yet.", RC(area.left, area.top + 6, area.right, area.top + 34), 17, C(0xD6E0EA), SEMI);
    Text(none ? L"Choose a backup directory on the Backup page to see its versions."
              : viewFire ? L"The Firebox has no snapshots yet, or its drive is not connected."
                         : L"Run your first backup and it will appear here.",
         RC(area.left, area.top + 38, area.right, area.top + 90), 14.5f, C(0x9CB0C5), REG, LEFT, true, VTOP);
}
static void Chip(int id, D2D1_RECT_F r, const std::wstring& label, bool on, bool en);
// Primary | Firebox switch above the History and Restore lists. Returns the list area below it.
static D2D1_RECT_F ViewSwitch(D2D1_RECT_F list) {
    if (!FireboxSet()) return list;
    Chip(ID_VIEW_PRIMARY, RC(list.left, list.top - 4, list.left + 120, list.top + 26), L"Primary", !viewFire, true);
    Chip(ID_VIEW_FIRE, RC(list.left + 128, list.top - 4, list.left + 248, list.top + 26), L"Firebox", viewFire, true);
    Text(viewFire ? cfg.fire : cfg.to, RC(list.left + 258, list.top - 4, list.right, list.top + 26), 12.5f, C(0x9CB0C5));
    return RC(list.left, list.top + 40, list.right, list.bottom);
}
static void DrawHistoryPage() {
    Glass(PagePanel(), 22, false);
    auto list = ViewSwitch(RC(356, 164, 690, 734));
    zoneList = ToPx(list);
    if (snaps.empty()) { EmptyList(list); return; }
    SnapCards(list, listScroll, ID_LIST, 72, false);
    Line(704, 164, 704, 734, C(0xAACBEA, 0.15f), 1);
    const Snap& s = snaps[std::clamp(selSnap, 0, (int)snaps.size() - 1)];
    Text(s.date + L"   " + s.time, RC(720, 160, 1088, 192), 22, C(0xF6F8FB), SEMI);
    Text(s.mirror ? L"Mirror (latest only)" : L"Snapshot " + s.id, RC(720, 192, 1088, 214), 14, C(0xEBC56C));
    float y = 232;
    DetailRow(y, L"Description", s.desc, 3);
    DetailRow(y, L"Source", s.source, 2);
    DetailRow(y, L"Type", s.type.empty() ? L"" : (s.type == L"full" ? L"Full copy" : L"Incremental") + std::wstring(s.shadow ? L"  \u00B7  shadow copy" : L"") +
                              std::wstring(s.firebox ? L"  \u00B7  Firebox" : L""));
    DetailRow(y, L"Files", s.files ? Num(s.files) : L"");
    DetailRow(y, L"Copied", s.created.empty() ? L"" : Num(s.copied));
    DetailRow(y, L"Unchanged", s.created.empty() ? L"" : Num(s.reused) + L" (linked)");
    DetailRow(y, L"Data written", s.created.empty() ? L"" : Bytes(s.bytes) + (s.bytesTotal ? L" of " + Bytes(s.bytesTotal) : L""));
    DetailRow(y, L"Duration", s.seconds > 0 ? Dur(s.seconds) : L"");
    if (s.failed) DetailRow(y, L"Skipped", Num(s.failed) + L" unreadable files");
    DetailRow(y, L"Checksums", !s.sums ? L"Not recorded"
                               : !s.verified ? L"Recorded, not verified yet"
                               : s.vBad ? Num(s.vBad) + L" problems found " + s.vWhen
                               : L"Verified " + s.vWhen);
    Button(ID_H_OPEN, RC(720, 626, 898, 672), L"Open Folder", BS_SECOND, IC_FOLDEROPEN, false, true, 15);
    Button(ID_H_VERIFY, RC(910, 626, 1088, 672), L"Verify", BS_SECOND, IC_CHECK, false, s.sums && !Running(), 15);
    Button(ID_H_RESTORE, RC(720, 684, 1088, 734), L"Restore This Version", BS_PRIMARY, IC_RESTORE, true, !Running(), 17);
}
static void DrawRestorePage() {
    Glass(PagePanel(), 22, false);
    auto list = ViewSwitch(RC(356, 164, 690, 734));
    zoneList = ToPx(list);
    if (snaps.empty()) EmptyList(list);
    else SnapCards(list, listScroll, ID_LIST, 72, false);
    Line(704, 164, 704, 734, C(0xAACBEA, 0.15f), 1);
    Text(L"Selected version", RC(720, 160, 1088, 182), 14, C(0x93A7BD));
    if (!snaps.empty()) {
        const Snap& s = snaps[std::clamp(selSnap, 0, (int)snaps.size() - 1)];
        Text(s.date + L"   " + s.time, RC(720, 184, 1088, 212), 20, C(0xF6F8FB), SEMI);
        Text(s.desc.empty() ? L"No description" : s.desc, RC(720, 214, 1088, 258), 14.5f, C(0xC3D0DE), REG, LEFT, true, VTOP);
    } else Text(L"None", RC(720, 184, 1088, 212), 20, C(0x9CB0C5), SEMI);
    Icon(IC_FOLDER, 732, 292, 24, C(0xEBC56C));
    LabelPair(L"Restore to", L"(Target Directory)", 752, 292);
    Field(RC(720, 314, 1088, 358), editRestore);
    Button(ID_R_EXPLORE, RC(720, 370, 898, 412), L"Explore Version", BS_GHOST, IC_FOLDEROPEN, false, !snaps.empty(), 15);
    Button(ID_R_BROWSE, RC(910, 370, 1088, 412), L"Browse", BS_SECOND, IC_FOLDER, false, true, 15);
    Check(ID_R_OVERWRITE, 722, 448, L"Overwrite existing files", cfg.overwrite);
    Text(cfg.overwrite ? L"Files that exist in the target are replaced with the version from this snapshot."
                       : L"Files that already exist in the target are left untouched. Restore into an empty folder for an exact copy.",
         RC(750, 466, 1088, 530), 13.5f, C(0x9CB0C5), REG, LEFT, true, VTOP);
    Text(L"Restoring never modifies the snapshot itself \u2014 every version stays intact.", RC(720, 560, 1088, 610), 13.5f, C(0x9CB0C5), REG, LEFT, true, VTOP);
    Button(ID_R_GO, RC(720, 684, 1088, 734), Running() ? L"Working..." : L"Restore Now", BS_PRIMARY, IC_RESTORE, true, !Running() && !snaps.empty(), 18);
}
// Pill-shaped option button; one of a row is `on`.
static void Chip(int id, D2D1_RECT_F r, const std::wstring& label, bool on, bool en) {
    bool hv = en && hoverId == id;
    float al = en ? 1.f : 0.4f;
    if (on) FillRR(r, 15, LG(P2(0, r.top), P2(0, r.bottom), {{0, C(0xF6DA94, 0.85f * al)}, {1, C(0xC99B45, 0.85f * al)}}).Get());
    else FillRR(r, 15, SB(C(0xFFFFFF, (hv ? 0.14f : 0.06f) * al)).Get());
    StrokeRR(r, 15, SB(C(on ? 0xFFE9B0 : 0xFFFFFF, (on ? 0.9f : 0.25f) * al)).Get(), 1.1f);
    Text(label, r, 13.5f, on ? C(0x14213A, al) : C(0xE6EDF5, al), SEMI, CENTER);
    if (en) AddHot(id, r);
}
static void Swatch(int id, float x, float y, UINT32 col, bool en) {
    auto r = RC(x, y - 15, x + 62, y + 15);
    float al = en ? 1.f : 0.4f;
    if (en && hoverId == id) StrokeRR(Inflate(r, 3), 10, SB(C(0xF2CF7A, 0.6f)).Get(), 1.2f);
    FillRR(r, 8, SB(C(col, al)).Get());
    FillRR(RC(r.left, r.top, r.right, y), 8, LG(P2(0, r.top), P2(0, y), {{0, C(0xFFFFFF, 0.25f * al)}, {1, C(0xFFFFFF, 0)}}).Get());
    StrokeRR(r, 8, SB(C(0xFFFFFF, 0.5f * al)).Get(), 1.2f);
    if (en) AddHot(id, r);
}
static void DrawAutomationPage() {
    Glass(PagePanel(), 22, false);
    D2D1_COLOR_F gold = C(0xEBC56C), dim = C(0x9CB0C5), lc = C(0xDCE4EE), sep = C(0xAACBEA, 0.15f);
    Text(L"Scheduled Backups", RC(360, 156, 800, 186), 18, gold, SEMI);
    if (!flash.empty()) Text(flash, RC(560, 156, 1088, 186), 14, gold, REG, RIGHT);
    Text(L"Run", RC(362, 196, 520, 226), 15, lc);
    static const wchar_t* freq[4] = {L"Off", L"Hourly", L"Daily", L"Weekly"};
    for (int i = 0; i < 4; i++) Chip(ID_AU_FREQ + i, RC(520 + i * 95.f, 196, 609 + i * 95.f, 226), freq[i], cfg.schedMode == i, true);
    bool on = cfg.schedMode != 0;
    Text(cfg.schedMode == 1 ? L"Starting at" : L"At", RC(362, 234, 520, 268), 15, C(0xDCE4EE, on ? 1.f : 0.4f));
    Field(RC(520, 234, 609, 268), editTime);
    static const wchar_t* days[7] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
    for (int i = 0; i < 7; i++) Chip(ID_AU_DAY + i, RC(625 + i * 66.f, 236, 685 + i * 66.f, 266), days[i], cfg.schedDay == i, cfg.schedMode == 3);
    std::wstring st1 = L"No backup is scheduled.", st2;
    if (taskInfo.exists) {
        st1 = taskInfo.summary + (taskInfo.enabled ? L"" : L"  (disabled in Task Scheduler)");
        st2 = L"Next run: " + (taskInfo.next.empty() ? std::wstring(L"—") : taskInfo.next) + L"     Last run: " +
              (taskInfo.last.empty() ? std::wstring(L"never") : taskInfo.last + L" (" + TaskResultText(taskInfo.lastResult) + L")");
    }
    Text(st1, RC(362, 276, 1088, 298), 14.5f, C(0xE8EEF5));
    Text(st2, RC(362, 298, 1088, 320), 13.5f, dim);
    Button(ID_AU_APPLY, RC(360, 328, 560, 366), L"Save Schedule", BS_SECOND, IC_CHECK, false, true, 15);
    Text(L"Runs in the background while you are signed in, even with the app closed. A missed run starts as soon as possible.",
         RC(576, 328, 1088, 366), 13, dim, REG, LEFT, true, VMID);
    Line(360, 378, 1088, 378, sep, 1);

    Text(L"Retention", RC(360, 386, 800, 416), 18, gold, SEMI);
    Text(L"Keep the newest", RC(362, 422, 520, 456), 15, lc);
    Field(RC(520, 422, 609, 456), editKeep);
    Text(L"snapshots   (0 = keep all)", RC(622, 422, 1088, 456), 14.5f, dim);
    Text(L"Delete after", RC(362, 460, 520, 494), 15, lc);
    Field(RC(520, 460, 609, 494), editDays);
    Text(L"days   (0 = never; the newest snapshot always stays)", RC(622, 460, 1088, 494), 14.5f, dim);
    Line(360, 504, 1088, 504, sep, 1);

    Text(L"Integrity", RC(360, 512, 800, 542), 18, gold, SEMI);
    Check(ID_AU_VSS, 362, 560, L"Copy open and locked files with a Volume Shadow Copy  (needs administrator)", cfg.shadow);
    Check(ID_AU_SUMS, 362, 590, L"Record a SHA-256 checksum for every file", cfg.checksums);
    Check(ID_AU_VERIFY, 362, 620, L"Verify each new snapshot against its checksums", cfg.verifyAfter && cfg.checksums, cfg.checksums);
    Line(360, 640, 1088, 640, sep, 1);

    Text(L"Command Line", RC(360, 648, 700, 678), 18, gold, SEMI);
    Button(ID_AU_COPY, RC(938, 646, 1088, 678), L"Copy", BS_GHOST, IC_DOC, false, true, 14);
    Text(CliCommand(EditText(editFrom), NormPath(EditText(editTo)), cfg.fireOn ? NormPath(EditText(editFire)) : L""), RC(362, 684, 1088, 734), 13, C(0xC8D4E2), REG, LEFT, true, VTOP);
}
static void DrawSettingsPage() {
    Glass(PagePanel(), 22, false);
    D2D1_COLOR_F gold = C(0xEBC56C), sep = C(0xAACBEA, 0.15f);
    Text(L"General", RC(360, 152, 800, 182), 18, gold, SEMI);
    if (!flash.empty()) Text(flash, RC(620, 152, 1088, 182), 14, gold, REG, RIGHT);
    Check(ID_S_REMEMBER, 362, 194, L"Remember last used folders", cfg.remember, true, 16);
    Check(ID_S_HIDDEN, 362, 224, L"Skip hidden and system files when backing up  (the versions folder is always skipped)", cfg.skipHidden, true, 16);
    Line(360, 244, 1088, 244, sep, 1);

    Text(L"Appearance", RC(360, 250, 800, 280), 18, gold, SEMI);
    Slider(ID_S_TINT, 520, 294, 380, L"Glass tint", cfg.tint);
    Slider(ID_S_FROST, 520, 324, 380, L"Frost (blur)", cfg.frost);
    Check(ID_S_BANNER, 362, 354, L"Show page header banner  (alternate layout for A/B testing)", cfg.banner, true, 16);
    // Built-in backgrounds (the mountain scene and three looping videos), or a custom image or video
    Text(L"Background", RC(362, 372, 520, 398), 15, C(0xDCE4EE));
    int pre = PresetIndex(cfg.bgImage);
    static const wchar_t* bgs[4] = {L"Mountain Dusk", L"Nebula Lattice", L"Tidal Fractal", L"Sacred Geometry"};
    for (int i = 0; i < 4; i++) Chip(ID_S_BGPRE + i, RC(520 + i * 142.f, 370, 656 + i * 142.f, 398), bgs[i], i == 0 ? cfg.bgImage.empty() : pre == i, true);
    Button(ID_S_BG_PICK, RC(360, 406, 588, 438), L"Choose Image or Video...", BS_SECOND, IC_FOLDER, false, true, 15);
    std::wstring bgName = cfg.bgImage.empty() || pre ? (pre && !video.Active() ? L"This video cannot be played; showing the mountain scene." : L"")
                        : cfg.bgImage.substr(cfg.bgImage.find_last_of(L"\\/") + 1) + (!IsVideoFile(cfg.bgImage) ? L""
                           : video.Active() ? L"  \u00B7  looping video" : L"  \u00B7  cannot be played");
    Text(bgName, RC(600, 406, 816, 438), 13.5f, C(0xE8EEF5));
    Button(ID_S_LOOK_RESET, RC(824, 406, 1088, 438), L"Reset Glass Look", BS_GHOST, IC_RESTORE, false, cfg.tint != 55 || cfg.frost != 100, 15);
    Line(360, 446, 1088, 446, sep, 1);

    Text(L"Section Outline", RC(360, 452, 800, 482), 18, gold, SEMI);
    Check(ID_S_OUTLINE, 362, 496, L"Outline all sections with a two-colour gradient", cfg.outline, true, 16);
    bool en = cfg.outline;
    D2D1_COLOR_F lc = C(0xDCE4EE, en ? 1.f : 0.4f);
    Text(L"Colours", RC(362, 516, 520, 544), 15, lc);
    Swatch(ID_S_COLA, 520, 530, cfg.outA, en);
    Button(ID_S_SWAP, RC(592, 516, 652, 544), L"\u21C4", BS_GHOST, IC_NONE, false, en, 16);
    Swatch(ID_S_COLB, 662, 530, cfg.outB, en);
    auto prev = RC(744, 518, 900, 542);  // live preview of the gradient
    FillRR(prev, 12, LG(P2(prev.left, 0), P2(prev.right, 0), cfg.outStyle == 4 ? std::initializer_list<D2D1_GRADIENT_STOP>{{0, C(cfg.outA)}, {0.5f, C(cfg.outB)}, {1, C(cfg.outA)}}
                                                                                : std::initializer_list<D2D1_GRADIENT_STOP>{{0, C(cfg.outA)}, {1, C(cfg.outB)}}).Get());
    if (!en) FillRR(prev, 12, SB(C(0x0C1A2E, 0.6f)).Get());
    Check(ID_S_SPAN, 920, 530, L"Flow across window", cfg.outSpan, en, 14);
    Text(L"Gradient", RC(362, 552, 520, 580), 15, lc);
    static const wchar_t* styles[6] = {L"Horizontal", L"Vertical", L"Diagonal", L"Reverse", L"Mirror", L"Radial"};
    for (int i = 0; i < 6; i++) Chip(ID_S_STYLE + i, RC(520 + i * 95.f, 552, 609 + i * 95.f, 580), styles[i], cfg.outStyle == i, en);
    Slider(ID_S_OWIDTH, 520, 604, 380, L"Thickness", cfg.outWidth, en);
    Slider(ID_S_OGLOW, 520, 634, 380, L"Glow", cfg.outGlow, en);
    // Animated glow: any mix of calm effects, plus their speed
    bool an = en && cfg.glowAnim;
    Check(ID_S_GLOWANIM, 362, 668, L"Animate", cfg.glowAnim, en, 15);
    static const wchar_t* fxNames[4] = {L"Pulse", L"Flow", L"Shimmer", L"Drift"};
    for (int i = 0; i < 4; i++) Chip(ID_S_FX + i, RC(520 + i * 95.f, 654, 609 + i * 95.f, 682), fxNames[i], (cfg.glowFx >> i) & 1, an);
    bool fl = an && (cfg.glowFx & FX_FLOW);  // Flow travels the window edge: pick its resting direction
    Chip(ID_S_CW, RC(904, 654, 994, 682), L"\u21BB CW", cfg.flowDir == 0, fl);
    Chip(ID_S_CCW, RC(998, 654, 1088, 682), L"\u21BA CCW", cfg.flowDir == 1, fl);
    Slider(ID_S_GSPEED, 520, 708, 380, L"Speed", cfg.glowSpeed, an);
    if (fl) Text(L"Backup \u21BB   Restore \u21BA", RC(958, 696, 1090, 720), 12.f, C(0x9CB0C5), REG, RIGHT);
}
static void DrawExcludesPage() {
    Glass(PagePanel(), 22, false);
    Text(L"Files and folders matching these patterns are never copied into snapshots.", RC(360, 160, 1088, 186), 16, C(0xE8EEF5));
    Text(L"One pattern per line. A plain name such as node_modules matches at any depth; * and ? are wildcards; "
         L"patterns containing \\ match the path relative to the project (e.g. src\\generated\\*). Lines starting with # are comments.",
         RC(360, 190, 1088, 234), 13.5f, C(0x9CB0C5), REG, LEFT, true, VTOP);
    Field(RC(356, 242, 1088, 664), editExcl);
    std::vector<wchar_t> b((size_t)GetWindowTextLengthW(editExcl) + 1);
    GetWindowTextW(editExcl, b.data(), (int)b.size());
    size_t n = Patterns(b.data()).size();
    Button(ID_X_DEFAULTS, RC(356, 684, 560, 734), L"Reset to Defaults", BS_GHOST, IC_RESTORE, false, true, 15);
    Text(flash.empty() ? std::to_wstring(n) + (n == 1 ? L" active pattern" : L" active patterns") : flash, RC(570, 684, 874, 734), 15, flash.empty() ? C(0x9CB0C5) : C(0xEBC56C), REG, CENTER);
    Button(ID_X_SAVE, RC(884, 684, 1088, 734), L"Save Excludes", BS_PRIMARY, IC_CHECK, false, true, 16);
}
static void DrawAboutPage() {
    Glass(PagePanel(), 22, false);
    DrawLogo(420, 222, 50);
    Text(L"Entropy Backup", RC(492, 180, 1000, 222), 30, C(0xF7F9FC), SEMI);
    Text(std::wstring(L"Version ") + APP_VERSION + L"  \u00B7  Native Windows 11 x64  \u00B7  C++20, Direct2D, DirectWrite", RC(494, 222, 1088, 248), 15, C(0xEBC56C));
    Text(L"When Entropy is back UP, launch EntropyBackup!....exe", RC(360, 256, 1088, 282), 16, C(0xF3F6FA), SEMI);
    Text(L"Entropy Backup keeps immutable, timestamped snapshots of your projects. Each snapshot is a complete, browsable copy, "
         L"while files that did not change are hard-linked to the previous snapshot so they take no additional space.",
         RC(360, 294, 1088, 354), 15.5f, C(0xDCE4EE), REG, LEFT, true, VTOP);
    Text(L"How it works", RC(360, 366, 800, 394), 18, C(0xEBC56C), SEMI);
    const wchar_t* pts[] = {
        L"Each backup is written to a temporary folder and renamed to snapshot_YYYY-MM-DD_HH-MM-SS only once it is complete.",
        L"manifest.tsv records every file's size and timestamp so the next incremental backup recognises unchanged files instantly.",
        L"backup.json stores the description, source, counts and duration shown in Version History.",
        L"A snapshot's files folder is a plain copy of your project \u2014 copy it anywhere to restore by hand.",
    };
    float y = 402;
    for (auto p : pts) {
        R->FillEllipse(D2D1::Ellipse(P2(368, y + 11), 3, 3), SB(C(0xEBC56C)).Get());
        Text(p, RC(382, y, 1088, y + 44), 14.5f, C(0xC8D4E2), REG, LEFT, true, VTOP);
        y += 50;
    }
    Text(L"Provided \"AS IS\" under the MIT License, without warranty of any kind. You are solely responsible for your data and for verifying "
         L"your backups; the author is not liable for any data loss or damage. See DISCLAIMER.md.", RC(360, 600, 1088, 636), 12.5f, C(0x9CB0C5), REG, LEFT, true, VTOP);
    Text(L"Developed by Peter V. Odintsov", RC(360, 640, 800, 666), 16, C(0xF3F6FA), SEMI);
    Button(ID_A_DATA, RC(360, 684, 600, 734), L"Open Data Folder", BS_SECOND, IC_FOLDEROPEN, false, true, 15);
    Text(g_appDir, RC(614, 684, 1088, 734), 13.5f, C(0x9CB0C5), REG, RIGHT);
}
static void DrawProgress() {
    bool cb = cfg.banner;  // compact lower row in the banner layout
    Glass(RC(334, cb ? 770.f : 702.f, 1110, 936), 22, false);
    int ph = job.phase;
    bool run = ph >= 1 && ph <= 3, rest = job.kind == 1;
    double el = 0;
    std::wstring cur, msg;
    {
        std::lock_guard l(job.m);
        cur = job.current;
        msg = job.message;
        if (ph) el = std::chrono::duration<double>((run ? std::chrono::steady_clock::now() : job.t1) - job.t0).count();
    }
    uint64_t wt = job.workTotal, wd = job.workDone;
    double f = ph == 4 ? 1.0 : (ph == 2 || ph == 3) && wt ? std::min(1.0, (double)wd / wt) : 0.0;
    if (run && f > 0.995) f = 0.995;
    if (ph == 5 || ph == 6) f = wt ? std::min(1.0, (double)wd / wt) : 0.0;
    float cx = cb ? 408.f : 414.f, cy = cb ? 853.f : 819.f, rr = cb ? 44.f : 56.f, rw = cb ? 9.f : 10.f, ox = cb ? 474.f : 500.f;
    R->DrawEllipse(D2D1::Ellipse(P2(cx, cy), rr, rr), SB(C(0xFFFFFF, 0.10f)).Get(), rw);
    auto goldGrad = LG(P2(cx - rr, cy - rr), P2(cx + rr, cy + rr), {{0, C(0xFBE3A0)}, {1, C(0xD9A944)}});
    if (f > 0.002) {
        if (f >= 0.999) R->DrawEllipse(D2D1::Ellipse(P2(cx, cy), rr, rr), goldGrad.Get(), rw);
        else {
            float a1 = (float)(-90 + 360 * f);
            auto g = Geo([&](ID2D1GeometrySink* k) { ArcFig(k, cx, cy, rr, -90, a1); });
            R->DrawGeometry(g.Get(), goldGrad.Get(), rw, roundSt.Get());
        }
    } else if (ph == 1) {  // scanning: rotating comet
        float a = (float)fmod(el * 300.0, 360.0);
        auto g = Geo([&](ID2D1GeometrySink* k) { ArcFig(k, cx, cy, rr, a, a + 70); });
        R->DrawGeometry(g.Get(), goldGrad.Get(), rw, roundSt.Get());
    }
    std::wstring pc = std::to_wstring((int)(f * 100 + (ph == 4 ? 0.5 : 0)));
    float w1 = TW(pc, cb ? 26.f : 30.f, SEMI), w2 = TW(L"%", cb ? 14.f : 16.f, SEMI), px = cx - (w1 + w2) / 2;
    Text(pc, RC(px, cy - 22, px + w1 + 2, cy + 20), cb ? 26.f : 30.f, C(0xF7F9FC), SEMI);
    Text(L"%", RC(px + w1, cy - 6, px + w1 + w2 + 4, cy + 16), cb ? 14.f : 16.f, C(0xF7F9FC), SEMI);

    int kind = job.kind;  // 0 backup, 1 restore, 2 verify
    static const wchar_t* titles[3][7] = {
        {L"Ready to back up", L"Scanning project...", L"Backing up files...", L"Finalizing snapshot...", L"Backup complete", L"Backup failed", L"Backup cancelled"},
        {L"Ready to back up", L"Preparing restore...", L"Restoring files...", L"Finalizing...", L"Restore complete", L"Restore failed", L"Restore cancelled"},
        {L"Ready to back up", L"Preparing verification...", L"Verifying checksums...", L"Finalizing...", L"Verification passed", L"Verification failed", L"Verification cancelled"}};
    std::wstring title = ph == 4 && kind == 2 && job.warnings ? L"Verification found problems" : titles[kind][std::clamp(ph, 0, 6)];
    bool two = kind == 0 && job.legs == 2, partial = kind == 0 && ph == 4 && job.failedLegs;
    if (partial) title = L"Backup incomplete \u2014 one destination failed";
    else if (two && ph == 4) title = L"Backup complete \u00B7 both copies saved";
    else if (two && (ph == 2 || ph == 3)) title += job.leg == 1 ? L"  \u00B7  Firebox 2/2" : L"  \u00B7  Primary 1/2";
    bool bad = ph == 5 || partial || (ph == 4 && kind == 2 && job.warnings);
    Text(title, cb ? RC(ox, 782, 950, 810) : RC(ox, 726, 950, 758), cb ? 20.f : 22.f, bad ? C(0xFF9C8A) : C(0xF7F9FC), SEMI);
    std::wstring line = msg;
    if (ph == 0 && GetWindowTextLengthW(editFrom) && GetWindowTextLengthW(editTo) && msg == IDLE_MESSAGE)
        line = L"Folders are set. Press Back Up Now to create a snapshot.";
    if (ph == 1) line = cur.rfind(L"Creating", 0) == 0 ? cur : L"Scanning: " + cur;
    else if (ph == 2) line = kind == 2 ? cur : (rest ? L"Restoring: " : two && job.leg == 1 ? L"Copying to Firebox: " : L"Copying: ") + cur;
    else if (ph == 3) line = cur.empty() ? L"Writing manifest and metadata..." : cur;
    if (two && ph >= 4)  // two destinations: the result has a clause for each, so it may wrap onto a second line
        Text(line, cb ? RC(ox, 808, 950, 844) : RC(ox, 758, 950, 800), 13.5f, C(0xB5C5D5), REG, LEFT, true, VTOP);
    else Text(line, cb ? RC(ox, 810, 950, 836) : RC(ox, 760, 950, 786), 15.5f, C(0xB5C5D5));
    Button(ID_CANCEL, cb ? RC(962, 786, 1088, 828) : RC(962, 728, 1088, 770), L"Cancel", BS_GHOST, IC_STOP, false, run && !job.cancel, 15);

    auto bar = cb ? RC(ox, 846, 1088, 856) : RC(ox, 806, 1088, 816);
    FillRR(bar, 5, SB(C(0xFFFFFF, 0.12f)).Get());
    if (f > 0.002) FillRR(RC(bar.left, bar.top, bar.left + (bar.right - bar.left) * (float)f, bar.bottom), 5, LG(P2(bar.left, 0), P2(bar.right, 0), {{0, C(0xFBE3A0)}, {1, C(0xD9A944)}}).Get());

    const wchar_t* labels[6] = {kind == 2 ? L"Files Checked" : L"Files Scanned", kind == 2 ? L"Matched" : rest ? L"Files Restored" : L"Files Copied",
                                kind == 2 ? L"Problems" : rest ? L"Skipped (Existing)" : L"Unchanged (Linked)", kind == 2 ? L"Size Read" : L"Size Written",
                                L"Elapsed", L"Est. Remaining"};
    std::wstring eta = L"--:--:--";
    if (ph == 2 && f > 0.01) eta = Dur(el * (1 - f) / f);
    else if (ph == 4) eta = Dur(0);
    else if (ph >= 5) eta = L"—";
    std::wstring vals[6] = {Num(job.scanned), Num(job.copied), Num(rest ? job.skipped : job.linked), Bytes(job.bytesWritten), Dur(el), eta};
    float xs[7] = {ox, ox + 98, ox + 190, ox + 318, ox + 414, ox + 504, 1096};
    float ly = cb ? 864.f : 842.f, vy = cb ? 886.f : 866.f;
    for (int i = 0; i < 6; i++) {
        Text(labels[i], RC(xs[i], ly, xs[i + 1] - 4, ly + 20), 13, C(0x9CB0C5));
        Text(ph ? vals[i] : L"\u2014", RC(xs[i], vy, xs[i + 1] - 4, vy + 30), cb ? 17.f : 18.f, C(0xF2F5F9));
    }
}
static void DrawHistoryPanel() {
    float hb = cfg.banner ? 756.f : 686.f;
    Glass(RC(1126, 58, 1466, hb), 22, false);
    bool fv = viewFire && FireboxSet();
    Icon(fv ? IC_FLAME : IC_HISTHDR, 1160, 104, 26, fv ? C(FLAME) : C(0xD8E2EE));
    Text(fv ? L"Firebox History" : L"Version History", RC(1186, 88, 1400, 120), 20, C(0xF4F7FA), SEMI);
    auto dots = RC(1412, 88, 1450, 120);
    if (hoverId == ID_HIST_MENU) FillRR(dots, 8, SB(C(0xFFFFFF, 0.10f)).Get());
    Icon(IC_DOTS, 1431, 104, 22, C(0xE6EDF5));
    AddHot(ID_HIST_MENU, dots);
    auto area = RC(1142, 136, 1450, hb - 14);
    zoneCards = ToPx(area);
    if (snaps.empty()) {
        Text(ViewDest().empty() ? L"No backup directory selected." : L"No snapshots yet.", RC(1146, 140, 1450, 168), 16, C(0xA9BACC));
        return;
    }
    SnapCards(area, cardScroll, ID_CARD, 74, true);
}
static void DrawQuickBackupPanel() {
    bool cb = cfg.banner, run = Running();
    float top = cb ? 770.f : 702.f, ty = top + (cb ? 20.f : 30.f);
    Glass(RC(1126, top, 1466, 936), 22, false);
    Icon(IC_DB, 1162, ty + 16, 30, C(0, 0));
    Text(L"Back Up", RC(1194, ty, 1450, ty + 32), 20, C(0xF4F7FA), SEMI);
    wchar_t from[1024] = {}, to[1024] = {};
    GetWindowTextW(editFrom, from, 1024);
    GetWindowTextW(editTo, to, 1024);
    std::wstring f = from, name = f.substr(f.find_last_of(L"\\/") + 1);
    std::wstring sub = run ? L"A job is running \u2014 see the status panel."
                     : (f.empty() || !to[0]) ? L"Set From and To on the Backup page first."
                     : L"Snapshot \u201C" + (name.empty() ? f : name) + L"\u201D now.";
    Text(sub, RC(1146, ty + 38, 1446, ty + 62), 14.5f, C(0xB5C5D5));
    float by = cb ? 866.f : 860.f;
    static const wchar_t* busy[3] = {L"Backing Up...", L"Restoring...", L"Verifying..."};
    Button(ID_BACKUP, RC(1146, by, 1446, by + 52), run ? busy[std::clamp((int)job.kind, 0, 2)] : L"Back Up Now", BS_PRIMARY, IC_DB, !run, !run, 18);
}
// Flow: one soft comet of light travels around the edge of the window. It also says what the app is doing:
// clockwise while backing up, counter-clockwise while restoring, otherwise the direction chosen in Settings.
// A change of direction eases through a stop rather than jumping.
static double g_flowPos = 0, g_flowVel = 0;  // position along the edge (0..1, clockwise) and speed (loops per second)
static std::chrono::steady_clock::time_point g_flowT{};
static int FlowDirection() {
    if (Running() && job.kind == 0) return 1;
    if (Running() && job.kind == 1) return -1;
    return cfg.flowDir ? -1 : 1;
}
static void DrawEdgeFlow(float cw, float ch) {
    auto now = std::chrono::steady_clock::now();
    double dt = g_flowT.time_since_epoch().count() ? std::min(0.1, std::chrono::duration<double>(now - g_flowT).count()) : 0.0;
    g_flowT = now;
    if (!cfg.outline || !cfg.glowAnim || !(cfg.glowFx & FX_FLOW)) { g_flowVel = 0; return; }
    double speed = (0.25 + cfg.glowSpeed / 100.0 * 1.75) / 16.0, target = FlowDirection() * speed;  // one lap in 16 s at 1x
    if (g_flowVel == 0) g_flowVel = target * 1e-3;  // just switched on: ease in from a standstill
    g_flowVel += (target - g_flowVel) * std::min(1.0, dt * 1.6);
    g_flowPos = fmod(g_flowPos + g_flowVel * dt + 2.0, 1.0);
    bool maxed = IsZoomed(g_hwnd);
    float k = std::max(0.8f, gs), in = 1.f * k, rad = maxed ? 0.f : 8.f;
    auto edge = RC(in, in, cw - in, ch - in);
    float pw = edge.right - edge.left, ph = edge.bottom - edge.top, per = 2 * (pw + ph);
    auto at = [&](double u) {  // point on the edge, clockwise from the top-left corner
        float d = (float)(fmod(u + 2.0, 1.0) * per);
        return d < pw ? P2(edge.left + d, edge.top) : d < pw + ph ? P2(edge.right, edge.top + d - pw)
             : d < 2 * pw + ph ? P2(edge.right - (d - pw - ph), edge.bottom) : P2(edge.left, edge.bottom - (d - 2 * pw - ph));
    };
    float glow = cfg.outGlow / 100.f, move = (float)std::min(1.0, fabs(g_flowVel) / speed);
    int dir = g_flowVel >= 0 ? 1 : -1;
    const int N = 16;
    float tail = 0.11f * move;  // the tail shrinks to nothing while the comet turns around
    for (int i = N; i >= 0; i--) {
        float f = (float)i / N, fade = (1 - f) * (1 - f);
        D2D1_COLOR_F c = Mix(Mix(C(cfg.outA), C(cfg.outB), f), C(0xFFFFFF), 0.25f * (1 - f)), c0 = c;
        c0.a = 0;
        float r = (110 - 50 * f) * k;
        auto br = RG(at(g_flowPos - dir * f * tail), r, r, {{0, c}, {1, c0}});
        br->SetOpacity(fade * (0.18f + 0.30f * glow));
        StrokeRR(edge, rad, br.Get(), (5 + 12 * glow) * k);
        br->SetOpacity(fade * 0.95f);
        StrokeRR(edge, rad, br.Get(), 1.8f * k);
    }
}
static void DrawCaption(float cw) {
    float k = std::max(0.8f, gs), bw = 46 * k, bh = 34 * k, x = cw - 3 * bw;
    for (int i = 0; i < 3; i++) {
        auto r = RC(x + i * bw, 0, x + (i + 1) * bw, bh);
        int id = ID_CAP_MIN + i;
        if (hoverId == id) R->FillRectangle(r, SB(i == 2 ? C(0xC42B1C) : C(0xFFFFFF, 0.10f)).Get());
        float cx = (r.left + r.right) / 2, cy = bh / 2, g = 5 * k, lw = std::max(1.f, k);
        D2D1_COLOR_F c = C(0xE8EEF5);
        if (i == 0) Line(cx - g, cy, cx + g, cy, c, lw);
        else if (i == 1) {
            if (IsZoomed(g_hwnd)) {
                R->DrawRectangle(RC(cx - g + 2 * k, cy - g, cx + g, cy + g - 2 * k), SB(c).Get(), lw);
                R->FillRectangle(RC(cx - g, cy - g + 2 * k, cx + g - 2 * k, cy + g), SB(hoverId == id ? C(0x34465C) : C(0x101C2E)).Get());
                R->DrawRectangle(RC(cx - g, cy - g + 2 * k, cx + g - 2 * k, cy + g), SB(c).Get(), lw);
            } else R->DrawRectangle(RC(cx - g, cy - g, cx + g, cy + g), SB(c).Get(), lw);
        } else { Line(cx - g, cy - g, cx + g, cy + g, c, lw); Line(cx + g, cy - g, cx - g, cy + g, c, lw); }
        hots.push_back({r, id});
    }
}

static void SetDesignTransform(D2D1_SIZE_F z, float dy) {
    pageDy = dy;
    if (blurBrush)
        blurBrush->SetTransform(D2D1::Matrix3x2F::Scale(D2D1::SizeF(z.width / bgBlur->GetSize().width, z.height / bgBlur->GetSize().height)) *
                                D2D1::Matrix3x2F::Translation(-gox, -goy) * D2D1::Matrix3x2F::Scale(D2D1::SizeF(1 / gs, 1 / gs)) *
                                D2D1::Matrix3x2F::Translation(0, -dy));
    if (sharpBrush)
        sharpBrush->SetTransform(D2D1::Matrix3x2F::Translation(-gox, -goy) * D2D1::Matrix3x2F::Scale(D2D1::SizeF(1 / gs, 1 / gs)) * D2D1::Matrix3x2F::Translation(0, -dy));
    rt->SetTransform(D2D1::Matrix3x2F::Translation(0, dy) * D2D1::Matrix3x2F::Scale(D2D1::SizeF(gs, gs)) * D2D1::Matrix3x2F::Translation(gox, goy));
}
static bool CreateRT() {
    if (rt) return true;
    if (!d2f) return false;
    RECT r{};
    GetClientRect(g_hwnd, &r);
    if (r.right <= 0 || r.bottom <= 0) return false;
    HRESULT hr = d2f->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_IGNORE), 96, 96),
                                             D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU((UINT32)r.right, (UINT32)r.bottom)), &rt);
    if (FAILED(hr) || !rt) { LogRuntime(L"CreateHwndRenderTarget failed", hr); return false; }
    rt->SetDpi(96, 96);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    for (auto& t : bgRT) t.Reset();
    bgFull.Reset(); bgBlur.Reset(); blurBrush.Reset(); sharpBrush.Reset();
    LoadImages();
    return true;
}
static void Paint() {
    if (!CreateRT()) {
        HDC dc = GetDC(g_hwnd);
        RECT cr{};
        GetClientRect(g_hwnd, &cr);
        HBRUSH bg = CreateSolidBrush(RGB(7, 17, 31));
        FillRect(dc, &cr, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(246, 231, 176));
        TextOutW(dc, 36, 36, L"Entropy Backup - Direct2D unavailable (see runtime.log)", 55);
        ReleaseDC(g_hwnd, dc);
        return;
    }
    R = rt.Get();
    EnsureBackground();
    D2D1_SIZE_F z = rt->GetSize();
    ComputeFit(z.width, z.height);
    hots.clear();
    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    if (bgFull) rt->DrawBitmap(bgFull.Get(), RC(0, 0, z.width, z.height));
    else rt->Clear(C(0x0A1626));
    rt->FillRectangle(RC(0, 0, z.width, z.height), SB(C(0x06101F, 0.18f)).Get());
    SetDesignTransform(z, 0);
    zoneList = {};
    DrawSidebar();
    if (cfg.banner) DrawHeader();
    SetDesignTransform(z, PageDy());
    switch (page) {
    case PG_BACKUP: DrawBackupPage(); break;
    case PG_HISTORY: DrawHistoryPage(); break;
    case PG_RESTORE: DrawRestorePage(); break;
    case PG_AUTO: DrawAutomationPage(); break;
    case PG_SETTINGS: DrawSettingsPage(); break;
    case PG_EXCLUDES: DrawExcludesPage(); break;
    case PG_ABOUT: DrawAboutPage(); break;
    }
    SetDesignTransform(z, 0);
    DrawProgress();
    DrawHistoryPanel();
    DrawQuickBackupPanel();
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    DrawEdgeFlow(z.width, z.height);
    DrawCaption(z.width);
    HRESULT hr = rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        LogRuntime(L"EndDraw: recreate target", hr);
        for (auto& t : bgRT) t.Reset();
        rt.Reset(); bgFull.Reset(); bgBlur.Reset(); blurBrush.Reset(); sharpBrush.Reset(); bgImageBmp.Reset(); signatureBmp.Reset(); videoBmp.Reset();
    }
    else if (FAILED(hr)) LogRuntime(L"EndDraw failed", hr);
}

// ---------------------------------------------------------------------------------------------
// Child edit controls
// ---------------------------------------------------------------------------------------------
// Re-checks the Firebox against the backup folder; shown in the Firebox row.
static void CheckFirebox() {
    fireNote.clear();
    if (!cfg.fireOn || cfg.fire.empty() || cfg.to.empty()) return;
    fireNote = FireboxConflict(cfg.to, cfg.fire);
    if (fireNote.empty() && SameDrive(cfg.to, cfg.fire)) fireNote = L"\u26A0 Same drive as To \u2014 not a real second copy";
}
static void ReadEdits() {
    cfg.from = EditText(editFrom);
    cfg.to = NormPath(EditText(editTo));
    cfg.fire = NormPath(EditText(editFire));
    cfg.excludes = EditText(editExcl);
    cfg.keepCount = std::clamp(_wtoi(EditText(editKeep).c_str()), 0, 99999);
    cfg.keepDays = std::clamp(_wtoi(EditText(editDays).c_str()), 0, 99999);
}
// Reads HH:MM from the schedule time field; false (settings unchanged) if it is not a valid time.
static bool ReadTime() {
    int h = -1, m = -1;
    if (swscanf_s(EditText(editTime).c_str(), L"%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return false;
    cfg.schedHour = h;
    cfg.schedMin = m;
    return true;
}
static void PlaceEdit(HWND e, D2D1_RECT_F d) {
    auto p = ToPx(d);
    MoveWindow(e, (int)lroundf(p.left), (int)lroundf(p.top), (int)lroundf(p.right - p.left), (int)lroundf(p.bottom - p.top), TRUE);
}
static void ShowControls() {
    ShowWindow(editFrom, page == PG_BACKUP ? SW_SHOW : SW_HIDE);
    ShowWindow(editTo, page == PG_BACKUP ? SW_SHOW : SW_HIDE);
    ShowWindow(editDesc, page == PG_BACKUP ? SW_SHOW : SW_HIDE);
    ShowWindow(editFire, page == PG_BACKUP && cfg.fireOn ? SW_SHOW : SW_HIDE);
    ShowWindow(editRestore, page == PG_RESTORE ? SW_SHOW : SW_HIDE);
    ShowWindow(editExcl, page == PG_EXCLUDES ? SW_SHOW : SW_HIDE);
    for (HWND e : {editTime, editKeep, editDays}) ShowWindow(e, page == PG_AUTO ? SW_SHOW : SW_HIDE);
    EnableWindow(editTime, cfg.schedMode != 0);
}
static void LayoutControls() {
    RECT cr;
    GetClientRect(g_hwnd, &cr);
    if (cr.right <= 0 || cr.bottom <= 0) return;
    ComputeFit((float)cr.right, (float)cr.bottom);
    int fh = -(int)lroundf(16 * gs);
    if (fh != editFontH) {
        HFONT f = CreateFontW(fh, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        for (HWND e : {editFrom, editTo, editDesc, editRestore, editExcl, editTime, editKeep, editDays, editFire}) SendMessageW(e, WM_SETFONT, (WPARAM)f, TRUE);
        if (editFont) DeleteObject(editFont);
        editFont = f;
        editFontH = fh;
    }
    pageDy = PageDy();
    BackupRows w = Rows();
    float pad = (w.fieldH - 24) / 2;  // single-line fields: 24 units of text, centred in the field
    PlaceEdit(editFrom, RC(436, w.from + 21 + pad, 926, w.from + 45 + pad));
    PlaceEdit(editTo, RC(436, w.to + 21 + pad, 926, w.to + 45 + pad));
    PlaceEdit(editFire, RC(436, w.fire + 21 + pad, 926, w.fire + 45 + pad));
    PlaceEdit(editDesc, RC(436, w.desc + 30, 1074, w.desc + 12 + w.descH));
    PlaceEdit(editRestore, RC(732, 324, 1076, 348));
    PlaceEdit(editExcl, RC(368, 252, 1080, 656));
    PlaceEdit(editTime, RC(530, 240, 600, 262));
    PlaceEdit(editKeep, RC(530, 428, 600, 450));
    PlaceEdit(editDays, RC(530, 466, 600, 488));
    pageDy = 0;
    ShowControls();
}
static HWND MakeEdit(HWND parent, int id, DWORD style, const wchar_t* cue) {
    HWND e = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | style, 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SetWindowTheme(e, L"DarkMode_Explorer", nullptr);
    if (cue) SendMessageW(e, EM_SETCUEBANNER, TRUE, (LPARAM)cue);
    return e;
}

// ---------------------------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------------------------
static std::wstring PickFolder(HWND h, const std::wstring& start) {
    ComPtr<IFileDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return {};
    DWORD o{};
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    ComPtr<IShellItem> si;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&si)))) d->SetFolder(si.Get());
    if (FAILED(d->Show(h))) return {};
    ComPtr<IShellItem> i;
    if (FAILED(d->GetResult(&i))) return {};
    PWSTR p{};
    i->GetDisplayName(SIGDN_FILESYSPATH, &p);
    std::wstring s = p ? p : L"";
    CoTaskMemFree(p);
    return s;
}
static std::wstring PickImage(HWND h) {
    ComPtr<IFileDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return {};
    COMDLG_FILTERSPEC f[] = {{L"Images and videos", L"*.jpg;*.jpeg;*.png;*.bmp;*.webp;*.tif;*.tiff;*.mp4;*.m4v;*.mov;*.wmv;*.avi;*.mkv"},
                             {L"Images", L"*.jpg;*.jpeg;*.png;*.bmp;*.webp;*.tif;*.tiff"}, {L"Videos (looping, muted)", L"*.mp4;*.m4v;*.mov;*.wmv;*.avi;*.mkv"}, {L"All files", L"*.*"}};
    d->SetFileTypes(4, f);
    if (FAILED(d->Show(h))) return {};
    ComPtr<IShellItem> i;
    if (FAILED(d->GetResult(&i))) return {};
    PWSTR p{};
    i->GetDisplayName(SIGDN_FILESYSPATH, &p);
    std::wstring s = p ? p : L"";
    CoTaskMemFree(p);
    return s;
}
// Runs the effect timer only while the animated glow is visible, and pauses the video background while the
// window is minimized or a backup, restore or verification is reading and writing the disks.
static void UpdateFx() {
    if (!g_hwnd) return;
    bool shown = !IsIconic(g_hwnd);
    if (cfg.outline && cfg.glowAnim && shown) SetTimer(g_hwnd, T_FX, 33, nullptr);
    else KillTimer(g_hwnd, T_FX);
    if (video.Active()) video.Pause(!shown || Running());
}
static void OpenFolder(const std::wstring& p) { ShellExecuteW(g_hwnd, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
static void Notify(const std::wstring& m) {
    { std::lock_guard l(job.m); job.message = m; }
    if (job.phase >= 4) job.phase = 0;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}
static void ResetJob(int kind, const std::wstring& title) {
    if (worker.joinable()) worker.join();
    job.kind = kind;
    job.cancel = false;
    job.scanned = job.copied = job.linked = job.skipped = job.bytesWritten = job.workDone = job.workTotal = job.warnings = 0;
    job.legs = 1;
    job.leg = job.failedLegs = 0;
    {
        std::lock_guard l(job.m);
        job.t0 = job.t1 = std::chrono::steady_clock::now();
        job.current.clear();
        job.resultDir.clear();
        job.title = title;
    }
    job.phase = 1;
    if (g_hwnd) SetTimer(g_hwnd, T_ANIM, 50, nullptr);
    UpdateFx();
}
static void StartBackup() {
    ReadEdits();
    if (cfg.from.empty() || cfg.to.empty()) { Notify(L"Choose both From and To directories first."); return; }
    SaveSettings();
    if (cfg.fireOn && cfg.fire.empty()) { Notify(L"Choose a Firebox folder, or turn Firebox mode off."); return; }
    std::wstring fire = cfg.fireOn ? cfg.fire : L"";
    if (!fire.empty() && !FireboxConflict(cfg.to, fire).empty()) { Notify(FireboxConflict(cfg.to, fire)); return; }
    BackupParams p{cfg.from, cfg.to, EditText(editDesc), cfg.full, cfg.hardLinks, cfg.preserve, cfg.versioned, cfg.skipHidden, Patterns(cfg.excludes),
                   cfg.shadow, cfg.checksums, cfg.verifyAfter && cfg.checksums, cfg.keepCount, cfg.keepDays, fire};
    ResetJob(0, L"Backup " + NormPath(cfg.from) + L" -> " + cfg.to + (fire.empty() ? L"" : L" + Firebox " + fire));
    worker = std::thread(BackupWorker, p);
}
static void StartVerify() {
    if (snaps.empty()) return;
    const Snap& s = snaps[std::clamp(selSnap, 0, (int)snaps.size() - 1)];
    ResetJob(2, L"Verify " + s.dir);
    worker = std::thread(VerifyWorker, s.dir, nullptr);
}
static void CopyText(const std::wstring& t) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t))) {
        memcpy(GlobalLock(g), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}
static void StartRestore() {
    if (snaps.empty()) return;
    std::wstring target = NormPath(EditText(editRestore));
    if (target.empty()) { Notify(L"Choose a folder to restore into."); return; }
    const Snap& s = snaps[std::clamp(selSnap, 0, (int)snaps.size() - 1)];
    std::wstring q = L"Restore the version from " + s.date + L" " + s.time + L" into\n" + target + L"?";
    if (cfg.overwrite) q += L"\n\nExisting files with the same name will be overwritten.";
    if (MessageBoxW(g_hwnd, q.c_str(), L"Entropy Backup", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return;
    ResetJob(1, L"Restore " + s.dir + L" -> " + target);
    worker = std::thread(RestoreWorker, s.dir, target, cfg.overwrite);
}
static void GoPage(int p) {
    if (page == PG_EXCLUDES && p != PG_EXCLUDES) { cfg.excludes = EditText(editExcl); SaveSettings(); }
    if (page == PG_AUTO && p != PG_AUTO) { ReadEdits(); ReadTime(); SaveSettings(); }
    if (p == PG_AUTO) RefreshTask();
    page = p;
    flash.clear();
    ShowControls();
    SetFocus(g_hwnd);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}
static void HistoryMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, L"Refresh");
    AppendMenuW(m, MF_STRING, 2, L"Show All Versions");
    AppendMenuW(m, MF_STRING | (ViewDest().empty() ? MF_GRAYED : 0), 3, L"Open Versions Folder");
    if (FireboxSet()) AppendMenuW(m, MF_STRING, 4, viewFire ? L"Show Primary Versions" : L"Show Firebox Versions");
    auto r = ToPx(RC(1412, 88, 1450, 120));
    POINT pt{(LONG)r.left, (LONG)r.bottom};
    ClientToScreen(g_hwnd, &pt);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
    if (cmd == 1) { ReadEdits(); RefreshHistory(); }
    else if (cmd == 2) GoPage(PG_HISTORY);
    else if (cmd == 3) { std::wstring root = VersionsRoot(); MakeDirs(root); OpenFolder(root); }
    else if (cmd == 4) { viewFire = !viewFire; selSnap = cardScroll = listScroll = 0; RefreshHistory(); }
}
static void DragSlider(float x) {
    for (auto& hs : hots)
        if (hs.id == dragId) {
            float l = hs.px.left + 10 * gs, r = hs.px.right - 10 * gs;
            int v = std::clamp((int)lroundf((x - l) / std::max(1.f, r - l) * 100), 0, 100);
            int* target = dragId == ID_S_TINT ? &cfg.tint : dragId == ID_S_FROST ? &cfg.frost : dragId == ID_S_OWIDTH ? &cfg.outWidth
                        : dragId == ID_S_GSPEED ? &cfg.glowSpeed : &cfg.outGlow;
            *target = v;
            InvalidateRect(g_hwnd, nullptr, FALSE);
            return;
        }
}
static void PickColour(UINT32& col) {
    static COLORREF custom[16] = {RGB(0xF2, 0xCF, 0x7A), RGB(0x5C, 0xCB, 0xFF), RGB(0xFF, 0x6B, 0xD5), RGB(0x8A, 0x5C, 0xFF),
                                  RGB(0x4F, 0xE0, 0xB0), RGB(0xFF, 0x8A, 0x3D), RGB(0xFF, 0xFF, 0xFF), RGB(0x2A, 0x7B, 0xFF)};
    CHOOSECOLORW cc{sizeof(cc)};
    cc.hwndOwner = g_hwnd;
    cc.rgbResult = RGB((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF);
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    if (ChooseColorW(&cc)) col = ((UINT32)GetRValue(cc.rgbResult) << 16) | ((UINT32)GetGValue(cc.rgbResult) << 8) | GetBValue(cc.rgbResult);
}
static void OnClick(int id) {
    if (id >= ID_S_STYLE && id < ID_S_STYLE + 6) { cfg.outStyle = id - ID_S_STYLE; SaveSettings(); InvalidateRect(g_hwnd, nullptr, FALSE); return; }
    if (id >= ID_S_BGPRE && id < ID_S_BGPRE + 4) {  // Mountain Dusk, or one of the built-in videos
        std::wstring old = cfg.bgImage;
        cfg.bgImage = id == ID_S_BGPRE ? L"" : L"builtin:" + std::to_wstring(id - ID_S_BGPRE);
        std::wstring why = SyncVideo();
        if (!why.empty()) { cfg.bgImage = old; SyncVideo(); flash = why; }
        else { SaveSettings(); flash = id == ID_S_BGPRE ? L"Built-in scene restored." : std::wstring(PRESETS[id - ID_S_BGPRE - 1].name) + L" playing."; }
        LoadImages();
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (id >= ID_S_FX && id < ID_S_FX + 4) {  // effects combine; at least one stays on
        int v = cfg.glowFx ^ (1 << (id - ID_S_FX));
        if (v) { cfg.glowFx = v; SaveSettings(); }
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (id >= ID_AU_FREQ && id < ID_AU_FREQ + 4) { cfg.schedMode = id - ID_AU_FREQ; ShowControls(); InvalidateRect(g_hwnd, nullptr, FALSE); return; }
    if (id >= ID_AU_DAY && id < ID_AU_DAY + 7) { cfg.schedDay = id - ID_AU_DAY; InvalidateRect(g_hwnd, nullptr, FALSE); return; }
    if (id >= ID_NAV && id < ID_NAV + (int)PG_COUNT) { GoPage(id - ID_NAV); return; }
    if (id >= ID_CARD && id < ID_LIST) { selSnap = id - ID_CARD; listScroll = selSnap - 3; if (page != PG_RESTORE) GoPage(PG_HISTORY); return; }
    if (id >= ID_LIST) { selSnap = id - ID_LIST; return; }
    switch (id) {
    case ID_BROWSE_FROM: { auto q = PickFolder(g_hwnd, EditText(editFrom)); if (!q.empty()) SetWindowTextW(editFrom, q.c_str()); break; }
    case ID_BROWSE_TO: {
        auto q = PickFolder(g_hwnd, EditText(editTo));
        if (!q.empty()) { SetWindowTextW(editTo, q.c_str()); KillTimer(g_hwnd, T_HIST); ReadEdits(); CheckFirebox(); RefreshHistory(); SaveSettings(); }
        break;
    }
    case ID_BROWSE_FIRE: {
        auto q = PickFolder(g_hwnd, EditText(editFire));
        if (!q.empty()) { SetWindowTextW(editFire, q.c_str()); KillTimer(g_hwnd, T_HIST); ReadEdits(); CheckFirebox(); RefreshHistory(); SaveSettings(); }
        break;
    }
    case ID_CHK_FIRE:
        ReadEdits();
        cfg.fireOn = !cfg.fireOn;
        if (!cfg.fireOn) viewFire = false;
        CheckFirebox();
        SaveSettings();
        LayoutControls();
        RefreshHistory();
        if (cfg.fireOn && cfg.fire.empty()) SetFocus(editFire);
        break;
    case ID_VIEW_PRIMARY:
    case ID_VIEW_FIRE:
        ReadEdits();
        viewFire = id == ID_VIEW_FIRE;
        selSnap = cardScroll = listScroll = 0;
        RefreshHistory();
        break;
    case ID_RADIO_INC: cfg.full = false; SaveSettings(); break;
    case ID_RADIO_FULL: cfg.full = true; SaveSettings(); break;
    case ID_CHK_LINK: cfg.hardLinks = !cfg.hardLinks; SaveSettings(); break;
    case ID_CHK_PRESERVE: cfg.preserve = !cfg.preserve; SaveSettings(); break;
    case ID_CHK_VERSIONED: cfg.versioned = !cfg.versioned; SaveSettings(); break;
    case ID_CHK_OPEN: cfg.openAfter = !cfg.openAfter; SaveSettings(); break;
    case ID_BACKUP: if (!Running()) StartBackup(); break;
    case ID_CANCEL: job.cancel = true; break;
    case ID_HIST_MENU: HistoryMenu(); break;
    case ID_OPEN_VERSIONS: {
        ReadEdits();
        if (ViewDest().empty()) { Notify(L"Choose a backup directory first."); break; }
        std::wstring root = VersionsRoot();
        MakeDirs(root);
        OpenFolder(root);
        break;
    }
    case ID_H_OPEN: if (!snaps.empty()) OpenFolder(snaps[std::clamp(selSnap, 0, (int)snaps.size() - 1)].dir); break;
    case ID_H_RESTORE: GoPage(PG_RESTORE); break;
    case ID_H_VERIFY: if (!Running()) StartVerify(); break;
    case ID_AU_APPLY:
        ReadEdits();
        if (cfg.schedMode && !ReadTime()) { flash = L"Enter the time as HH:MM (24-hour)."; break; }
        SaveSettings();
        flash = ApplySchedule();
        RefreshTask();
        break;
    case ID_AU_VSS: cfg.shadow = !cfg.shadow; SaveSettings(); break;
    case ID_AU_SUMS: cfg.checksums = !cfg.checksums; SaveSettings(); break;
    case ID_AU_VERIFY: cfg.verifyAfter = !cfg.verifyAfter; SaveSettings(); break;
    case ID_AU_COPY: CopyText(CliCommand(EditText(editFrom), NormPath(EditText(editTo)), cfg.fireOn ? NormPath(EditText(editFire)) : L"")); flash = L"Command copied."; break;
    case ID_R_EXPLORE: if (!snaps.empty()) OpenFolder(Join(snaps[std::clamp(selSnap, 0, (int)snaps.size() - 1)].dir, L"files")); break;
    case ID_R_BROWSE: { auto q = PickFolder(g_hwnd, EditText(editRestore)); if (!q.empty()) SetWindowTextW(editRestore, q.c_str()); break; }
    case ID_R_OVERWRITE: cfg.overwrite = !cfg.overwrite; SaveSettings(); break;
    case ID_R_GO: if (!Running()) StartRestore(); break;
    case ID_S_REMEMBER: cfg.remember = !cfg.remember; ReadEdits(); SaveSettings(); break;
    case ID_S_HIDDEN: cfg.skipHidden = !cfg.skipHidden; SaveSettings(); break;
    case ID_S_BG_PICK: {
        auto f = PickImage(g_hwnd);
        if (f.empty()) break;
        if (IsVideoFile(f)) {
            std::wstring old = cfg.bgImage;
            cfg.bgImage = f;
            std::wstring why = SyncVideo();
            if (!why.empty()) { cfg.bgImage = old; SyncVideo(); LoadImages(); flash = why; break; }
            SaveSettings(); LoadImages(); flash = L"Video background playing.";
            break;
        }
        if (!LoadWic(f)) { flash = L"That image could not be read."; break; }
        cfg.bgImage = f; SaveSettings(); SyncVideo(); LoadImages(); flash = L"Background updated.";
        break;
    }
    case ID_S_BANNER: cfg.banner = !cfg.banner; SaveSettings(); LayoutControls(); break;
    case ID_S_OUTLINE: cfg.outline = !cfg.outline; SaveSettings(); UpdateFx(); break;
    case ID_S_GLOWANIM: cfg.glowAnim = !cfg.glowAnim; SaveSettings(); UpdateFx(); break;
    case ID_S_CW: cfg.flowDir = 0; SaveSettings(); break;
    case ID_S_CCW: cfg.flowDir = 1; SaveSettings(); break;
    case ID_S_SPAN: cfg.outSpan = !cfg.outSpan; SaveSettings(); break;
    case ID_S_COLA: PickColour(cfg.outA); SaveSettings(); break;
    case ID_S_COLB: PickColour(cfg.outB); SaveSettings(); break;
    case ID_S_SWAP: std::swap(cfg.outA, cfg.outB); SaveSettings(); break;
    case ID_S_LOOK_RESET: cfg.tint = 55; cfg.frost = 100; SaveSettings(); break;
    case ID_S_BG_RESET: cfg.bgImage.clear(); SaveSettings(); SyncVideo(); LoadImages(); flash = L"Built-in scene restored."; break;
    case ID_S_OPENDIR: case ID_A_DATA: OpenFolder(g_appDir); break;
    case ID_X_DEFAULTS: SetWindowTextW(editExcl, DEFAULT_EXCLUDES); flash = L"Defaults loaded \u2014 press Save to keep them."; break;
    case ID_X_SAVE: cfg.excludes = EditText(editExcl); SaveSettings(); flash = L"Excludes saved."; break;
    case ID_CAP_MIN: ShowWindow(g_hwnd, SW_MINIMIZE); break;
    case ID_CAP_MAX: ShowWindow(g_hwnd, IsZoomed(g_hwnd) ? SW_RESTORE : SW_MAXIMIZE); break;
    case ID_CAP_CLOSE: PostMessageW(g_hwnd, WM_CLOSE, 0, 0); break;
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------------------------
// Command line: scripted and scheduled runs without a window (EntropyBackupCLI.exe, or EntropyBackup.exe <command>)
// ---------------------------------------------------------------------------------------------
static HANDLE g_out{};
static bool g_outConsole = false, g_quiet = false;
static void Out(const std::wstring& s) {
    if (g_quiet || !g_out || g_out == INVALID_HANDLE_VALUE) return;
    DWORD w;
    if (g_outConsole) WriteConsoleW(g_out, s.c_str(), (DWORD)s.size(), &w, nullptr);
    else { std::string u = U8(s); WriteFile(g_out, u.data(), (DWORD)u.size(), &w, nullptr); }
}
static void OpenOutput() {
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if ((!g_out || g_out == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS))  // the windowed exe started from a console
        g_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD m;
    g_outConsole = g_out && g_out != INVALID_HANDLE_VALUE && GetConsoleMode(g_out, &m);
}
static std::wstring Usage() {
    return std::wstring(L"Entropy Backup ") + APP_VERSION + LR"( - incremental, versioned project backups

Usage: EntropyBackupCLI <command> [options]

Commands
  backup          Create a snapshot of --from inside --to
  verify [ID]     Check a snapshot against its SHA-256 checksums (default: the newest)
  prune           Apply the retention rules (--keep / --keep-days) now, to --to and the Firebox
  list            List the snapshots in --to (give the Firebox folder as --to to list it)
  help            Show this text

Options (anything not given comes from the app's saved settings)
  --from DIR      Project folder to back up
  --to DIR        Backup folder; snapshots go in DIR\EntropyBackup_Versions
  --firebox DIR   Firebox mode: also back up to DIR, a second, independent
                  destination (another drive, a NAS, off-site)  (--no-firebox)
                  If one destination fails, the other is still saved (exit 1).
  --desc TEXT     Description stored with the snapshot
  --full          Full copy instead of incremental      (--incremental to force)
  --mirror        Keep one mirror_latest instead of versioned snapshots
  --no-links      Copy unchanged files instead of hard-linking them
  --exclude PAT   Add an exclude pattern (repeatable)
  --vss           Copy open files from a Volume Shadow Copy (administrator)  (--no-vss)
  --checksums     Record SHA-256 checksums                                  (--no-checksums)
  --verify        Verify the new snapshot after the backup; implies --checksums (--no-verify)
  --keep N        Keep only the newest N snapshots (0 = all)
  --keep-days N   Delete snapshots older than N days (0 = never)
  --log FILE      Append the result to FILE instead of %APPDATA%\EntropyBackup\backup.log
  --quiet         Print nothing; check the exit code

Exit codes: 0 success, 1 success with warnings, 2 failed, 3 invalid arguments, 4 cancelled
)";
}
static bool IsCliCommand(std::wstring c) {
    c = Lower(c);
    while (!c.empty() && (c[0] == L'-' || c[0] == L'/')) c.erase(0, 1);
    for (auto k : {L"backup", L"verify", L"prune", L"list", L"help", L"h", L"?"}) if (c == k) return true;
    return false;
}
static std::wstring Pad(std::wstring s, size_t n) {
    if (s.size() > n) s = s.substr(0, n - 1) + L"…";
    return s + std::wstring(n - s.size(), L' ');
}
static BOOL WINAPI OnConsoleCtrl(DWORD) { job.cancel = true; return TRUE; }
// Waits for the worker, drawing a one-line progress meter when the output is a console.
static int WaitJob() {
    while (Running()) {
        Sleep(200);
        if (!g_outConsole || g_quiet) continue;
        uint64_t wt = job.workTotal, wd = job.workDone;
        int pc = job.phase >= 2 && wt ? (int)std::min<uint64_t>(99, wd * 100 / wt) : 0;
        std::wstring cur;
        { std::lock_guard l(job.m); cur = job.current; }
        wchar_t b[16];
        swprintf_s(b, L"\r[%3d%%] ", pc);
        Out(b + Pad(Num(job.scanned) + L" files  " + Bytes(job.bytesWritten) + L"  " + cur, 70));
    }
    if (worker.joinable()) worker.join();
    if (g_outConsole && !g_quiet) Out(L"\r" + std::wstring(78, L' ') + L"\r");
    std::wstring msg;
    { std::lock_guard l(job.m); msg = job.message; }
    Out(msg + L"\n");
    int ph = job.phase;
    return ph == 4 ? (job.warnings ? 1 : 0) : ph == 6 ? 4 : 2;
}
static int RunCli(const std::vector<std::wstring>& a) {
    OpenOutput();
    std::wstring cmd = a.empty() ? L"help" : Lower(a[0]);
    while (!cmd.empty() && (cmd[0] == L'-' || cmd[0] == L'/')) cmd.erase(0, 1);
    BackupParams P{cfg.from, cfg.to, L"", cfg.full, cfg.hardLinks, cfg.preserve, cfg.versioned, cfg.skipHidden, Patterns(cfg.excludes),
                   cfg.shadow, cfg.checksums, cfg.verifyAfter && cfg.checksums, cfg.keepCount, cfg.keepDays, FireboxSet() ? cfg.fire : L""};
    bool scheduled = false;
    std::wstring snapArg;
    auto bad = [](const std::wstring& m) { Out(L"Error: " + m + L"\nRun EntropyBackupCLI help for usage.\n"); return 3; };
    for (size_t i = 1; i < a.size(); i++) {
        std::wstring o = Lower(a[i]);
        bool val = i + 1 < a.size();
        auto num = [&](int& v) {
            wchar_t* end = nullptr;
            long n = wcstol(a[++i].c_str(), &end, 10);
            v = (int)std::clamp(n, 0L, 99999L);
            return end && !*end && n >= 0;
        };
        if (o == L"--from" && val) P.src = a[++i];
        else if (o == L"--to" && val) P.dst = a[++i];
        else if (o == L"--firebox" && val) P.fire = a[++i];
        else if (o == L"--no-firebox") P.fire.clear();
        else if ((o == L"--desc" || o == L"--description") && val) P.desc = a[++i];
        else if (o == L"--exclude" && val) { for (auto& x : Patterns(a[++i])) P.patterns.push_back(x); }
        else if (o == L"--log" && val) g_logPath = NormPath(a[++i]);
        else if (o == L"--keep" && val) { if (!num(P.keep)) return bad(L"--keep needs a number."); }
        else if (o == L"--keep-days" && val) { if (!num(P.keepDays)) return bad(L"--keep-days needs a number."); }
        else if (o == L"--full") P.full = true;
        else if (o == L"--incremental") P.full = false;
        else if (o == L"--mirror") P.versioned = false;
        else if (o == L"--no-links") P.hardLinks = false;
        else if (o == L"--vss") P.shadow = true;
        else if (o == L"--no-vss") P.shadow = false;
        else if (o == L"--checksums") P.checksums = true;
        else if (o == L"--no-checksums") P.checksums = P.verify = false;
        else if (o == L"--verify") P.verify = P.checksums = true;
        else if (o == L"--no-verify") P.verify = false;
        else if (o == L"--quiet" || o == L"-q") g_quiet = true;
        else if (o == L"--scheduled") scheduled = true;
        else if (o == L"--help" || o == L"-h" || o == L"/?") cmd = L"help";
        else if (cmd == L"verify" && snapArg.empty() && o[0] != L'-') snapArg = a[i];
        else return bad(L"unknown or incomplete option " + a[i]);
    }
    if (cmd == L"help" || cmd == L"h" || cmd == L"?") { Out(Usage()); return 0; }
    if (cmd != L"backup" && cmd != L"verify" && cmd != L"prune" && cmd != L"list") return bad(L"unknown command " + a[0]);
    if (P.dst.empty()) return bad(L"--to is required.");
    std::wstring dst = NormPath(P.dst), root = Join(dst, L"EntropyBackup_Versions");
    if (g_outConsole) SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);

    if (cmd == L"backup") {
        if (P.src.empty()) return bad(L"--from is required.");
        if (P.desc.empty() && scheduled) P.desc = L"Scheduled backup";
        std::wstring src = NormPath(P.src);
        ResetJob(0, (scheduled ? L"Scheduled backup " : L"Backup ") + src + L" -> " + dst + (P.fire.empty() ? L"" : L" + Firebox " + NormPath(P.fire)));
        if (!P.fire.empty()) {
            std::wstring why = FireboxConflict(dst, P.fire);
            if (!why.empty()) return bad(why);
        }
        Out(L"Backing up " + src + L" to " + root + L"\n");
        if (!P.fire.empty()) Out(L"Firebox:   " + Join(NormPath(P.fire), L"EntropyBackup_Versions") + L"\n");
        worker = std::thread(BackupWorker, P);
        return WaitJob();
    }
    if (cmd == L"prune") {
        if (!P.keep && !P.keepDays) return bad(L"give --keep or --keep-days (or set retention in the app).");
        std::vector<std::wstring> roots{root};
        if (!P.fire.empty()) roots.push_back(Join(NormPath(P.fire), L"EntropyBackup_Versions"));
        int rc = 0;
        for (auto& r : roots) {
            try {
                VersionsLock lock(r);
                int n = Prune(r, P.keep, P.keepDays);
                std::wstring msg = L"Removed " + Num(n) + (n == 1 ? L" old snapshot" : L" old snapshots") + L" from " + r + L".";
                LogEvent(L"Prune " + r + L" | OK | " + msg);
                Out(msg + L"\n");
            } catch (const Fail& f) {
                LogEvent(L"Prune " + r + L" | FAILED | " + f.msg);
                Out(L"Error: " + r + L": " + f.msg + L"\n");
                rc = 2;
            }
        }
        return rc;
    }
    cfg.to = dst;
    viewFire = false;
    RefreshHistory();
    if (cmd == L"list") {
        if (snaps.empty()) { Out(L"No snapshots in " + root + L"\n"); return 0; }
        Out(Pad(L"ID", 21) + Pad(L"TYPE", 13) + Pad(L"FILES", 10) + Pad(L"SIZE", 11) + Pad(L"CHECKSUMS", 12) + L"DESCRIPTION\n");
        for (auto& s : snaps) {
            std::wstring sums = !s.sums ? L"-" : !s.verified ? L"recorded" : s.vBad ? L"PROBLEMS" : L"verified";
            Out(Pad(s.mirror ? L"mirror_latest" : s.id, 21) + Pad(s.type.empty() ? L"-" : s.type, 13) + Pad(Num(s.files), 10) +
                Pad(Bytes(s.bytesTotal), 11) + Pad(sums, 12) + s.desc + L"\n");
        }
        return 0;
    }
    std::wstring dir;  // verify: newest, or the one named by ID, folder name or path
    for (auto& s : snaps) {
        std::wstring name = s.dir.substr(s.dir.find_last_of(L'\\') + 1);
        if (snapArg.empty() || Lower(s.id) == Lower(snapArg) || Lower(name) == Lower(snapArg) || Lower(s.dir) == Lower(NormPath(snapArg))) { dir = s.dir; break; }
    }
    if (dir.empty()) { Out(L"Error: no snapshot " + (snapArg.empty() ? L"found in " + root : snapArg) + L"\n"); return 2; }
    ResetJob(2, L"Verify " + dir);
    Out(L"Verifying " + dir + L"\n");
    VerifyResult v;
    worker = std::thread(VerifyWorker, dir, &v);
    int rc = WaitJob();
    for (auto& p : v.problems) Out(L"  " + p + L"\n");
    return rc;
}

// ---------------------------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------------------------
static LRESULT CALLBACK Wnd(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_hwnd = h;
        g_dpi = GetDpiForWindow(h);
        fieldBrush = CreateSolidBrush(FIELD_GDI);
        editFrom = MakeEdit(h, IDC_FROM, ES_AUTOHSCROLL, L"Select the project folder to protect");
        editTo = MakeEdit(h, IDC_TO, ES_AUTOHSCROLL, L"Select a backup folder, ideally on another drive");
        editFire = MakeEdit(h, IDC_FIRE, ES_AUTOHSCROLL, L"Select a second backup folder on another drive, NAS or off-site location");
        editDesc = MakeEdit(h, IDC_DESC, ES_MULTILINE | ES_AUTOVSCROLL, nullptr);
        editRestore = MakeEdit(h, IDC_RTARGET, ES_AUTOHSCROLL, L"Select an empty folder to restore into");
        editExcl = MakeEdit(h, IDC_EXCL, ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL, nullptr);
        editTime = MakeEdit(h, IDC_TIME, ES_AUTOHSCROLL | ES_CENTER, nullptr);
        editKeep = MakeEdit(h, IDC_KEEP, ES_AUTOHSCROLL | ES_CENTER | ES_NUMBER, nullptr);
        editDays = MakeEdit(h, IDC_DAYS, ES_AUTOHSCROLL | ES_CENTER | ES_NUMBER, nullptr);
        SendMessageW(editTime, EM_SETLIMITTEXT, 5, 0);
        SendMessageW(editKeep, EM_SETLIMITTEXT, 5, 0);
        SendMessageW(editDays, EM_SETLIMITTEXT, 5, 0);
        wchar_t tm[8];
        swprintf_s(tm, L"%02d:%02d", cfg.schedHour, cfg.schedMin);
        SetWindowTextW(editTime, tm);
        SetWindowTextW(editKeep, std::to_wstring(cfg.keepCount).c_str());
        SetWindowTextW(editDays, std::to_wstring(cfg.keepDays).c_str());
        SendMessageW(editDesc, EM_SETLIMITTEXT, 500, 0);
        SetWindowTextW(editFrom, cfg.from.c_str());
        SetWindowTextW(editTo, cfg.to.c_str());
        SetWindowTextW(editFire, cfg.fire.c_str());
        SetWindowTextW(editExcl, cfg.excludes.c_str());
        CheckFirebox();
        RefreshHistory();
        LayoutControls();
        if (!SyncVideo().empty()) LogRuntime(L"Background video could not be played");
        UpdateFx();
        return 0;
    }
    case WM_NCCALCSIZE:
        if (w) {
            if (IsZoomed(h)) {
                auto* p = (NCCALCSIZE_PARAMS*)l;
                int fx = GetSystemMetricsForDpi(SM_CXFRAME, g_dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, g_dpi);
                int fy = GetSystemMetricsForDpi(SM_CYFRAME, g_dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, g_dpi);
                p->rgrc[0].left += fx; p->rgrc[0].right -= fx; p->rgrc[0].top += fy; p->rgrc[0].bottom -= fy;
            }
            return 0;
        }
        break;
    case WM_NCACTIVATE: return DefWindowProcW(h, m, w, -1);
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(h, &pt);
        RECT cr;
        GetClientRect(h, &cr);
        if (!IsZoomed(h)) {
            int b = MulDiv(6, g_dpi, 96);
            bool L = pt.x < b, Rr = pt.x >= cr.right - b, T = pt.y < b, B = pt.y >= cr.bottom - b;
            if (T && L) return HTTOPLEFT;
            if (T && Rr) return HTTOPRIGHT;
            if (B && L) return HTBOTTOMLEFT;
            if (B && Rr) return HTBOTTOMRIGHT;
            if (L) return HTLEFT;
            if (Rr) return HTRIGHT;
            if (T) return HTTOP;
            if (B) return HTBOTTOM;
        }
        if (HitId((float)pt.x, (float)pt.y)) return HTCLIENT;
        float dx = (pt.x - gox) / gs, dy = (pt.y - goy) / gs;
        if (dx < 0 || dy < 0 || dx > DW || dy > DH || dy < 14) return HTCAPTION;
        if (dy < 54) return HTCAPTION;
        if (cfg.banner && dx > 318 && dx < 1110 && dy < 140) return HTCAPTION;
        if (dx < 318 && dy < 340) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)l;
        mm->ptMinTrackSize = {MulDiv(980, g_dpi, 96), MulDiv(640, g_dpi, 96)};
        return 0;
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(w);
        RECT* r = (RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SIZE:
        UpdateFx();
        if (w == SIZE_MINIMIZED) return 0;
        if (rt) rt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
        bgDirty = true;
        LayoutControls();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_ACTIVATEAPP:  // pick up snapshots made meanwhile by the scheduled task or the command line
        if (w && !Running()) {
            RefreshHistory();
            if (page == PG_AUTO) RefreshTask();
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT p;
        BeginPaint(h, &p);
        Paint();
        EndPaint(h, &p);
        return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {  // disabled edits ask for the static colours
        if (m == WM_CTLCOLORSTATIC && (HWND)l != editTime) break;
        HDC dc = (HDC)w;
        SetTextColor(dc, m == WM_CTLCOLOREDIT ? RGB(236, 241, 247) : RGB(110, 126, 146));
        SetBkColor(dc, FIELD_GDI);
        return (LRESULT)fieldBrush;
    }
    case WM_COMMAND: {
        int code = HIWORD(w), id = LOWORD(w);
        if (code == EN_SETFOCUS || code == EN_KILLFOCUS) InvalidateRect(h, nullptr, FALSE);
        if (code == EN_CHANGE) {
            if (id == IDC_TO || id == IDC_FIRE) SetTimer(h, T_HIST, 450, nullptr);
            if (id == IDC_EXCL || id == IDC_TIME) flash.clear();
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_APP_FRAME: {  // a new video frame: upload it and re-blur the glass
        static std::vector<BYTE> px;
        UINT fw = 0, fh = 0;
        if (rt && video.Take(px, fw, fh) && px.size() >= (size_t)fw * fh * 4) {
            if (!videoBmp || videoBmp->GetPixelSize().width != fw || videoBmp->GetPixelSize().height != fh) {
                videoBmp.Reset();
                rt->CreateBitmap(D2D1::SizeU(fw, fh), D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)), &videoBmp);
            }
            if (videoBmp && SUCCEEDED(videoBmp->CopyFromMemory(nullptr, px.data(), fw * 4))) { bgDirty = true; InvalidateRect(h, nullptr, FALSE); }
        }
        return 0;
    }
    case WM_TIMER:
        if (w == T_HIST) { KillTimer(h, T_HIST); ReadEdits(); CheckFirebox(); RefreshHistory(); }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_APP_JOBDONE: {
        if (worker.joinable()) worker.join();
        KillTimer(h, T_ANIM);
        UpdateFx();
        std::wstring dir;
        { std::lock_guard lk(job.m); dir = job.resultDir; }
        RefreshHistory();
        if (job.phase == 4 && job.kind == 0) {
            selSnap = 0; cardScroll = 0;
            SetWindowTextW(editDesc, L"");
            if (cfg.openAfter && !dir.empty()) OpenFolder(dir);
        }
        if (job.phase == 4 && job.kind == 1 && !dir.empty()) OpenFolder(dir);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!trackingMouse) { TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0}; TrackMouseEvent(&t); trackingMouse = true; }
        if (dragId) { DragSlider((float)GET_X_LPARAM(l)); return 0; }
        int id = HitId((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l));
        if (id != hoverId) { hoverId = id; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE:
        trackingMouse = false;
        if (hoverId) { hoverId = 0; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && (HWND)w == h) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            int id = HitId((float)p.x, (float)p.y);
            if (id && (id < ID_CAP_MIN || id > ID_CAP_CLOSE)) { SetCursor(LoadCursor(nullptr, IDC_HAND)); return TRUE; }
        }
        break;
    case WM_LBUTTONDOWN:
        SetFocus(h);
        pressId = HitId((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l));
        if (pressId) SetCapture(h);
        if (pressId == ID_S_TINT || pressId == ID_S_FROST || pressId == ID_S_OWIDTH || pressId == ID_S_OGLOW || pressId == ID_S_GSPEED) { dragId = pressId; DragSlider((float)GET_X_LPARAM(l)); }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        int id = HitId((float)GET_X_LPARAM(l), (float)GET_Y_LPARAM(l)), was = pressId;
        pressId = 0;
        if (GetCapture() == h) ReleaseCapture();
        if (dragId) { dragId = 0; SaveSettings(); InvalidateRect(h, nullptr, FALSE); return 0; }
        if (id && id == was) OnClick(id);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(h, &p);
        int step = GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1;
        if (In(zoneCards, (float)p.x, (float)p.y)) cardScroll += step;
        else if (In(zoneList, (float)p.x, (float)p.y)) listScroll += step;
        else return 0;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_CLOSE:
        if (Running()) {
            if (MessageBoxW(h, L"A job is still running. Cancel it and exit?", L"Entropy Backup", MB_YESNO | MB_ICONWARNING) != IDYES) return 0;
            job.cancel = true;
            if (worker.joinable()) worker.join();
        }
        ReadEdits();
        ReadTime();
        SaveSettings();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (worker.joinable()) { job.cancel = true; worker.join(); }
        video.Close();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// Start-up shared by the window and the command line.
static HRESULT InitApp(DWORD coInit) {
    HRESULT co = CoInitializeEx(nullptr, coInit);
    // Packet privacy with identify-level impersonation lets the shadow copy writers call back into this process.
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IDENTIFY, nullptr, EOAC_DYNAMIC_CLOAKING, nullptr);
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) g_appDir = Join(p, L"EntropyBackup");
    CoTaskMemFree(p);
    MakeDirs(g_appDir);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_exeDir = exe;
    g_exeDir = g_exeDir.substr(0, g_exeDir.find_last_of(L'\\'));
    LoadSettings();
    return co;
}
#ifdef ENTROPY_CLI
// EntropyBackupCLI.exe: the same program built for the console subsystem, so shells wait for it and get its exit code.
int wmain(int argc, wchar_t** argv) {
    HRESULT co = InitApp(COINIT_MULTITHREADED);
    int rc = RunCli(std::vector<std::wstring>(argv + 1, argv + argc));
    if (SUCCEEDED(co)) CoUninitialize();
    return rc;
}
#endif

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int) {
    HRESULT co = InitApp(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    {  // a command (as used by the scheduled task) runs without a window
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::vector<std::wstring> args;
        if (argv) args.assign(argv + 1, argv + argc);
        LocalFree(argv);
        if (!args.empty() && IsCliCommand(args[0])) {
            int rc = RunCli(args);
            if (SUCCEEDED(co)) CoUninitialize();
            return rc;
        }
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    LogRuntime(L"Starting Entropy Backup", co);
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), reinterpret_cast<void**>(d2f.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) LogRuntime(L"D2D1CreateFactory failed", hr);
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwf.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) LogRuntime(L"DWriteCreateFactory failed", hr);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (d2f) d2f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &roundSt);

    HICON bigIcon = d2f && dwf ? MakeIcon(64) : nullptr, smallIcon = d2f && dwf ? MakeIcon(32) : nullptr;
    WNDCLASSEXW wc{sizeof(wc), CS_HREDRAW | CS_VREDRAW, Wnd, 0, 0, hi, bigIcon ? bigIcon : LoadIcon(nullptr, IDI_APPLICATION), LoadCursor(nullptr, IDC_ARROW),
                   nullptr, nullptr, CLS, smallIcon ? smallIcon : LoadIcon(nullptr, IDI_APPLICATION)};
    if (!RegisterClassExW(&wc)) { LogRuntime(L"RegisterClassExW failed", HRESULT_FROM_WIN32(GetLastError())); return 2; }

    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    float k = GetDpiForSystem() / 96.f;
    k = std::min({k, (wa.right - wa.left) * 0.94f / DW, (wa.bottom - wa.top) * 0.94f / DH});
    int ww = (int)(DW * k), wh = (int)(DH * k);
    // IMPORTANT: do not use WS_EX_NOREDIRECTIONBITMAP with ID2D1HwndRenderTarget.
    HWND h = CreateWindowExW(0, CLS, L"Entropy Backup", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, wa.left + (wa.right - wa.left - ww) / 2,
                             wa.top + (wa.bottom - wa.top - wh) / 2, ww, wh, nullptr, nullptr, hi, nullptr);
    if (!h) { LogRuntime(L"CreateWindowExW failed", HRESULT_FROM_WIN32(GetLastError())); return 3; }
    BOOL dark = TRUE;
    DwmSetWindowAttribute(h, 20, &dark, sizeof(dark));          // DWMWA_USE_IMMERSIVE_DARK_MODE
    int corner = 2;
    DwmSetWindowAttribute(h, 33, &corner, sizeof(corner));      // DWMWA_WINDOW_CORNER_PREFERENCE = round
    MARGINS mg{0, 0, 1, 0};
    DwmExtendFrameIntoClientArea(h, &mg);                         // keeps the native shadow on a frameless window
    SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB) {  // simple Tab navigation between visible fields
            HWND next = GetNextDlgTabItem(h, GetFocus(), GetKeyState(VK_SHIFT) < 0);
            if (next) { SetFocus(next); SendMessageW(next, EM_SETSEL, 0, -1); continue; }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (editFont) DeleteObject(editFont);
    if (fieldBrush) DeleteObject(fieldBrush);
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}
