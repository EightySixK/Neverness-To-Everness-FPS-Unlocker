#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "psapi.lib")

static HMODULE g_realDll = NULL;

// Load the real version.dll from system32, for forwarding.
static void LoadRealDll() {
    if (g_realDll) return;
    wchar_t sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    wcscat(sysDir, L"\\version.dll");
    g_realDll = LoadLibraryW(sysDir);
}

// UE5 IConsoleManager::FindConsoleVariable
typedef uintptr_t (*FindConsoleVariable_t)(uintptr_t, const wchar_t*, uint8_t);

// read fps from fpx.txt
static float ReadTargetFPS() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* s = strrchr(path, '\\');
    if (s) *s = '\0';
    strcat(path, "\\fps.txt");

    float fps = 9999.0f;
    FILE* f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%f", &fps) == 1) {
            if (fps < 0) fps = 9999.0f;  // -1 = unlimited
            if (fps < 10.0f) fps = 10.0f;
            if (fps > 9999.0f) fps = 9999.0f;
        }
        fclose(f);
    }
    return fps;
}

static uintptr_t FindCVar(uintptr_t singleton, FindConsoleVariable_t fn, const wchar_t* name) {
    __try {
        return fn(singleton, name, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// memory patch
static void PatchFloat(uintptr_t cvar, size_t offset, float value) {
    __try {
        float* ptr = (float*)(cvar + offset);
        DWORD old;
        if (VirtualProtect(ptr, 4, PAGE_READWRITE, &old)) {
            *ptr = value;
            VirtualProtect(ptr, 4, old, &old);
            FlushInstructionCache(GetCurrentProcess(), ptr, 4);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void PatchInt(uintptr_t cvar, size_t offset, int32_t value) {
    __try {
        int32_t* ptr = (int32_t*)(cvar + offset);
        DWORD old;
        if (VirtualProtect(ptr, 4, PAGE_READWRITE, &old)) {
            *ptr = value;
            VirtualProtect(ptr, 4, old, &old);
            FlushInstructionCache(GetCurrentProcess(), ptr, 4);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static DWORD WINAPI MainThread(LPVOID) {
    Sleep(25000); // wait for engine init

    HMODULE hGame = GetModuleHandleA("HTGame.exe");
    if (!hGame) return 0;

    uintptr_t gameBase = (uintptr_t)hGame;

    // this uses pattern matching to find RVAs
    uintptr_t* singletonPtr = (uintptr_t*)(gameBase + 0xE582ED0);
    FindConsoleVariable_t findFunc = (FindConsoleVariable_t)(gameBase + 0x1459410);

    uintptr_t singleton = 0;
    __try { singleton = *singletonPtr; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (!singleton) return 0;

    // Grab the cvar objects
    uintptr_t tMaxFPS = FindCVar(singleton, findFunc, L"t.MaxFPS");
    uintptr_t rVSync = FindCVar(singleton, findFunc, L"r.VSync");
    if (!tMaxFPS) return 0;

    // actual application of patches
    float targetFPS = ReadTargetFPS();
    PatchFloat(tMaxFPS, 0x50, targetFPS);
    if (rVSync) PatchInt(rVSync, 0x50, 0);

    // loop reappliacation.
    while (true) {
        Sleep(10000);
        float newTarget = ReadTargetFPS();
        if (newTarget != targetFPS) targetFPS = newTarget;
        if (*(float*)(tMaxFPS + 0x50) != targetFPS)
            PatchFloat(tMaxFPS, 0x50, targetFPS);
        if (rVSync && *(int32_t*)(rVSync + 0x50) != 0)
            PatchInt(rVSync, 0x50, 0);
    }
}

// Forward all DLL exports to the actual dll
extern "C" {

BOOL WINAPI GetFileVersionInfoA(LPCSTR a1, DWORD a2, DWORD a3, LPVOID a4) {
    LoadRealDll(); static auto fn = (BOOL(WINAPI*)(LPCSTR,DWORD,DWORD,LPVOID))GetProcAddress(g_realDll,"GetFileVersionInfoA"); return fn?fn(a1,a2,a3,a4):FALSE;
}
BOOL WINAPI GetFileVersionInfoByHandle(HANDLE a1, DWORD a2, DWORD a3, LPVOID a4) {
    LoadRealDll(); static auto fn = (BOOL(WINAPI*)(HANDLE,DWORD,DWORD,LPVOID))GetProcAddress(g_realDll,"GetFileVersionInfoByHandle"); return fn?fn(a1,a2,a3,a4):FALSE;
}
DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR a1, LPDWORD a2) {
    LoadRealDll(); static auto fn = (DWORD(WINAPI*)(LPCSTR,LPDWORD))GetProcAddress(g_realDll,"GetFileVersionInfoSizeA"); return fn?fn(a1,a2):0;
}
DWORD WINAPI GetFileVersionInfoSizeExA(DWORD a1, LPCSTR a2, LPDWORD a3) {
    LoadRealDll(); static auto fn = (DWORD(WINAPI*)(DWORD,LPCSTR,LPDWORD))GetProcAddress(g_realDll,"GetFileVersionInfoSizeExA"); return fn?fn(a1,a2,a3):0;
}
DWORD WINAPI GetFileVersionInfoSizeExW(DWORD a1, LPCWSTR a2, LPDWORD a3) {
    LoadRealDll(); static auto fn = (DWORD(WINAPI*)(DWORD,LPCWSTR,LPDWORD))GetProcAddress(g_realDll,"GetFileVersionInfoSizeExW"); return fn?fn(a1,a2,a3):0;
}
DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR a1, LPDWORD a2) {
    LoadRealDll(); static auto fn = (DWORD(WINAPI*)(LPCWSTR,LPDWORD))GetProcAddress(g_realDll,"GetFileVersionInfoSizeW"); return fn?fn(a1,a2):0;
}
BOOL WINAPI GetFileVersionInfoExA(DWORD a1, LPCSTR a2, DWORD a3, DWORD a4, LPVOID a5) {
    LoadRealDll(); static auto fn = (BOOL(WINAPI*)(DWORD,LPCSTR,DWORD,DWORD,LPVOID))GetProcAddress(g_realDll,"GetFileVersionInfoExA"); return fn?fn(a1,a2,a3,a4,a5):FALSE;
}
BOOL WINAPI GetFileVersionInfoExW(DWORD a1, LPCWSTR a2, DWORD a3, DWORD a4, LPVOID a5) {
    LoadRealDll(); static auto fn = (BOOL(WINAPI*)(DWORD,LPCWSTR,DWORD,DWORD,LPVOID))GetProcAddress(g_realDll,"GetFileVersionInfoExW"); return fn?fn(a1,a2,a3,a4,a5):FALSE;
}
BOOL WINAPI GetFileVersionInfoW(LPCWSTR a1, DWORD a2, DWORD a3, LPVOID a4) {
    LoadRealDll(); static auto fn = (BOOL(WINAPI*)(LPCWSTR,DWORD,DWORD,LPVOID))GetProcAddress(g_realDll,"GetFileVersionInfoW"); return fn?fn(a1,a2,a3,a4):FALSE;
}
DWORD WINAPI VerFindFileA(DWORD a1, LPCSTR a2, LPCSTR a3, LPCSTR a4, LPSTR a5, PUINT a6, LPSTR a7, PUINT a8) {
    LoadRealDll(); static auto fn = (DWORD(WINAPI*)(DWORD,LPCSTR,LPCSTR,LPCSTR,LPSTR,PUINT,LPSTR,PUINT))GetProcAddress(g_realDll,"VerFindFileA"); return fn?fn(a1,a2,a3,a4,a5,a6,a7,a8):0;
}
DWORD WINAPI VerFindFileW(DWORD a1, LPCWSTR a2, LPCWSTR a3, LPCWSTR a4, LPWSTR a5, PUINT a6, LPWSTR a7, PUINT a8) {
    LoadRealDll(); static auto fn = (DWORD(WINAPI*)(DWORD,LPCWSTR,LPCWSTR,LPCWSTR,LPWSTR,PUINT,LPWSTR,PUINT))GetProcAddress(g_realDll,"VerFindFileW"); return fn?fn(a1,a2,a3,a4,a5,a6,a7,a8):0;
}
DWORD WINAPI VerInstallFileA(DWORD a1, LPCSTR a2, LPCSTR a3, LPCSTR a4, LPCSTR a5, LPCSTR a6, LPSTR a7, PUINT a8) {
    LoadRealDll(); static auto fn = (DWORD(WINAPI*)(DWORD,LPCSTR,LPCSTR,LPCSTR,LPCSTR,LPCSTR,LPSTR,PUINT))GetProcAddress(g_realDll,"VerInstallFileA"); return fn?fn(a1,a2,a3,a4,a5,a6,a7,a8):0;
}
DWORD WINAPI VerInstallFileW(DWORD a1, LPCWSTR a2, LPCWSTR a3, LPCWSTR a4, LPCWSTR a5, LPCWSTR a6, LPWSTR a7, PUINT a8) {
    LoadRealDll(); static auto fn = (DWORD(WINAPI*)(DWORD,LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR,LPCWSTR,LPWSTR,PUINT))GetProcAddress(g_realDll,"VerInstallFileW"); return fn?fn(a1,a2,a3,a4,a5,a6,a7,a8):0;
}
DWORD WINAPI VerLanguageNameA(DWORD a1, LPSTR a2, DWORD a3) {
    LoadRealDll(); static auto fn = (DWORD(WINAPI*)(DWORD,LPSTR,DWORD))GetProcAddress(g_realDll,"VerLanguageNameA"); return fn?fn(a1,a2,a3):0;
}
DWORD WINAPI VerLanguageNameW(DWORD a1, LPWSTR a2, DWORD a3) {
    LoadRealDll(); static auto fn = (DWORD(WINAPI*)(DWORD,LPWSTR,DWORD))GetProcAddress(g_realDll,"VerLanguageNameW"); return fn?fn(a1,a2,a3):0;
}
BOOL WINAPI VerQueryValueA(LPCVOID a1, LPCSTR a2, LPVOID* a3, PUINT a4) {
    LoadRealDll(); static auto fn = (BOOL(WINAPI*)(LPCVOID,LPCSTR,LPVOID*,PUINT))GetProcAddress(g_realDll,"VerQueryValueA"); return fn?fn(a1,a2,a3,a4):FALSE;
}
BOOL WINAPI VerQueryValueW(LPCVOID a1, LPCWSTR a2, LPVOID* a3, PUINT a4) {
    LoadRealDll(); static auto fn = (BOOL(WINAPI*)(LPCVOID,LPCWSTR,LPVOID*,PUINT))GetProcAddress(g_realDll,"VerQueryValueW"); return fn?fn(a1,a2,a3,a4):FALSE;
}

}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        LoadRealDll();
        HANDLE mutex = CreateMutexA(NULL, TRUE, "Local\\NTE_FPSUnlocker");
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_realDll) FreeLibrary(g_realDll);
    }
    return TRUE;
}
