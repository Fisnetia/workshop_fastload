// test_runner.cpp - offline harness for workshop_fastload.dll.
//
// Loads the fake steam_api.dll, loads the real plugin against it, lets the
// plugin patch the fake vtable, then calls through the hooked slots and reports
// what both the hook and the "original" function observed.
//
// usage: test_runner.exe <expectedMaxAgeSeenByOriginal> [v016mode]
//        expectedMaxAge is what OrigGPD should have received after the hook ran:
//          mode=0 -> the original maxAge (we always pass 0)
//          mode=1 -> whatever max_seconds_old says (default 604800)
//        v016mode 0 (default) = v016 returns the object pointer, which is what
//        the real steam_api.dll does; 1 = pointer-to-pointer (compat branch).
//
// Covers all three hooked slots, so the two thunks that had never been executed
// before (EnumerateUserSubscribedFiles, UGCDownload) are exercised as well.
//
// If workshop_fastload.ini has check_all_next_launch=1 when the runner starts, this
// launch must forward the caller's window for every item (pass expected=0) and
// the plugin must have cleared the flag. The HUD menu is exercised every run:
// its toggle callback must flip the flag in the ini and back.

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// IL4NPluginV1 vtable order on MSVC x86:
//   0 scalar deleting destructor
//   1 GetInterfaceVersion
//   2 GetName
//   3 GetVersion
//   4 OnModuleLoaded
//   5 OnGameLaunch
// so OnModuleLoaded is index 4 == byte offset 16.
extern "C" __declspec(naked) void CallOnModuleLoaded(void* self, const char* name)
{
    __asm {
        push ebp
        mov  ebp, esp
        push 0                         // std::uintptr_t handle
        push dword ptr [ebp + 12]      // const char* module_name
        mov  ecx, dword ptr [ebp + 8]  // this
        mov  eax, dword ptr [ecx]      // vtable
        call dword ptr [eax + 16]      // [4] OnModuleLoaded, thiscall: callee pops 8
        pop  ebp
        ret
    }
}

// IL4NPlugin (v2) adds RequestHudMenu after the six V1 entries above plus
// OnD3DCreated [6] and OnD3DDeviceCreated [7]: index 8 == byte offset 32.
extern "C" __declspec(naked) const char* CallRequestHudMenu(void* self, int requestTitle)
{
    __asm {
        push ebp
        mov  ebp, esp
        push dword ptr [ebp + 12]      // bool request_title, passed as a dword
        mov  ecx, dword ptr [ebp + 8]  // this
        mov  eax, dword ptr [ecx]
        call dword ptr [eax + 32]      // [8] RequestHudMenu, thiscall: callee pops 4
        pop  ebp
        ret
    }
}

typedef const char* (__cdecl* FnMenuCallback)(void*);

static char g_ini[MAX_PATH];

// Independent minimal reader (not the plugin's): the ini may be UTF-8 with or
// without BOM, ANSI, or UTF-16 LE, and the Windows profile API cannot be trusted
// with the first two. Returns the value of `key` (inline comment stripped), or
// def when missing.
static std::string IniValue(const char* key, const char* def)
{
    std::string raw;
    FILE* f = fopen(g_ini, "rb");
    if (!f) return def;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) raw.append(chunk, n);
    fclose(f);
    if (raw.size() >= 3 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0) raw.erase(0, 3);
    else if (raw.size() >= 2 && raw.compare(0, 2, "\xFF\xFE") == 0) {
        const wchar_t* w = (const wchar_t*)(raw.data() + 2);
        int wn = (int)((raw.size() - 2) / 2);
        int un = WideCharToMultiByte(CP_UTF8, 0, w, wn, nullptr, 0, nullptr, nullptr);
        std::string u(un > 0 ? un : 0, '\0');
        if (un > 0) WideCharToMultiByte(CP_UTF8, 0, w, wn, &u[0], un, nullptr, nullptr);
        raw.swap(u);
    }
    size_t klen = strlen(key);
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t eol = raw.find('\n', pos);
        if (eol == std::string::npos) eol = raw.size();
        std::string line = raw.substr(pos, eol - pos);
        pos = eol + 1;
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos || line.compare(a, klen, key) != 0) continue;
        size_t eq = line.find_first_not_of(" \t", a + klen);
        if (eq == std::string::npos || line[eq] != '=') continue;
        std::string v = line.substr(eq + 1);
        size_t c = v.find(" ;");
        if (c != std::string::npos) v.erase(c);
        while (!v.empty() && strchr(" \t\r", v.back())) v.pop_back();
        size_t s = v.find_first_not_of(" \t");
        return s == std::string::npos ? std::string() : v.substr(s);
    }
    return def;
}

