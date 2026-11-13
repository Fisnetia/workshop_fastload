// workshop_fastload.cpp - l4n plugin
//
// Purpose: observe / accelerate the Left 4 Dead 2 "workshop check" that runs on
// every launch.
//
// How it works: engine.dll resolves SteamInternal_CreateInterface(
// "STEAMREMOTESTORAGE_INTERFACE_VERSION016") once, then dispatches every call
// through the interface object's vtable (we verified `mov eax,[eax+0x9C];
// call eax` style sites inside engine.dll). The vtable is shared process wide,
// so patching its slots intercepts *all* callers.
//
// Two behaviours, selected by the ini file:
//   mode=0  log only        - forward everything untouched, record timestamps /
//                             arguments so we can see where time goes
//   mode=1  rewrite one arg - replace unMaxSecondsOld in GetPublishedFileDetails
//                             with a large value so the Steam client answers
//                             from its local cache; everything else still goes
//                             through the original path untouched
//
// In mode=1 an item is still checked live (asks the Steam servers) when:
//   - it is on the bypass list (built-in tooling ids plus bypass_ids): it gets
//     the engine's own window, 0
//   - check_all_next_launch=1: every item gets 0 for one launch; set from the
//     HUD menu, cleared at startup
//   - max_seconds_old is a number of seconds and the item's cache entry is older
//     than that (the default "never" keeps this from happening)
//
// Failure policy: every hook only records (and maybe rewrites one argument) and
// then jumps back to the original function. If anything looks wrong we skip that
// slot, so the worst case is the current slow-but-working behaviour.

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>

#include "l4n_plugin.h"

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
namespace {
HMODULE          g_hSelf   = nullptr;
CRITICAL_SECTION g_cs;
volatile LONG    g_csReady = 0;
HANDLE           g_hLog    = INVALID_HANDLE_VALUE;
LARGE_INTEGER    g_qpcFreq = {};
LARGE_INTEGER    g_qpcBase = {};
volatile LONG    g_installed = 0;

int          g_mode        = 0;
unsigned int g_forceMaxAge = 604800;   // 7 days
int          g_logVtable   = 1;
// Diagnostic hooks (UGCDownload / UGCRead / GetUGCDetails) only record their
// arguments and change nothing. Two of them have never been seen in a live
// session and no test has ever executed their argument layouts, so they stay off
// unless explicitly asked for. EnumerateUserSubscribedFiles is deliberately *not*
// one of these: its timestamps are the only view of the subscription list paging
// and mark where the per-item queries start, so it is always installed. (The gap
// between its pages varies 2-9 s with Steam's own response time; it is noise, not
// something this plugin changes.)
int          g_diagHooks   = 0;

// A config problem cannot be logged before the log file exists, so it is captured
// here and printed right after OpenLog.
char         g_cfgWarn[160] = {};

// Items that must keep the stock freshness window: they always get the caller's
// original value, i.e. are checked live on every launch. The first entries are
// built in (the tooling this plugin ships with relies on workshop auto-update);
// bypass_ids in the ini appends to them, with no fixed upper limit - each entry
// only costs one live query (~0.3 s) per launch, which is the user's call.
// Filled once in LoadConfig, before any hook is installed, and read-only after.
const std::uint64_t kBuiltinBypass[] = {
    3558049615ULL,   // left4neko itself - it checks for its own updates
    3787239937ULL,   // skeeto client framework - also updates itself from the workshop
};
const int kBuiltinBypassCount = (int)(sizeof(kBuiltinBypass) / sizeof(kBuiltinBypass[0]));
std::vector<std::uint64_t> g_bypass(kBuiltinBypass, kBuiltinBypass + kBuiltinBypassCount);
char g_bypassWarn[192] = {};   // bad bypass_ids entries, printed after OpenLog

enum BypassKind { BYPASS_NONE = 0, BYPASS_BUILTIN = 1, BYPASS_USER = 2 };
BypassKind BypassKindOf(std::uint64_t id);

// max_seconds_old=never: every non-bypassed item is answered from Steam's local
// cache for good, so mods never re-check for updates on their own. 0x7FFFFFFF
// (~68 years) instead of 0xFFFFFFFF on purpose: if anything along the way treats
// the window as a signed int, 0xFFFFFFFF would turn into -1 and could mean
// "stale, always refetch" - the exact opposite of what was asked for.
const unsigned int kNeverMaxAge = 0x7FFFFFFFu;

// One-shot full check. check_all_next_launch=1 in the ini (set from the HUD menu,
// or by hand) makes the *next* launch forward the engine's own window (0) for
// every item, so pending mod updates are picked up once. The flag is read and
// cleared at startup, so the launch after that is fast again.
int           g_fullCheckThisLaunch = 0;
volatile LONG g_fullCheckNext       = 0;   // what the ini says for the next launch

volatile LONG g_cntCached = 0;   // GPD calls answered from the local cache
volatile LONG g_cntLive   = 0;   // GPD calls forwarded with the caller's window

volatile LONG g_cntGpd     = 0;
volatile LONG g_cntEnum    = 0;
volatile LONG g_cntDl      = 0;
volatile LONG g_cntRead    = 0;
volatile LONG g_cntDetails = 0;

// Per-hook log ceiling: a misbehaving mod could otherwise make a callback fire
// thousands of times a second and turn the log into a disk sink. Behaviour is
// never capped, only the log lines.
enum { LOG_BUDGET = 2000 };

// Wide on purpose: the game folder can contain characters outside the system
// code page (say, a Korean folder name on a Chinese Windows), and the ANSI file
// APIs cannot open such a path at all.
wchar_t g_logPath[MAX_PATH] = {};
wchar_t g_iniPath[MAX_PATH] = {};
char    g_cfgNote[160] = {};   // what happened to the config file, for the log

inline double Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_qpcBase.QuadPart) / (double)g_qpcFreq.QuadPart;
}

