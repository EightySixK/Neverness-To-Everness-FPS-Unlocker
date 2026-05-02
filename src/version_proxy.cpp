#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

#pragma comment(lib, "psapi.lib")

static HMODULE g_realDll = NULL;

// Load the real version.dll
static void LoadRealDll() {
    if (g_realDll) return;
    wchar_t sysDir[MAX_PATH];
    GetSystemDirectoryW(sysDir, MAX_PATH);
    wcscat(sysDir, L"\\version.dll");
    g_realDll = LoadLibraryW(sysDir);
}

// UE5 IConsoleManager::FindConsoleVariable
typedef uintptr_t (*FindConsoleVariable_t)(uintptr_t, const wchar_t*, uint8_t);

struct Config {
    float fpsTarget;
    bool smoothingFix;
};

// this just checks the config file, has a fallback for the old config file because people drop in updates without reading what has actually changed
static Config ReadConfig() {
    Config cfg;
    cfg.fpsTarget = 9999.0f;
    cfg.smoothingFix = false;

    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char* s = strrchr(path, '\\');
    if (s) *s = '\0';
    strcat(path, "\\version_config.ini");

    FILE* f = fopen(path, "r");
    if (!f) {
        char path2[MAX_PATH];
        GetModuleFileNameA(NULL, path2, MAX_PATH);
        char* s2 = strrchr(path2, '\\');
        if (s2) *s2 = '\0';
        strcat(path2, "\\fps.txt");
        f = fopen(path2, "r");
        if (f) {
            if (fscanf(f, "%f", &cfg.fpsTarget) == 1) {
                if (cfg.fpsTarget < 0) cfg.fpsTarget = 9999.0f;  // -1 = unlimited
                if (cfg.fpsTarget < 10.0f) cfg.fpsTarget = 10.0f;
                if (cfg.fpsTarget > 9999.0f) cfg.fpsTarget = 9999.0f;
            }
            fclose(f);
        }
        return cfg;
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char* cr = strchr(line, '\r'); if (cr) *cr = '\0';
        char* comment = strchr(line, ';'); if (comment) *comment = '\0';
        char* hash = strchr(line, '#'); if (hash) *hash = '\0';
        if (line[0] == '[') continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = line;
        char* val = eq + 1;
        while (*key == ' ' || *key == '\t') key++;
        char* keyEnd = key + strlen(key) - 1;
        while (keyEnd > key && (*keyEnd == ' ' || *keyEnd == '\t')) *keyEnd-- = '\0';
        while (*val == ' ' || *val == '\t') val++;
        char* valEnd = val + strlen(val) - 1;
        while (valEnd > val && (*valEnd == ' ' || *valEnd == '\t')) *valEnd-- = '\0';

        if (_stricmp(key, "FPS_TARGET") == 0) {
            float v = 9999.0f;
            if (sscanf(val, "%f", &v) == 1) {
                if (v < 0) v = 9999.0f;
                if (v < 10.0f) v = 10.0f;
                if (v > 9999.0f) v = 9999.0f;
                cfg.fpsTarget = v;
            }
        }
        else if (_stricmp(key, "SMOOTHING_FIX") == 0) {
            if (_stricmp(val, "true") == 0 || strcmp(val, "1") == 0)
                cfg.smoothingFix = true;
        }
    }
    fclose(f);
    return cfg;
}