static int IniFlag()
{
    return atoi(IniValue("check_all_next_launch", "0").c_str());
}

// n-th `"callback" "0x...."` address in a menu string, or 0.
static unsigned int MenuCallback(const char* menu, int n)
{
    const char* key = "\"callback\" \"0x";
    const char* p = menu;
    for (int i = 0; p && (p = strstr(p, key)) != nullptr; i++) {
        p += strlen(key);
        if (i == n) return (unsigned int)strtoul(p, nullptr, 16);
    }
    return 0;
}

// Braces balance and quotes pair up - the minimum l4n's KeyValues parser needs.
static bool MenuWellFormed(const char* menu)
{
    int depth = 0, quotes = 0;
    for (const char* p = menu; *p; p++) {
        if (*p == '"') quotes++;
        else if (!(quotes & 1) && *p == '{') depth++;
        else if (!(quotes & 1) && *p == '}') { if (--depth < 0) return false; }
    }
    return depth == 0 && (quotes % 2) == 0;
}

typedef void* (__cdecl* FnGetInstance)();
typedef unsigned long long (__cdecl* FnCallGPD)(std::uint64_t, unsigned int);
typedef void (__cdecl* FnCallEnum)(unsigned int);
typedef void (__cdecl* FnCallDownload)(unsigned long long, unsigned int);
typedef unsigned long long (__cdecl* FnSeenId)();
typedef unsigned int (__cdecl* FnSeenU32)();
typedef void (__cdecl* FnSetMode)(int);
typedef void* (__cdecl* FnPtr)();