void Logf(const char* fmt, ...)
{
    if (g_hLog == INVALID_HANDLE_VALUE || !g_csReady) return;

    char buf[1024];
    int n = _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[%10.6f] ", Now());
    if (n < 0) return;

    va_list ap;
    va_start(ap, fmt);
    int m = _vsnprintf_s(buf + n, sizeof(buf) - (size_t)n - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (m < 0) m = (int)strlen(buf + n);
    n += m;
    buf[n++] = '\n';

    EnterCriticalSection(&g_cs);
    DWORD wrote = 0;
    WriteFile(g_hLog, buf, (DWORD)n, &wrote, nullptr);
    LeaveCriticalSection(&g_cs);
}
}  // namespace

// ---------------------------------------------------------------------------
// Original function pointers - global (not in an anonymous namespace) so the
// inline assembly below can reference them by their plain symbol name.
// ---------------------------------------------------------------------------
void* g_pfnGpd  = nullptr;
void* g_pfnEnum = nullptr;
void* g_pfnDl   = nullptr;
void* g_pfnRead = nullptr;
void* g_pfnDet  = nullptr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

bool IsExecutablePtr(const void* p)
{
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD prot = mbi.Protect & 0xFF;
    return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ ||
           prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
}

bool LooksLikeVtable(void** vt, int probeOffset)
{
    if (!vt) return false;
    __try {
        if (!IsExecutablePtr(vt[0])) return false;
        if (!IsExecutablePtr(vt[1])) return false;
        if (probeOffset > 0 && !IsExecutablePtr(vt[probeOffset / 4])) return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

// steam_api.dll's flat wrappers look like:
//   55 8B EC              push ebp / mov ebp,esp
//   8B 4D 08              mov  ecx,[ebp+8]        ; this
//   FF 75 14 FF 75 10     push ...                ; the call's arguments
//   8B 01                 mov  eax,[ecx]          ; this->vtable
//   FF 75 0C              push ...
//   8B 80 9C 00 00 00     mov  eax,[eax+0x9C]
//   FF D0                 call eax
// Note the `mov eax,[ecx]` is *not* immediately followed by the vtable load -
// pushes for the arguments sit in between - so the scan has to walk forward a
// short window instead of looking at the next two bytes.
//
// The scan stops at the end of the wrapper (`ret` / `int3` padding). Without
// that stop it walks past the wrapper into the next exported function, which is
// only 27 bytes away, and latches onto *its* vtable load - the cause of the
// first crash, where GetPublishedFileDetails reported 0x68 (GetUGCDetails'
// slot) and UGCDownload reported 0x30. The window size alone cannot prevent
// this; only stopping at the function boundary does.
//
// This is a cross-check only: the verified offsets below are authoritative and
// the parsed value is never used to pick a slot.
bool ParseVtableOffset(const void* fn, int* out)
{
    if (!fn || !out) return false;
    const unsigned char* p = (const unsigned char*)fn;
    __try {
        for (int i = 0; i + 2 < 48; i++) {
            if (p[i] == 0xC3 || p[i] == 0xCC) break;   // end of this wrapper
            if (p[i] != 0x8B || p[i + 1] != 0x01) continue;
            for (int j = i + 2; j < i + 26 && j + 6 < 48; j++) {
                if (p[j] == 0xC3 || p[j] == 0xCC) break;
                int off = -1;
                if (p[j] == 0x8B && p[j + 1] == 0x80)      off = *(const int*)(p + j + 2);
                else if (p[j] == 0xFF && p[j + 1] == 0x90) off = *(const int*)(p + j + 2);
                else if (p[j] == 0x8B && p[j + 1] == 0x40) off = p[j + 2];
                else if (p[j] == 0xFF && p[j + 1] == 0x50) off = p[j + 2];
                if (off > 0 && off < 0x400 && (off % 4) == 0) { *out = off; return true; }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

bool PatchVtable(void** vt, int offset, void* replacement, void** outOriginal, const char* label)
{
    void** slot = (void**)((char*)vt + offset);
    void*  orig = nullptr;
    __try {
        orig = *slot;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("HOOK   %-28s read fault at slot %#x, skipped", label, offset);
        return false;
    }
    if (!IsExecutablePtr(orig)) {
        Logf("HOOK   %-28s slot %#x is not executable (%p), skipped", label, offset, orig);
        return false;
    }

    // Publish the original first, then make the hook reachable. The other order
    // leaves a window (one VirtualProtect syscall wide) where the engine can call
    // the new hook while g_pfnXxx is still null, and the hook would jump to null.
    *outOriginal = orig;

    // The originals live in steamclient.dll. If this one lives somewhere else, a
    // previous hook already owns the slot and we are chaining behind it - not an
    // error (our thunks restore the stack exactly, so a plain `jmp` upstream is
    // fine), but worth saying out loud because it changes who is really in control.
    HMODULE hOwner = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)orig, &hOwner) && hOwner) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(hOwner, path, MAX_PATH);
        const char* base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        if (_stricmp(base, "steamclient.dll") != 0) {
            Logf("HOOK   %-28s WARN original %p lives in %s, not steamclient.dll "
                 "(chaining behind another hook)", label, orig, base);
        }
    }

    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        Logf("HOOK   %-28s VirtualProtect failed, err=%lu", label, GetLastError());
        return false;
    }
    *slot = replacement;
    DWORD restore = 0;
    VirtualProtect(slot, sizeof(void*), old, &restore);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));

    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Hook bodies
//
// thiscall convention: ecx = this, the rest live on the stack and the callee pops
// them. Each hook re-pushes the arguments it wants to inspect, calls its cdecl
// callback, then jumps to the original function - which pops the stack itself, so
// argument accounting stays exact.
//
// ecx must be restored before the jump: the callback is free to clobber it, and
// the original function starts with `mov eax,[ecx]`. Losing this is exactly what
// crashes inside steamclient.dll. The saved copy lives on the stack while the
// callback runs, and is reloaded right before the tail jump.
// ---------------------------------------------------------------------------
extern "C" unsigned int __cdecl OnGpd(void* self, std::uint64_t id, unsigned int maxAge);
extern "C" void __cdecl OnEnum(void* self, unsigned int startIndex);
extern "C" void __cdecl OnDownload(void* self, std::uint64_t handle, unsigned int priority);
extern "C" void __cdecl OnRead(void* self, std::uint64_t handle);
extern "C" void __cdecl OnDetails(void* self, std::uint64_t handle);