static uintptr_t FindCVar(uintptr_t singleton, FindConsoleVariable_t fn, const wchar_t* name) {
    __try { return fn(singleton, name, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
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

static uintptr_t ReadPtr(uintptr_t addr) {
    __try { return *(uintptr_t*)addr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int32_t ReadI32(uintptr_t addr) {
    __try { return *(int32_t*)addr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static bool SafeReadFName(uintptr_t pool, int32_t idx, char* out, int outLen) {
    out[0] = '\0';
    if (idx < 0 || !pool) return false;
    __try {
        uint32_t blockIdx = (uint32_t)idx >> 16;
        uint32_t offset = ((uint32_t)idx & 0xFFFF) * 2;
        if (blockIdx >= 512) return false;

        uintptr_t block = ReadPtr(pool + 0x10 + blockIdx * 8);
        if (!block || block < 0x10000) return false;

        uint8_t* entry = (uint8_t*)(block + offset);
        uint16_t header = *(uint16_t*)entry;
        bool isWide = (header & 1) != 0;
        uint16_t len = header >> 6;
        if (len == 0 || len > 1024) return false;

        if (isWide) {
            const wchar_t* wstr = (const wchar_t*)(entry + 2);
            size_t copyLen = (len < (uint16_t)(outLen - 1)) ? len : (uint16_t)(outLen - 1);
            for (uint16_t i = 0; i < copyLen; i++) {
                if (wstr[i] < 0x20 && wstr[i] != 0) return false;
            }
            WideCharToMultiByte(CP_UTF8, 0, wstr, copyLen, out, outLen, NULL, NULL);
            return true;
        }

        const char* str = (const char*)(entry + 2);
        for (uint16_t i = 0; i < len; i++) {
            if ((unsigned char)str[i] < 0x20 && str[i] != 0) return false;
        }
        size_t copyLen = (len < (size_t)(outLen - 1)) ? len : (size_t)(outLen - 1);
        memcpy(out, str, copyLen);
        out[copyLen] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// walk class hierarchy for SpringArm (This is commonly used in gacha games third person cameras and is what gives them their smoothing feel)
static bool IsSpringArmByName(uintptr_t cls, uintptr_t pool) {
    for (int depth = 0; depth < 16; depth++) {
        if (!cls || cls < 0x10000) return false;
        int32_t ni = ReadI32(cls + 0x18);
        char n[128] = {};
        if (SafeReadFName(pool, ni, n, sizeof(n))) {
            if (strstr(n, "SpringArm")) return true;
            if (strcmp(n, "ActorComponent") == 0) return false;
            if (strcmp(n, "Object") == 0) return false;
        }
        uintptr_t p1 = ReadPtr(cls + 0x30);
        if (p1 && p1 != cls) { cls = p1; continue; }
        uintptr_t p2 = ReadPtr(cls + 0x28);
        if (p2 && p2 != cls) { cls = p2; continue; }
        break;
    }
    return false;
}

// this targets specifically the spring arm from the player controller
static bool IsPlayerSpringArm(uintptr_t obj, uintptr_t pool) {
    __try {
        uintptr_t outer = *(uintptr_t*)(obj + 0x20);
        if (!outer || outer < 0x10000) return false;
        int32_t outerNi = *(int32_t*)(outer + 0x18);
        char outerName[128] = {};
        SafeReadFName(pool, outerNi, outerName, sizeof(outerName));
        if (strstr(outerName, "PlayerController") && !strstr(outerName, "Default__"))
            return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static volatile bool g_smoothRunning = true;

struct GObjData {
    uintptr_t address;
    uintptr_t* chunks;
    int32_t numChunks;
    int32_t maxElements;
    int32_t maxChunks;
    int32_t perChunk;
    int32_t itemSize;
};

static GObjData g_gobj = {};
static uintptr_t g_namePool = 0;

// just rereads the live struct
static void RefreshGObjLive() {
    if (!g_gobj.address) return;
    __try {
        int32_t liveNumC = *(int32_t*)(g_gobj.address + 0x1C);
        int32_t liveMaxC = *(int32_t*)(g_gobj.address + 0x18);
        int32_t liveMaxE = *(int32_t*)(g_gobj.address + 0x10);
        if (liveNumC > 0 && liveMaxC > 0 && liveMaxE > 0) {
            g_gobj.numChunks = liveNumC;
            g_gobj.maxChunks = liveMaxC;
            g_gobj.maxElements = liveMaxE;
            g_gobj.perChunk = liveMaxE / liveMaxC;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static bool ValidateGObj(uintptr_t gobjAddr) {
    __try {
        uintptr_t chunksPtr = *(uintptr_t*)gobjAddr;
        if (chunksPtr < 0x10000 || chunksPtr > 0x7FFFFFFFFFFF) return false;

        int32_t maxE = *(int32_t*)(gobjAddr + 0x10);
        int32_t numE = *(int32_t*)(gobjAddr + 0x14);
        int32_t maxC = *(int32_t*)(gobjAddr + 0x18);
        int32_t numC = *(int32_t*)(gobjAddr + 0x1C);

        if (numC < 1 || numC > 0x14) return false;
        if (maxC < 6 || maxC > 0x5FF) return false;
        if (numE < 0x800 || numE > 5000000) return false;
        if (maxE < 0x10000 || maxE > 10000000) return false;
        if (numE > maxE || numC > maxC) return false;

        int32_t perChunk = maxE / maxC;
        if (perChunk < 0x8000 || perChunk > 0x80000) return false;
        if (perChunk % 0x10 != 0) return false;
        if (numC != (numE / perChunk) + 1) return false;
        if (maxC != maxE / perChunk) return false;

        g_gobj.address = gobjAddr;
        g_gobj.chunks = (uintptr_t*)chunksPtr;
        g_gobj.numChunks = numC;
        g_gobj.maxElements = maxE;
        g_gobj.maxChunks = maxC;
        g_gobj.perChunk = perChunk;
        g_gobj.itemSize = 24;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool FindGUObjectArray(uintptr_t gameBase, uintptr_t dataStart, size_t dataSize) {
    __try {
        uintptr_t* dp = (uintptr_t*)dataStart;
        size_t ndp = dataSize / 8;
        for (size_t i = 0; i < ndp; i++) {
            uintptr_t ptr = dp[i];
            if (ptr < 0x10000 || ptr > 0x7FFFFFFFFFFF) continue;
            uintptr_t gobjAddr = (uintptr_t)&dp[i];
            if (ValidateGObj(gobjAddr)) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

static bool FindFNamePool(uintptr_t gameBase, uintptr_t dataStart, size_t dataSize) {
    const uint8_t* data = (const uint8_t*)dataStart;
    for (size_t i = 0; i + 8 <= dataSize; i += 8) {
        uintptr_t candidate = *(uintptr_t*)(data + i);
        if (!candidate || candidate < 0x10000) continue;
        uintptr_t block0 = ReadPtr(candidate + 0x10);
        if (!block0 || block0 < 0x10000) continue;
        __try {
            uint16_t header = *(uint16_t*)block0;
            uint16_t len = header >> 6;
            if (len != 4) continue;
            if (memcmp((const char*)(block0 + 2), "None", 4) != 0) continue;
            g_namePool = candidate;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
    }
    return false;
}

static DWORD WINAPI SmoothingThread(LPVOID) {
    while (g_smoothRunning && !g_gobj.address) Sleep(1000);
    if (!g_smoothRunning) return 0;

    uintptr_t cached[64] = {};
    int cacheCount = 0;
    bool needScan = true;
    DWORD lastScan = 0;

    while (g_smoothRunning) {
        DWORD now = GetTickCount();

        if (needScan || (now - lastScan) >= 3000) {
            needScan = false;
            lastScan = now;
            RefreshGObjLive();
            int newCount = 0;
            uintptr_t newCache[64] = {};
            int32_t perChunk = g_gobj.perChunk;
            for (int ci = 0; ci < g_gobj.numChunks && newCount < 64; ci++) {
                uintptr_t chunk = ReadPtr((uintptr_t)g_gobj.chunks + ci * 8);
                if (!chunk) continue;
                for (int j = 0; j < perChunk && newCount < 64; j++) {
                    uintptr_t obj = ReadPtr(chunk + j * 24);
                    if (!obj || obj < 0x10000) continue;
                    uintptr_t cls = ReadPtr(obj + 0x10);
                    if (!cls || cls < 0x10000) continue;
                    if (!IsSpringArmByName(cls, g_namePool)) continue;
                    if (!IsPlayerSpringArm(obj, g_namePool)) continue;
                    newCache[newCount++] = obj;
                }
            }
            if (newCount > 0) {
                memcpy(cached, newCache, sizeof(uintptr_t) * newCount);
                cacheCount = newCount;
            }
        }

        for (int i = 0; i < cacheCount; i++) {
            if (!cached[i]) continue;
            __try {
                *(float*)(cached[i] + 660) = 99999.0f;
                *(float*)(cached[i] + 664) = 99999.0f;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                cached[i] = 0;
                needScan = true;
            }
        }
        Sleep(1);
    }
    return 0;
}

static DWORD WINAPI MainThread(LPVOID) {
    Sleep(25000); // wait for engine init, it doesn't NEED to be this long of a wait but... its a precaution for shitty hardware

    HMODULE hGame = GetModuleHandleA("HTGame.exe");
    if (!hGame) return 0;
    uintptr_t gameBase = (uintptr_t)hGame;

    uintptr_t textStart = 0; size_t textSize = 0;
    uintptr_t rdataStart = 0; size_t rdataSize = 0;
    uintptr_t dataStart = 0; size_t dataSize = 0;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)gameBase;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(gameBase + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        char name[9] = {}; memcpy(name, sec[i].Name, 8);
        uintptr_t va = gameBase + sec[i].VirtualAddress;
        size_t sz = sec[i].Misc.VirtualSize;
        if (strcmp(name, ".text") == 0) { textStart = va; textSize = sz; }
        else if (strcmp(name, ".rdata") == 0) { rdataStart = va; rdataSize = sz; }
        else if (strcmp(name, ".data") == 0) { dataStart = va; dataSize = sz; }
    }
    if (!textStart || !rdataStart) return 0;

    const wchar_t targetW[] = L"t.MaxFPS";
    size_t targetLen = wcslen(targetW);
    uintptr_t stringAddr = 0;
    __try {
        const uint8_t* rdata = (const uint8_t*)rdataStart;
        for (size_t i = 0; i + 16 <= rdataSize; i += 2) {
            const wchar_t* c = (const wchar_t*)(rdata + i);
            bool match = true;
            for (size_t j = 0; j < targetLen; j++) {
                if (c[j] != targetW[j]) { match = false; break; }
            }
            if (match && c[targetLen] == 0) { stringAddr = rdataStart + i; break; }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    if (!stringAddr) return 0;

    // this finds the IConsoleManager singleton dynamically by tracing xrefs to "t.MaxFPS" in .rdata
    // previously i hardcoded the RVAs and thats why it broke in the 1.0.8 update.
    uintptr_t singleton = 0;
    FindConsoleVariable_t findFunc = nullptr;
    __try {
        const uint8_t* text = (const uint8_t*)textStart;
        for (size_t i = 0; i + 7 <= textSize; i++) {
            if (text[i] != 0x48 && text[i] != 0x4C) continue;
            if (text[i+1] != 0x8D) continue;
            if ((text[i+2] & 0xC7) != 0x05) continue;
            int32_t disp = *(int32_t*)(text + i + 3);
            uintptr_t leaTarget = textStart + i + 7 + disp;
            if (leaTarget != stringAddr) continue;

            for (size_t back = 1; back <= 80 && i >= back; back++) {
                const uint8_t* p = text + i - back;
                if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x0D) continue;
                int32_t ripDisp = *(int32_t*)(p + 3);
                uintptr_t globalAddr = textStart + (i - back) + 7 + ripDisp;
                uintptr_t val = 0;
                __try { val = *(uintptr_t*)globalAddr; } __except(EXCEPTION_EXECUTE_HANDLER) { continue; }
                if (!val) continue;

                uint32_t vtblOff = 0;
                for (size_t fwd = 1; fwd <= 30 && i + 7 + fwd + 6 <= textSize; fwd++) {
                    const uint8_t* q = text + i + 7 + fwd;
                    if (q[0] == 0xFF && q[1] == 0x90) {
                        uint32_t off = *(uint32_t*)(q + 2);
                        if (off <= 0x800) { vtblOff = off; break; }
                    }
                }
                if (!vtblOff) continue;

                uintptr_t vtable = 0;
                __try { vtable = *(uintptr_t*)val; } __except(EXCEPTION_EXECUTE_HANDLER) { continue; }
                if (!vtable) continue;
                FindConsoleVariable_t fcv = nullptr;
                __try { fcv = (FindConsoleVariable_t)(*(uintptr_t*)(vtable + vtblOff)); } __except(EXCEPTION_EXECUTE_HANDLER) { continue; }
                if (!fcv) continue;

                uintptr_t testCVar = 0;
                __try { testCVar = fcv(val, L"t.MaxFPS", 1); } __except(EXCEPTION_EXECUTE_HANDLER) { continue; }
                if (testCVar) {
                    float fv = 0;
                    __try { fv = *(float*)(testCVar + 0x50); } __except(EXCEPTION_EXECUTE_HANDLER) {}
                    if (fv > 10.0f && fv < 1000.0f) {
                        singleton = val;
                        findFunc = fcv;
                        break;
                    }
                }
            }
            if (singleton) break;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    if (!singleton || !findFunc) return 0;

    // grab the cvar objects
    uintptr_t tMaxFPS = FindCVar(singleton, findFunc, L"t.MaxFPS");
    uintptr_t rVSync = FindCVar(singleton, findFunc, L"r.VSync");
    if (!tMaxFPS) return 0;

    Config cfg = ReadConfig();

    PatchFloat(tMaxFPS, 0x50, cfg.fpsTarget);
    if (rVSync) PatchInt(rVSync, 0x50, 0);

    if (cfg.smoothingFix) {
        if (dataStart && dataSize > 0) {
            FindFNamePool(gameBase, dataStart, dataSize);
            FindGUObjectArray(gameBase, dataStart, dataSize);
        }
        if (g_gobj.address && g_namePool) {
            CreateThread(NULL, 0, SmoothingThread, NULL, 0, NULL);
        }
    }

    // loop reapplication
    while (true) {
        Sleep(10000);
        Config newCfg = ReadConfig();
        if (newCfg.fpsTarget != cfg.fpsTarget) cfg.fpsTarget = newCfg.fpsTarget;
        if (*(float*)(tMaxFPS + 0x50) != cfg.fpsTarget)
    // apply patches
    PatchFloat(tMaxFPS, 0x50, cfg.fpsTarget);
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
