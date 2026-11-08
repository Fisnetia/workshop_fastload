// fake_steam_api.cpp - offline stand-in for bin\steam_api.dll.
//
// It publishes a hand-built ISteamRemoteStorage vtable where the three slots the
// plugin hooks (0x9C, 0xAC, 0x60) are thiscall functions with the real
// signatures, so the plugin can be loaded against it and the whole hook chain
// verified without starting the game.
//
// The originals dereference `ecx` as their first instruction, exactly like the
// real steamclient thunks. A hook that fails to restore ecx therefore faults
// here instead of only in the live game.
//
// The per-function flat wrappers (SteamAPI_ISteamRemoteStorage_*) are
// deliberately absent, which exercises the plugin's "runtime offset parse
// failed -> verified offset" path.

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cstdint>

#define VT_SLOTS 64

static void*         g_vt[VT_SLOTS];
static void*         g_obj[1];             // object memory: first dword is the vtable
static void*         g_pp[1];              // pointer-to-object
static std::uint64_t g_seenId;
static unsigned int  g_seenMaxAge;
static void*         g_seenThis;
static unsigned int  g_seenEnumIndex;
static unsigned int  g_seenDlLo, g_seenDlHi, g_seenDlPri;
static unsigned int  g_retValue = 0x11223344;
static int           g_v016Mode = 0;       // 0 = object pointer, 1 = pointer-to-pointer

// --- slot 0x9C: GetPublishedFileDetails(this, uint64 id, uint32 maxAge) ------
extern "C" __declspec(naked) void OrigGPD()
{
    __asm {
        push ebp
        mov  ebp, esp
        mov  dword ptr [g_seenThis], ecx      // record this
        mov  eax, dword ptr [ecx]             // this->vtable (crashes if ecx is junk)
        mov  eax, dword ptr [ebp + 8]         // id low
        mov  dword ptr [g_seenId], eax
        mov  eax, dword ptr [ebp + 12]        // id high
        mov  dword ptr [g_seenId + 4], eax
        mov  eax, dword ptr [ebp + 16]        // unMaxSecondsOld
        mov  dword ptr [g_seenMaxAge], eax
        xor  edx, edx
        mov  eax, dword ptr [g_retValue]
        pop  ebp
        ret  12                               // thiscall: id (8) + maxAge (4)
    }
}

// --- slot 0xAC: EnumerateUserSubscribedFiles(this, uint32 startIndex) --------
extern "C" __declspec(naked) void OrigEnum()
{
    __asm {
        push ebp
        mov  ebp, esp
        mov  dword ptr [g_seenThis], ecx
        mov  eax, dword ptr [ecx]             // must fault if ecx is junk
        mov  eax, dword ptr [ebp + 8]         // startIndex
        mov  dword ptr [g_seenEnumIndex], eax
        xor  edx, edx
        mov  eax, 1
        pop  ebp
        ret  4
    }
}

// --- slot 0x60: UGCDownload(this, uint64 handle, uint32 priority) ------------
extern "C" __declspec(naked) void OrigDownload()
{
    __asm {
        push ebp
        mov  ebp, esp
        mov  dword ptr [g_seenThis], ecx
        mov  eax, dword ptr [ecx]             // must fault if ecx is junk
        mov  eax, dword ptr [ebp + 8]
        mov  dword ptr [g_seenDlLo], eax
        mov  eax, dword ptr [ebp + 12]
        mov  dword ptr [g_seenDlHi], eax
        mov  eax, dword ptr [ebp + 16]
        mov  dword ptr [g_seenDlPri], eax
        xor  edx, edx
        mov  eax, 1
        pop  ebp
        ret  12
    }
}

// Placeholder for the slots the test never calls.
extern "C" __declspec(naked) void Dummy()
{
    __asm {
        xor eax, eax
        ret 32
    }
}