extern "C" __declspec(naked) void HookGpd()
{
    __asm {
        push ebp
        mov  ebp, esp
        push ecx                      // save this
        push dword ptr [ebp + 16]     // maxAge
        push dword ptr [ebp + 12]     // id high
        push dword ptr [ebp + 8]      // id low
        push ecx                      // this for the callback
        call OnGpd
        add  esp, 16
        mov  dword ptr [ebp + 16], eax
        mov  ecx, dword ptr [esp]     // restore this
        add  esp, 4
        pop  ebp
        jmp  dword ptr [g_pfnGpd]
    }
}

extern "C" __declspec(naked) void HookEnum()
{
    __asm {
        push ebp
        mov  ebp, esp
        push ecx
        push dword ptr [ebp + 8]      // startIndex
        push ecx
        call OnEnum
        add  esp, 8
        mov  ecx, dword ptr [esp]
        add  esp, 4
        pop  ebp
        jmp  dword ptr [g_pfnEnum]
    }
}

extern "C" __declspec(naked) void HookDownload()
{
    __asm {
        push ebp
        mov  ebp, esp
        push ecx
        push dword ptr [ebp + 16]     // priority
        push dword ptr [ebp + 12]     // handle high
        push dword ptr [ebp + 8]      // handle low
        push ecx
        call OnDownload
        add  esp, 16
        mov  ecx, dword ptr [esp]
        add  esp, 4
        pop  ebp
        jmp  dword ptr [g_pfnDl]
    }
}

extern "C" __declspec(naked) void HookRead()
{
    __asm {
        push ebp
        mov  ebp, esp
        push ecx
        push dword ptr [ebp + 12]
        push dword ptr [ebp + 8]
        push ecx
        call OnRead
        add  esp, 12
        mov  ecx, dword ptr [esp]
        add  esp, 4
        pop  ebp
        jmp  dword ptr [g_pfnRead]
    }
}

extern "C" __declspec(naked) void HookDetails()
{
    __asm {
        push ebp
        mov  ebp, esp
        push ecx
        push dword ptr [ebp + 12]
        push dword ptr [ebp + 8]
        push ecx
        call OnDetails
        add  esp, 12
        mov  ecx, dword ptr [esp]
        add  esp, 4
        pop  ebp
        jmp  dword ptr [g_pfnDet]
    }
}

// ---------------------------------------------------------------------------
// Hook callbacks
// ---------------------------------------------------------------------------
extern "C" unsigned int __cdecl OnGpd(void* /*self*/, std::uint64_t id, unsigned int maxAge)
{
    LONG n = InterlockedIncrement(&g_cntGpd);
    BypassKind kind = BypassKindOf(id);
    bool keep = kind != BYPASS_NONE;
    bool full = g_fullCheckThisLaunch != 0;
    unsigned int out = maxAge;
    if (g_mode == 1 && !keep && !full && maxAge < g_forceMaxAge) out = g_forceMaxAge;
    InterlockedIncrement(out != maxAge ? &g_cntCached : &g_cntLive);
    if (n <= LOG_BUDGET) {
        const char* tag = kind == BYPASS_BUILTIN ? "  (keep, tooling)"
                        : kind == BYPASS_USER    ? "  (keep, bypass_ids)"
                        : (full && g_mode == 1)  ? "  (keep, full check)"
                        : "";
        Logf("GPD    id=%-12llu maxSecondsOld=%-10u -> %-10u%s",
             (unsigned long long)id, maxAge, out, tag);
    }
    return out;
}

extern "C" void __cdecl OnEnum(void* /*self*/, unsigned int startIndex)
{
    LONG n = InterlockedIncrement(&g_cntEnum);
    if (n <= LOG_BUDGET) Logf("ENUM   #%ld startIndex=%u", n, startIndex);
}

extern "C" void __cdecl OnDownload(void* /*self*/, std::uint64_t handle, unsigned int priority)
{
    LONG n = InterlockedIncrement(&g_cntDl);
    if (n <= LOG_BUDGET) Logf("UGCdl  #%ld handle=%llu priority=%u", n, (unsigned long long)handle, priority);
}

extern "C" void __cdecl OnRead(void* /*self*/, std::uint64_t handle)
{
    LONG n = InterlockedIncrement(&g_cntRead);
    if (n <= LOG_BUDGET) Logf("UGCrd  handle=%llu", (unsigned long long)handle);
}

extern "C" void __cdecl OnDetails(void* /*self*/, std::uint64_t handle)
{
    LONG n = InterlockedIncrement(&g_cntDetails);
    if (n <= LOG_BUDGET) Logf("UGCdet handle=%llu", (unsigned long long)handle);
}