int main(int argc, char** argv)
{
    unsigned int expected = 0;
    if (argc > 1) expected = (unsigned int)strtoul(argv[1], nullptr, 10);
    int v016Mode = 0;
    if (argc > 2) v016Mode = (int)strtoul(argv[2], nullptr, 10);

    GetFullPathNameA("workshop_fastload.ini", MAX_PATH, g_ini, nullptr);
    const int fullCheckRequested = IniFlag();
    printf("check_all_next_launch at start: %d\n", fullCheckRequested);

    HMODULE hFake = LoadLibraryA("steam_api.dll");
    if (!hFake) {
        printf("FAIL load fake steam_api.dll (err=%lu)\n", GetLastError());
        return 2;
    }

    // Must be set before the plugin resolves the interface.
    FnSetMode setV016 = (FnSetMode)GetProcAddress(hFake, "TestSetV016Mode");
    if (!setV016) { printf("FAIL TestSetV016Mode missing\n"); return 2; }
    setV016(v016Mode);
    printf("v016 shape: %s\n", v016Mode ? "pointer-to-pointer" : "object pointer (real)");

    HMODULE hPlug = LoadLibraryA("workshop_fastload.dll");
    if (!hPlug) {
        printf("FAIL load workshop_fastload.dll (err=%lu)\n", GetLastError());
        return 2;
    }

    FnGetInstance getInstance = (FnGetInstance)GetProcAddress(hPlug, "GetL4NPluginInstance");
    if (!getInstance) { printf("FAIL GetL4NPluginInstance not exported\n"); return 2; }
    void* plugin = getInstance();
    if (!plugin) { printf("FAIL GetL4NPluginInstance returned null\n"); return 2; }

    FnPtr      testSlot     = (FnPtr)GetProcAddress(hFake, "TestSlot9C");
    FnPtr      testOrig     = (FnPtr)GetProcAddress(hFake, "TestOrigGPD");
    FnPtr      testObj      = (FnPtr)GetProcAddress(hFake, "TestObj");
    FnPtr      testSeenThis = (FnPtr)GetProcAddress(hFake, "TestSeenThis");
    FnCallGPD  callGpd      = (FnCallGPD)GetProcAddress(hFake, "TestCallGPD");
    FnCallEnum callEnum     = (FnCallEnum)GetProcAddress(hFake, "TestCallEnum");
    FnCallDownload callDl   = (FnCallDownload)GetProcAddress(hFake, "TestCallDownload");
    FnSeenId   seenId       = (FnSeenId)GetProcAddress(hFake, "TestSeenId");
    FnSeenU32  seenMaxAge   = (FnSeenU32)GetProcAddress(hFake, "TestSeenMaxAge");
    FnSeenU32  seenEnumIdx  = (FnSeenU32)GetProcAddress(hFake, "TestSeenEnumIndex");
    FnSeenU32  seenDlLo     = (FnSeenU32)GetProcAddress(hFake, "TestSeenDlLo");
    FnSeenU32  seenDlHi     = (FnSeenU32)GetProcAddress(hFake, "TestSeenDlHi");
    FnSeenU32  seenDlPri    = (FnSeenU32)GetProcAddress(hFake, "TestSeenDlPri");
    if (!testSlot || !testOrig || !testObj || !testSeenThis || !callGpd || !callEnum ||
        !callDl || !seenId || !seenMaxAge || !seenEnumIdx || !seenDlLo || !seenDlHi || !seenDlPri) {
        printf("FAIL fake steam_api.dll is missing test exports\n");
        return 2;
    }

    void* slotBefore = testSlot();
    void* orig = testOrig();

    CallOnModuleLoaded(plugin, "engine");

    // The plugin polls for steam_api readiness on a worker thread; give it room.
    void* slotAfter = nullptr;
    for (int i = 0; i < 40; ++i) {
        Sleep(50);
        slotAfter = testSlot();
        if (slotAfter != slotBefore) break;
    }
    if (slotAfter == slotBefore) {
        printf("FAIL hook not installed (slot 0x9C still %p)\n", slotBefore);
        return 1;
    }

    // A second notification (l4n may fire OnModuleLoaded and OnGameLaunch) must
    // not re-enter the bootstrap. Re-entering would patch the slot with the hook
    // that is already in it and leave g_pfn* pointing at the hook itself.
    CallOnModuleLoaded(plugin, "engine");
    Sleep(300);
    void* slotAgain = testSlot();

    int ok = 1;
    if (slotAgain != slotAfter) {
        printf("FAIL not idempotent: slot moved again %p -> %p\n", slotAfter, slotAgain);
        ok = 0;
    }

    const std::uint64_t testId = 1234567890123ULL;
    unsigned long long ret = callGpd(testId, 0);
    unsigned long long gotId = seenId();
    unsigned int gotMaxAge = seenMaxAge();
    void* gotThis = testSeenThis();
    void* expectedThis = testObj();

    printf("slot_original=%p slot_hook=%p orig=%p\n", slotBefore, slotAfter, orig);
    printf("seen_id=%llu seen_maxAge=%u seen_this=%p (expected %p) ret=0x%llX\n",
           (unsigned long long)gotId, gotMaxAge, gotThis, expectedThis,
           (unsigned long long)(ret & 0xFFFFFFFFULL));

    if (gotId != testId) { printf("FAIL id mangled: expected %llu\n", (unsigned long long)testId); ok = 0; }
    if (gotMaxAge != expected) { printf("FAIL maxAge: expected %u got %u\n", expected, gotMaxAge); ok = 0; }
    if (gotThis != expectedThis) { printf("FAIL this/ecx was not restored across the hook\n"); ok = 0; }
    if ((ret & 0xFFFFFFFFULL) != 0x11223344ULL) { printf("FAIL return value not passed through\n"); ok = 0; }

    // Slot 0xAC: EnumerateUserSubscribedFiles(this, uint32 startIndex)
    callEnum(150);
    unsigned int enumIdx = seenEnumIdx();
    void* enumThis = testSeenThis();
    printf("enum startIndex=%u (expected 150) this=%p\n", enumIdx, enumThis);
    if (enumIdx != 150) { printf("FAIL enum argument mangled\n"); ok = 0; }
    if (enumThis != expectedThis) { printf("FAIL enum lost ecx\n"); ok = 0; }

    // Slot 0x60: UGCDownload(this, uint64 handle, uint32 priority)
    const unsigned long long handle = 0x1122334455667788ULL;
    callDl(handle, 7);
    unsigned int dlLo = seenDlLo(), dlHi = seenDlHi(), dlPri = seenDlPri();
    void* dlThis = testSeenThis();
    printf("download lo=0x%08X hi=0x%08X pri=%u this=%p\n", dlLo, dlHi, dlPri, dlThis);
    if (dlLo != 0x55667788 || dlHi != 0x11223344) { printf("FAIL download handle mangled\n"); ok = 0; }
    if (dlPri != 7) { printf("FAIL download priority mangled\n"); ok = 0; }
    if (dlThis != expectedThis) { printf("FAIL download lost ecx\n"); ok = 0; }

    // The tooling bypass list must keep the caller's own window, otherwise the
    // update check for l4n / skeeto gets stretched along with everything else.
    if (expected != 0) {
        const std::uint64_t toolIds[] = { 3558049615ULL, 3782512354ULL };
        for (std::uint64_t toolId : toolIds) {
            callGpd(toolId, 0);
            unsigned int bypassMaxAge = seenMaxAge();
            unsigned long long bypassId = seenId();
            printf("bypass id=%llu seen_maxAge=%u (expected 0)\n",
                   (unsigned long long)bypassId, bypassMaxAge);
            if (bypassId != toolId || bypassMaxAge != 0) {
                printf("FAIL bypass id did not keep the original window\n");
                ok = 0;
            }
        }
    }

    // User bypass_ids: every numeric entry in the ini must keep the original
    // window, however many there are and even after a malformed entry.
    if (expected != 0) {
        std::string idv = IniValue("bypass_ids", "");
        std::vector<char> idbuf(idv.begin(), idv.end());
        idbuf.push_back(0);
        char* ids = idbuf.data();
        int checked = 0, wrong = 0;
        for (char* tok = strtok(ids, " ,\t"); tok; tok = strtok(nullptr, " ,\t")) {
            if (*tok == ';') break;
            char* end = nullptr;
            unsigned long long v = _strtoui64(tok, &end, 10);
            if (*end || v == 0) continue;   // malformed on purpose: plugin must skip it
            callGpd(v, 0);
            checked++;
            if (seenMaxAge() != 0) {
                if (wrong++ < 3) printf("FAIL bypass_ids entry %llu was rewritten to %u\n", v, seenMaxAge());
                ok = 0;
            }
        }
        if (checked) printf("bypass_ids: %d entries checked, %d rewritten (expected 0)\n", checked, wrong);
    }

    // One-shot full check: consumed at startup.
    if (fullCheckRequested) {
        int after = IniFlag();
        printf("full check: flag after startup=%d (expected 0)\n", after);
        if (after != 0) { printf("FAIL one-shot flag was not cleared\n"); ok = 0; }
    }

    // HUD menu: title, menu text, and the toggle callback round trip.
    const char* title = CallRequestHudMenu(plugin, 1);
    const char* menu  = CallRequestHudMenu(plugin, 0);
    printf("menu title=%s\n%s", title ? title : "(null)", menu ? menu : "(null)\n");
    if (!title || !*title) { printf("FAIL menu title missing\n"); ok = 0; }
    if (!menu || !MenuWellFormed(menu)) { printf("FAIL menu text malformed\n"); ok = 0; }
    unsigned int toggleAddr  = menu ? MenuCallback(menu, 0) : 0;
    unsigned int refreshAddr = menu ? MenuCallback(menu, 1) : 0;
    if (!toggleAddr || !refreshAddr) {
        printf("FAIL menu callbacks missing\n");
        ok = 0;
    } else {
        int before = IniFlag();
        const char* m1 = ((FnMenuCallback)(std::uintptr_t)toggleAddr)(nullptr);
        int mid = IniFlag();
        // Copy: the plugin alternates two buffers, so m1 survives one more call,
        // but not two.
        static char m1copy[1024];
        strncpy_s(m1copy, m1 ? m1 : "", _TRUNCATE);
        const char* m2 = ((FnMenuCallback)(std::uintptr_t)toggleAddr)(nullptr);
        int end = IniFlag();
        static char m2copy[1024];
        strncpy_s(m2copy, m2 ? m2 : "", _TRUNCATE);
        const char* m3 = ((FnMenuCallback)(std::uintptr_t)refreshAddr)(nullptr);
        printf("toggle: ini %d -> %d -> %d\n", before, mid, end);
        printf("after toggle:\n%s", m1copy);
        if (mid != !before || end != before) { printf("FAIL toggle did not flip the ini flag\n"); ok = 0; }
        if (!strstr(m1copy, "\"_refresh\" \"1\"") || !MenuWellFormed(m1copy) ||
            !MenuWellFormed(m2copy)) {
            printf("FAIL toggle menus must be well formed and carry _refresh\n");
            ok = 0;
        }
        // The status line is information only: its callback must return null so
        // l4n does not push a new menu level.
        if (m3 != nullptr) { printf("FAIL status item must not open a submenu\n"); ok = 0; }
        if (strcmp(m1copy, m2copy) == 0) { printf("FAIL menu label did not follow the toggle\n"); ok = 0; }
    }

    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