static void BuildVtable()
{
    for (int i = 0; i < VT_SLOTS; ++i) g_vt[i] = (void*)&Dummy;
    g_vt[0x9C / 4] = (void*)&OrigGPD;
    g_vt[0xAC / 4] = (void*)&OrigEnum;
    g_vt[0x60 / 4] = (void*)&OrigDownload;
    g_obj[0] = (void*)g_vt;
    g_pp[0]  = (void*)g_obj;
}

extern "C" __declspec(dllexport) unsigned int SteamAPI_GetHSteamPipe()
{
    return 1;   // pretend SteamAPI_Init() already succeeded
}

// Real steam_api.dll ends this wrapper with `mov eax,[eax]`, so what it hands
// back is the object pointer itself, not a pointer to it (the flat wrapper loads
// the object pointer out of a module-level slot). Default to that shape so the
// harness covers the branch the game actually takes; mode 1 reproduces the
// pointer-to-pointer shape for the compatibility path.
extern "C" __declspec(dllexport) void* SteamAPI_SteamRemoteStorage_v016()
{
    return g_v016Mode ? (void*)g_pp : (void*)g_obj;
}

extern "C" __declspec(dllexport) void TestSetV016Mode(int mode) { g_v016Mode = mode ? 1 : 0; }

// --- callers that go through the vtable, i.e. through whatever is hooked -----
extern "C" __declspec(naked, dllexport) unsigned long long TestCallGPD(std::uint64_t id, unsigned int maxAge)
{
    __asm {
        push ebp
        mov  ebp, esp
        push dword ptr [ebp + 16]            // maxAge
        push dword ptr [ebp + 12]            // id high
        push dword ptr [ebp + 8]             // id low
        mov  ecx, offset g_obj               // this = object memory
        mov  eax, dword ptr [g_obj]          // vtable
        call dword ptr [eax + 156]           // [0x9C / 4]
        pop  ebp
        ret
    }
}

extern "C" __declspec(naked, dllexport) void TestCallEnum(unsigned int startIndex)
{
    __asm {
        push ebp
        mov  ebp, esp
        push dword ptr [ebp + 8]
        mov  ecx, offset g_obj
        mov  eax, dword ptr [g_obj]
        call dword ptr [eax + 172]           // [0xAC / 4]
        pop  ebp
        ret
    }
}

extern "C" __declspec(naked, dllexport) void TestCallDownload(unsigned long long handle, unsigned int priority)
{
    __asm {
        push ebp
        mov  ebp, esp
        push dword ptr [ebp + 16]            // priority
        push dword ptr [ebp + 12]            // handle high
        push dword ptr [ebp + 8]             // handle low
        mov  ecx, offset g_obj
        mov  eax, dword ptr [g_obj]
        call dword ptr [eax + 96]            // [0x60 / 4]
        pop  ebp
        ret
    }
}

extern "C" __declspec(dllexport) unsigned long long TestSeenId()       { return g_seenId; }
extern "C" __declspec(dllexport) unsigned int       TestSeenMaxAge()   { return g_seenMaxAge; }
extern "C" __declspec(dllexport) void*              TestSeenThis()     { return g_seenThis; }
extern "C" __declspec(dllexport) unsigned int       TestSeenEnumIndex(){ return g_seenEnumIndex; }
extern "C" __declspec(dllexport) unsigned int       TestSeenDlLo()     { return g_seenDlLo; }
extern "C" __declspec(dllexport) unsigned int       TestSeenDlHi()     { return g_seenDlHi; }
extern "C" __declspec(dllexport) unsigned int       TestSeenDlPri()    { return g_seenDlPri; }
extern "C" __declspec(dllexport) void*              TestObj()          { return g_obj; }
extern "C" __declspec(dllexport) void*              TestObjPP()        { return g_pp; }
extern "C" __declspec(dllexport) void*              TestSlot9C()       { return g_vt[0x9C / 4]; }
extern "C" __declspec(dllexport) void*              TestOrigGPD()      { return (void*)&OrigGPD; }

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        BuildVtable();
    }
    return TRUE;
}