// ---------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------
namespace {

struct Slot {
    const char* label;
    const char* exportName;
    int         knownOffset;
    void**      pfn;
    void*       hook;
    int         usedOffset;
    bool        diag;          // only installed when diag_hooks=1
};

Slot g_slots[] = {
    { "GetPublishedFileDetails",      "SteamAPI_ISteamRemoteStorage_GetPublishedFileDetails",      0x9C, &g_pfnGpd,  (void*)&HookGpd,      -1, false },
    { "EnumerateUserSubscribedFiles", "SteamAPI_ISteamRemoteStorage_EnumerateUserSubscribedFiles", 0xAC, &g_pfnEnum, (void*)&HookEnum,     -1, false },
    { "UGCDownload",                  "SteamAPI_ISteamRemoteStorage_UGCDownload",                  0x60, &g_pfnDl,   (void*)&HookDownload, -1, true  },
    { "UGCRead",                      "SteamAPI_ISteamRemoteStorage_UGCRead",                      0x6C, &g_pfnRead, (void*)&HookRead,     -1, true  },
    { "GetUGCDetails",                "SteamAPI_ISteamRemoteStorage_GetUGCDetails",                0x68, &g_pfnDet,  (void*)&HookDetails,  -1, true  },
};

void* ResolveInterfaceObject(HMODULE hApi)
{
    typedef void* (__cdecl * FnCreateInterface)(const char*);
    typedef void* (__cdecl * FnRemoteStorageV016)();

    FnRemoteStorageV016 rsV016 = (FnRemoteStorageV016)GetProcAddress(hApi, "SteamAPI_SteamRemoteStorage_v016");
    FnCreateInterface   ci     = (FnCreateInterface)GetProcAddress(hApi, "SteamInternal_CreateInterface");

    void* raw[2] = { nullptr, nullptr };
    int   nraw   = 0;

    if (rsV016) {
        __try { raw[nraw] = rsV016(); } __except (EXCEPTION_EXECUTE_HANDLER) { raw[nraw] = nullptr; }
        if (raw[nraw]) nraw++;
    }
    if (nraw == 0 && ci) {
        __try { raw[nraw] = ci("STEAMREMOTESTORAGE_INTERFACE_VERSION016"); }
        __except (EXCEPTION_EXECUTE_HANDLER) { raw[nraw] = nullptr; }
        if (raw[nraw]) nraw++;
    }

    for (int i = 0; i < nraw; i++) {
        void** vtA = nullptr;
        __try { vtA = *(void***)raw[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { vtA = nullptr; }
        if (LooksLikeVtable(vtA, 0x9C)) return raw[i];

        // Some wrappers hand back a pointer-to-pointer instead of the object.
        void* objB = nullptr;
        __try { objB = *(void**)raw[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { objB = nullptr; }
        if (objB) {
            void** vtB = nullptr;
            __try { vtB = *(void***)objB; } __except (EXCEPTION_EXECUTE_HANDLER) { vtB = nullptr; }
            if (LooksLikeVtable(vtB, 0x9C)) return objB;
        }
    }
    return nullptr;
}

bool TryInstall()
{
    if (g_installed) return true;

    HMODULE hApi = GetModuleHandleA("steam_api.dll");
    if (!hApi) return false;

    typedef unsigned int(__cdecl * FnGetHSteamPipe)();
    FnGetHSteamPipe getPipe = (FnGetHSteamPipe)GetProcAddress(hApi, "SteamAPI_GetHSteamPipe");
    if (!getPipe) return false;

    unsigned int pipe = 0;
    __try { pipe = getPipe(); } __except (EXCEPTION_EXECUTE_HANDLER) { pipe = 0; }
    if (pipe == 0) return false;   // SteamAPI_Init has not finished yet

    void* obj = ResolveInterfaceObject(hApi);
    if (!obj) return false;

    void** vt = nullptr;
    __try { vt = *(void***)obj; } __except (EXCEPTION_EXECUTE_HANDLER) { vt = nullptr; }
    if (!vt) return false;

    Logf("INIT   steam_api pipe=%u object=%p vtable=%p", pipe, obj, (void*)vt);
    if (g_logVtable) {
        for (int i = 0; i < 8; i++) Logf("VTBL   [%02X] = %p", i * 4, vt[i]);
    }

    int installed = 0;
    int attempted = 0;
    int usedOffset[8] = {};
    int usedCount = 0;

    for (Slot& s : g_slots) {
        if (s.diag && g_diagHooks == 0) {
            Logf("HOOK   %-28s diagnostic hook, skipped (diag_hooks=0)", s.label);
            continue;
        }
        attempted++;

        // The known offsets were verified against the byte pattern of the flat
        // wrappers in bin\steam_api.dll, they are authoritative. A parsed value
        // that disagrees with them means the parser latched onto some other
        // indirect call inside the wrapper (that is exactly how slot 0x68 got
        // hooked twice in an earlier build), so it is never trusted blindly.
        int parsedOff = -1;
        void* flat = (void*)GetProcAddress(hApi, s.exportName);
        bool parsed = flat && ParseVtableOffset(flat, &parsedOff);

        int off = s.knownOffset;
        if (parsed && parsedOff != s.knownOffset) {
            Logf("HOOK   %-28s wrapper says %#x, keeping verified %#x",
                 s.label, parsedOff, s.knownOffset);
        } else if (!parsed) {
            Logf("HOOK   %-28s wrapper unparsed, using verified %#x", s.label, s.knownOffset);
        }

        bool duplicate = false;
        for (int i = 0; i < usedCount; i++) {
            if (usedOffset[i] == off) { duplicate = true; break; }
        }
        if (duplicate) {
            Logf("HOOK   %-28s slot %#x already taken, skipped", s.label, off);
            continue;
        }

        if (PatchVtable(vt, off, s.hook, s.pfn, s.label)) {
            s.usedOffset = off;
            usedOffset[usedCount++] = off;
            installed++;
            Logf("HOOK   %-28s offset %#x -> original %p", s.label, off, *s.pfn);
        } else {
            // No parsed fallback on purpose. The parsed offset is a cross-check,
            // not a trusted source: installing a hook on a slot that belongs to
            // another function would feed a rewritten argument into that
            // function (the GPD hook writes its third parameter, so an unrelated
            // call would get 604800 in its argument slot). A skipped slot only
            // costs speed; a misplaced one corrupts a call.
            Logf("HOOK   %-28s slot %#x refused, left stock", s.label, off);
        }
    }

    if (installed == 0) {
        Logf("INIT   !! no hook installed, keeping stock behaviour");
        return false;
    }

    InterlockedExchange(&g_installed, 1);
    Logf("INIT   done: %d/%d slots hooked, mode=%d, forceMaxAge=%u, diag_hooks=%d",
         installed, attempted, g_mode, g_forceMaxAge, g_diagHooks);
    return true;
}

DWORD WINAPI InitThread(LPVOID)
{
    // Give the loader a moment: OnModuleLoaded can arrive while the loader lock is
    // still held, and the worker should not touch anything during that window.
    Sleep(150);

    for (int i = 0; i < 900; i++) {   // up to 90 s
        if (TryInstall()) return 0;
        Sleep(100);
    }
    Logf("INIT   !! gave up after 90 s, keeping stock behaviour");
    return 0;
}

DWORD WINAPI StatsThread(LPVOID)
{
    LONG lastG = -1, lastE = -1;
    for (;;) {
        Sleep(3000);
        LONG g = g_cntGpd, e = g_cntEnum;
        if (g != lastG || e != lastE) {
            Logf("STATS  GPD=%ld ENUM=%ld DL=%ld READ=%ld DETAILS=%ld",
                 g, e, g_cntDl, g_cntRead, g_cntDetails);
            lastG = g;
            lastE = e;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Config / log bootstrap
// ---------------------------------------------------------------------------
void BuildPaths()
{
    wchar_t dll[MAX_PATH] = {};
    GetModuleFileNameW(g_hSelf, dll, MAX_PATH);

    // The extension dot must be in the file name, not in a folder name.
    wchar_t* slash = wcsrchr(dll, L'\\');
    wchar_t* dot = wcsrchr(slash ? slash : dll, L'.');
    size_t stem = dot ? (size_t)(dot - dll) : wcslen(dll);

    wcsncpy_s(g_logPath, dll, stem);
    wcscat_s(g_logPath, L".log");
    wcsncpy_s(g_iniPath, dll, stem);
    wcscat_s(g_iniPath, L".ini");
}

std::string WideToUtf8(const wchar_t* w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    s.resize((size_t)n - 1);
    return s;
}

// ---------------------------------------------------------------------------
// Config file: INI syntax, read and written here rather than by the Windows
// profile API.
//
// GetPrivateProfileStringA / WritePrivateProfileStringA are ANSI only: when they
// rewrite a UTF-8 file, Chinese comments can come back mangled, and a UTF-8 BOM
// hides the first [section] header from them. So:
//   - Encoding read: UTF-8 with or without BOM, plain ANSI/GBK, or UTF-16 LE
//     with BOM (Notepad "Unicode"). Keys and values are ASCII; comment lines
//     (; or #) and inline comments (" ; ..." after a value) may contain anything.
//   - The generated file is UTF-8 with a BOM and CRLF line endings: the BOM makes
//     every Windows editor, old Notepad included, show the Chinese comments
//     correctly whatever the system language is.
//   - Writing changes exactly one value in place and keeps every other byte:
//     comments, blank lines, line endings, BOM / UTF-16. It goes through a temp
//     file + MoveFileEx, so a crash mid-write cannot leave a truncated config.
// ---------------------------------------------------------------------------
namespace cfg {

// The comments below must reach the file as UTF-8 whatever machine builds this.
static_assert(sizeof("中") == 4, "compile with /utf-8 (see build.bat)");

const char kSection[] = "probe";

// Written on first launch, so installing the DLL alone is enough.
const char kDefaultIni[] = R"INI(; workshop_fastload 配置文件
; 第一次启动时自动生成，删掉后下次启动会重新生成。改完下次启动游戏生效。
; 分号 ; 或井号 # 开头的行是注释，也可以写在值后面（值和分号之间留个空格）。

[probe]

; 工作模式
;   1 = 加速（默认）：除豁免项外，创意工坊 mod 走 Steam 本地缓存，不再每次启动都去服务器检查
;   0 = 只记录：什么都不改，只在日志里记下每次查询，用于排查问题
; 这一项缺失时按 0 处理。
mode=1

; 缓存多久算新
;   never  = 永不过期（默认）：mod 不再自己检查更新，想更新时用下面的 check_all_next_launch
;   秒数   = 60 到 2147483647 之间，例如 604800（7 天）：超过这个时间的条目会重新去服务器检查，
;            那次启动会慢一分钟左右
max_seconds_old=never

; 下次启动检查全部 mod 更新（一次性）
;   1 = 下次启动对所有 mod 完整检查一次（慢一分钟左右），下载有更新的 mod，然后自动改回 0
; 平时不用手改：在 l4n 的 HUD 菜单"插件 → 创意工坊Mod检查"里切换即可。
check_all_next_launch=0

; 每次启动都照常检查更新的 mod，逗号分隔，数量不限
; ID 是创意工坊网址里 ?id= 后面的数字，也是 addons\workshop\<ID>.vpk 的文件名。
; 每加一个，启动大约慢 0.3 秒。l4n（3558049615）和 skeeto（3787239937）已内置，不用填。
; 去掉行首的分号即可启用：
;bypass_ids=1234567890,2345678901

; 以下是诊断选项，一般不用动

; 启动时把 Steam 接口的前 8 个虚函数地址写进日志
log_vtable=1

; 额外记录下载相关的三个接口（UGCDownload / UGCRead / GetUGCDetails），只记录不修改
diag_hooks=0
)INI";

std::string DefaultText()
{
    std::string t;
    for (const char* p = kDefaultIni; *p; p++) {
        if (*p == '\r') continue;
        if (*p == '\n') t += '\r';
        t += *p;
    }
    return t;
}

struct File {
    std::string text;      // UTF-8 / ANSI bytes, BOM removed
    bool bom     = false;  // had a UTF-8 BOM
    bool utf16   = false;  // was UTF-16 LE (converted to UTF-8 in text)
    bool found   = false;  // the file exists and was read
    bool missing = false;  // the file does not exist (as opposed to: unreadable)

    bool Blank() const { return found && text.find_first_not_of(" \t\r\n") == std::string::npos; }
};

bool ReadBytes(const wchar_t* path, std::string& out, DWORD* err)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *err = GetLastError();
        return false;
    }
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (16 << 20);
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out.empty() || (ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr) && got == out.size());
    }
    CloseHandle(h);
    return ok;
}

File Load(const wchar_t* path)
{
    File f;
    std::string raw;
    DWORD err = 0;
    if (!ReadBytes(path, raw, &err)) {
        f.missing = err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
        return f;
    }
    f.found = true;
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB &&
        (unsigned char)raw[2] == 0xBF) {
        f.bom = true;
        f.text = raw.substr(3);
    } else if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        f.utf16 = true;
        const wchar_t* w = (const wchar_t*)(raw.data() + 2);
        int wn = (int)((raw.size() - 2) / 2);
        int n = WideCharToMultiByte(CP_UTF8, 0, w, wn, nullptr, 0, nullptr, nullptr);
        f.text.resize(n > 0 ? (size_t)n : 0);
        if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, wn, &f.text[0], n, nullptr, nullptr);
    } else {
        f.text.swap(raw);
    }
    return f;
}

bool IEq(const std::string& t, size_t b, size_t e, const char* s)
{
    size_t n = strlen(s);
    return e - b == n && _strnicmp(t.data() + b, s, n) == 0;
}

void TrimRange(const std::string& t, size_t& b, size_t& e)
{
    while (b < e && (t[b] == ' ' || t[b] == '\t')) b++;
    while (e > b && (t[e - 1] == ' ' || t[e - 1] == '\t')) e--;
}

// Finds `key` inside [section]. On success returns the value's byte range in
// t, without surrounding blanks and without an inline comment. *sectionEnd (if
// given) receives the offset just past the section header line, or npos when
// the section does not exist.
bool Find(const std::string& t, const char* key, size_t* vb, size_t* ve, size_t* sectionEnd = nullptr)
{
    bool inSec = false;
    if (sectionEnd) *sectionEnd = std::string::npos;
    size_t pos = 0;
    while (pos < t.size()) {
        size_t eol = t.find('\n', pos);
        if (eol == std::string::npos) eol = t.size();
        size_t a = pos, le = eol;
        if (le > a && t[le - 1] == '\r') le--;
        TrimRange(t, a, le);
        if (a < le && t[a] == '[') {
            size_t c = t.find(']', a);
            size_t nb = a + 1, ne = (c != std::string::npos && c < le) ? c : a + 1;
            TrimRange(t, nb, ne);
            inSec = c != std::string::npos && c < le && IEq(t, nb, ne, kSection);
            if (inSec && sectionEnd) *sectionEnd = eol < t.size() ? eol + 1 : t.size();
        } else if (inSec && a < le && t[a] != ';' && t[a] != '#') {
            size_t eq = t.find('=', a);
            if (eq != std::string::npos && eq < le) {
                size_t kb = a, ke = eq;
                TrimRange(t, kb, ke);
                if (IEq(t, kb, ke, key)) {
                    size_t b = eq + 1, e = le;
                    for (size_t i = b; i < le; i++) {   // inline comment: ; or # after a blank
                        if ((t[i] == ';' || t[i] == '#') && (i == b || t[i - 1] == ' ' || t[i - 1] == '\t')) {
                            e = i;
                            break;
                        }
                    }
                    TrimRange(t, b, e);
                    if (e - b >= 2 && (t[b] == '"' || t[b] == '\'') && t[e - 1] == t[b]) { b++; e--; }
                    *vb = b;
                    *ve = e;
                    return true;
                }
            }
        }
        pos = eol + 1;
    }
    return false;
}

std::string Get(const File& f, const char* key, const char* def)
{
    size_t b = 0, e = 0;
    if (!Find(f.text, key, &b, &e)) return def;
    return f.text.substr(b, e - b);
}

int GetInt(const File& f, const char* key, int def)
{
    std::string v = Get(f, key, "");
    if (v.empty()) return def;
    char* end = nullptr;
    long n = strtol(v.c_str(), &end, 10);
    return (*end == '\0') ? (int)n : def;
}

// Temp file + rename: a crash mid-write cannot leave a truncated config.
// With replace=false an existing file is left alone.
bool WriteAtomic(const wchar_t* path, const std::string& bytes, bool replace)
{
    std::wstring tmp = std::wstring(path) + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = WriteFile(h, bytes.data(), (DWORD)bytes.size(), &wrote, nullptr) && wrote == bytes.size();
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp.c_str(), path, MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0)) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

// A missing or blank file is replaced by the defaults. A file that exists but
// cannot be read (locked by an editor, say) is never overwritten: this launch
// runs on the defaults and the user's file stays as it is. Either way the
// defaults apply, so a config problem cannot quietly turn the plugin off.
File LoadOrCreate(const wchar_t* path)
{
    File f = Load(path);
    if (f.found && !f.Blank()) return f;

    File def;
    def.text  = DefaultText();
    def.bom   = true;
    def.found = true;
    if (f.missing || f.Blank()) {
        bool wrote = WriteAtomic(path, std::string("\xEF\xBB\xBF", 3) + def.text, f.Blank());
        strcpy_s(g_cfgNote, wrote ? "created the default config file"
                                  : "!! could not create the config file, running on defaults");
    } else {
        strcpy_s(g_cfgNote, "!! config file exists but cannot be read, running on defaults");
    }
    return def;
}

bool Set(const wchar_t* path, const char* key, const char* value)
{
    File f = Load(path);
    if (!f.found && !f.missing) return false;   // unreadable: do not clobber it
    if (f.missing || f.Blank()) {
        f.text  = DefaultText();
        f.bom   = true;
        f.utf16 = false;
        f.found = true;
    }
    const char* nl = f.text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    size_t b = 0, e = 0, secEnd = std::string::npos;
    if (Find(f.text, key, &b, &e, &secEnd)) {
        f.text.replace(b, e - b, value);
    } else if (secEnd != std::string::npos) {
        std::string line = std::string(key) + "=" + value + nl;
        if (secEnd > 0 && f.text[secEnd - 1] != '\n') line = nl + line;
        f.text.insert(secEnd, line);
    } else {
        if (!f.text.empty() && f.text.back() != '\n') f.text += nl;
        f.text += std::string("[") + kSection + "]" + nl + key + "=" + value + nl;
    }

    std::string out;
    if (f.utf16) {
        int wn = MultiByteToWideChar(CP_UTF8, 0, f.text.data(), (int)f.text.size(), nullptr, 0);
        out.assign("\xFF\xFE", 2);
        out.resize(2 + (size_t)wn * 2);
        if (wn > 0) MultiByteToWideChar(CP_UTF8, 0, f.text.data(), (int)f.text.size(),
                                        (wchar_t*)&out[2], wn);
    } else {
        if (f.bom) out.assign("\xEF\xBB\xBF", 3);
        out += f.text;
    }

    return WriteAtomic(path, out, true);
}

}  // namespace cfg

void LoadConfig()
{
    cfg::File ini = cfg::LoadOrCreate(g_iniPath);

    g_mode      = cfg::GetInt(ini, "mode", 0);
    g_logVtable = cfg::GetInt(ini, "log_vtable", 1);
    g_diagHooks = cfg::GetInt(ini, "diag_hooks", 0);
    if (g_mode < 0 || g_mode > 1) g_mode = 0;
    if (g_diagHooks != 0) g_diagHooks = 1;

    // max_seconds_old is parsed from its raw text on purpose: as an int, "-1"
    // would arrive as 4294967295 and "99999999999" would silently wrap to
    // 1215752191, about 38 years. Accepted: the word "never" (-> kNeverMaxAge),
    // or a value that parses cleanly and lands in [60, 0x7FFFFFFF]; anything
    // else falls back and says so. The upper bound stays inside signed range for
    // the same reason kNeverMaxAge does.
    char ageBuf[64] = {};
    strncpy_s(ageBuf, cfg::Get(ini, "max_seconds_old", "604800").c_str(), _TRUNCATE);
    unsigned long long age = 0;
    if (_strnicmp(ageBuf, "never", 5) == 0 &&
        (ageBuf[5] == '\0' || ageBuf[5] == ' ' || ageBuf[5] == '\t' || ageBuf[5] == ';')) {
        age = kNeverMaxAge;
    } else {
        char* ageEnd = nullptr;
        age = _strtoui64(ageBuf, &ageEnd, 10);
        bool ageComplete = (ageEnd != ageBuf) &&
            (*ageEnd == '\0' || *ageEnd == ' ' || *ageEnd == '\t' || *ageEnd == ';');
        if (!ageComplete || age < 60ULL || age > (unsigned long long)kNeverMaxAge) {
            _snprintf_s(g_cfgWarn, sizeof(g_cfgWarn), _TRUNCATE,
                        "max_seconds_old=\"%s\" is neither \"never\" nor a plain number in "
                        "[60, 2147483647], using 604800", ageBuf);
            age = 604800ULL;
        }
    }
    g_forceMaxAge = (unsigned int)age;

    g_fullCheckThisLaunch = cfg::GetInt(ini, "check_all_next_launch", 0) ? 1 : 0;

    // bypass_ids=1234567890,2345678901 - comma / space / tab separated, appended
    // to the built-in list. No length limit: the whole line is read. An entry
    // that is not a plain number is skipped and reported; the rest of the line
    // is still read.
    std::string ids = cfg::Get(ini, "bypass_ids", "");
    int bad = 0;
    char firstBad[48] = {};
    const char* p = ids.c_str();
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p || *p == ';') break;   // end of value, or an inline comment
        const char* tok = p;
        while (*p && *p != ' ' && *p != '\t' && *p != ',' && *p != ';') p++;
        char* end = nullptr;
        unsigned long long v = _strtoui64(tok, &end, 10);
        if (end != p || v == 0) {
            if (bad++ == 0) {
                size_t len = (size_t)(p - tok);
                if (len >= sizeof(firstBad)) len = sizeof(firstBad) - 1;
                memcpy(firstBad, tok, len);
            }
            continue;
        }
        bool dup = false;
        for (std::uint64_t have : g_bypass) {
            if (have == v) { dup = true; break; }
        }
        if (!dup) g_bypass.push_back(v);
    }
    if (bad) {
        _snprintf_s(g_bypassWarn, sizeof(g_bypassWarn), _TRUNCATE,
                    "bypass_ids: %d entr%s skipped, not a workshop ID (first: \"%s\")",
                    bad, bad == 1 ? "y" : "ies", firstBad);
    }
}

BypassKind BypassKindOf(std::uint64_t id)
{
    for (size_t i = 0; i < g_bypass.size(); i++) {
        if (g_bypass[i] == id) return (int)i < kBuiltinBypassCount ? BYPASS_BUILTIN : BYPASS_USER;
    }
    return BYPASS_NONE;
}

void OpenLog()
{
    g_hLog = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_hLog == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    Logf("==== workshop_fastload start %04d-%02d-%02d %02d:%02d:%02d ====",
         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    Logf("log: %s", WideToUtf8(g_logPath).c_str());
    if (g_cfgNote[0]) Logf("CFG    %s", g_cfgNote);
    Logf("mode=%d (%s), forceMaxAge=%u%s", g_mode,
         g_mode == 1 ? "rewrite unMaxSecondsOld" : "log only", g_forceMaxAge,
         g_forceMaxAge == kNeverMaxAge ? " (never)" : "");
    Logf("diag_hooks=%d (%s)", g_diagHooks,
         g_diagHooks ? "UGC* diagnostic hooks installed too"
                     : "only GetPublishedFileDetails + EnumerateUserSubscribedFiles");
    if (g_cfgWarn[0]) Logf("CFG    %s", g_cfgWarn);
    if (g_bypassWarn[0]) Logf("CFG    %s", g_bypassWarn);
    for (size_t i = 0; i < g_bypass.size(); i++) {
        Logf("bypass id=%llu keeps the caller's window (%s)", (unsigned long long)g_bypass[i],
             (int)i < kBuiltinBypassCount ? "built in" : "bypass_ids");
    }
}

volatile LONG g_bootstrapped = 0;

void Bootstrap()
{
    if (InterlockedCompareExchange(&g_bootstrapped, 1, 0) != 0) return;

    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpcBase);
    InitializeCriticalSection(&g_cs);
    InterlockedExchange(&g_csReady, 1);

    BuildPaths();
    LoadConfig();
    OpenLog();

    LONG stillSet = 0;
    if (g_fullCheckThisLaunch) {
        // Consume the one-shot flag now. If the hooks never install, the stock
        // engine does a full check anyway, so clearing early loses nothing and
        // keeps the HUD menu from racing with this write later.
        bool cleared = cfg::Set(g_iniPath, "check_all_next_launch", "0");
        Logf("FULL   check_all_next_launch=1: this launch checks every item live%s",
             cleared ? ", flag cleared" : ", !! could not clear the flag in the ini");
        stillSet = cleared ? 0 : 1;
    }
    InterlockedExchange(&g_fullCheckNext, stillSet);

    CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, StatsThread, nullptr, 0, nullptr);
}

// ---------------------------------------------------------------------------
// HUD menu (l4n interface v2)
//
// The workshop check runs at the very start of a launch, before any menu can be
// opened, so the switch can only ever mean "on the NEXT launch". It writes
// check_all_next_launch to the ini; Bootstrap consumes it on the following start.
//
// l4n parses the returned KeyValues text and calls "callback" entries as
// `const char* cb(void* user_data)` (plain cdecl, per l4n_plugin.h). What a
// callback returns, as observed in left4neko.dll 2.51:
//   - null: nothing happens, the menu stays as it is
//   - a menu: pushed as a new level, UNLESS the clicked item allows in-place
//     refresh and the menu carries "_refresh" "1"; then it replaces the current
//     level. Items of the menu handed out by RequestHudMenu are parsed with
//     in-place refresh disabled; items of menus returned by callbacks have it
//     enabled. So the first toggle click from the top level pushes one level,
//     and further clicks inside it redraw in place.
// Menu strings are UTF-8 because build.bat compiles with /utf-8.
// ---------------------------------------------------------------------------
const char* __cdecl MenuToggleNextFullCheck(void* user_data);
const char* __cdecl MenuStatusNoop(void* user_data);

const char kMenuTitle[] = "创意工坊Mod检查";

const char* BuildMenu(bool refresh)
{
    // Two buffers, alternated: the string handed out last time stays intact while
    // l4n may still be reading it.
    static char s_buf[2][1024];
    static LONG s_idx = 0;
    char* out = s_buf[InterlockedIncrement(&s_idx) & 1];

    char status[192];
    LONG cached = g_cntCached, live = g_cntLive;
    if (!g_installed) {
        strcpy_s(status, "本次启动：插件未生效，按原版检查");
    } else if (g_mode == 0) {
        _snprintf_s(status, sizeof(status), _TRUNCATE,
                    "本次启动：仅记录模式，%ld 个全部照常检查", live);
    } else if (g_fullCheckThisLaunch) {
        _snprintf_s(status, sizeof(status), _TRUNCATE,
                    "本次启动：已完整检查全部 %ld 个物品", cached + live);
    } else {
        _snprintf_s(status, sizeof(status), _TRUNCATE,
                    "本次启动：%ld 个走缓存，%ld 个照常检查", cached, live);
    }

    _snprintf_s(out, sizeof(s_buf[0]), _TRUNCATE,
                "\"%s\" {\n"
                "%s"
                "  \"下次启动检查全部 mod 更新：%s\" { \"callback\" \"0x%08X\" }\n"
                "  \"%s\" { \"callback\" \"0x%08X\" }\n"
                "}\n",
                kMenuTitle,
                refresh ? "  \"_refresh\" \"1\"\n" : "",
                g_fullCheckNext ? "开" : "关",
                (unsigned int)(std::uintptr_t)&MenuToggleNextFullCheck,
                status,
                (unsigned int)(std::uintptr_t)&MenuStatusNoop);
    return out;
}

const char* __cdecl MenuToggleNextFullCheck(void* /*user_data*/)
{
    if (!g_iniPath[0]) BuildPaths();
    LONG next = g_fullCheckNext ? 0 : 1;
    if (cfg::Set(g_iniPath, "check_all_next_launch", next ? "1" : "0")) {
        InterlockedExchange(&g_fullCheckNext, next);
        Logf("MENU   next launch full check -> %s", next ? "on" : "off");
    } else {
        Logf("MENU   !! could not write check_all_next_launch, err=%lu", GetLastError());
    }
    return BuildMenu(true);
}

// The status line is information only. Returning null tells l4n "no submenu":
// it leaves the menu as it is (verified in left4neko.dll 2.51: a null result
// skips both the push and the in-place refresh). Returning a menu here would be
// pushed as a new level, because items of the menu handed out by RequestHudMenu
// are parsed with in-place refresh disabled.
const char* __cdecl MenuStatusNoop(void* /*user_data*/)
{
    return nullptr;
}

// ---------------------------------------------------------------------------
// Plugin object
// ---------------------------------------------------------------------------
class WorkshopFastloadPlugin : public IL4NPlugin {
public:
    unsigned int GetInterfaceVersion() override { return 2; }
    const char* GetName() override { return "WorkshopFastload"; }
    const char* GetVersion() override { return "0.1.1"; }

    void OnModuleLoaded(const char* module_name, std::uintptr_t /*handle*/) override
    {
        if (module_name && strcmp(module_name, "engine") == 0) Bootstrap();
    }

    void OnGameLaunch() override { Bootstrap(); }

    const char* RequestHudMenu(bool request_title) override
    {
        if (request_title) return kMenuTitle;
        return BuildMenu(false);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) IL4NPlugin* GetL4NPluginInstance()
{
    static WorkshopFastloadPlugin instance;
    return &instance;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hSelf = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}
