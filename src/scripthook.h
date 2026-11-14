// Minimal runtime binding to ScriptHookRDR2.dll.
// Resolves the hook's exports with GetProcAddress, so the mod builds without
// the ScriptHookRDR2 SDK .lib. Export names are the MSVC-mangled names of the
// functions declared in the SDK's inc/main.h.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <type_traits>

typedef int Entity;
typedef int Ped;
typedef int Player;
typedef int Prompt;
typedef uint32_t Hash;

struct Vector3
{
    alignas(8) float x;
    alignas(8) float y;
    alignas(8) float z;
};
static_assert(sizeof(Vector3) == 24, "Vector3 must match the SDK layout");

namespace sh
{
    using FnNativeInit     = void (*)(uint64_t);
    using FnNativePush64   = void (*)(uint64_t);
    using FnNativeCall     = uint64_t *(*)();
    using FnScriptWait     = void (*)(DWORD);
    using FnScriptRegister = void (*)(HMODULE, void (*)());
    using FnScriptUnreg    = void (*)(HMODULE);

    inline FnNativeInit     nativeInit       = nullptr;
    inline FnNativePush64   nativePush64     = nullptr;
    inline FnNativeCall     nativeCall       = nullptr;
    inline FnScriptWait     scriptWait       = nullptr;
    inline FnScriptRegister scriptRegister   = nullptr;
    inline FnScriptUnreg    scriptUnregister = nullptr;

    inline bool Load()
    {
        HMODULE h = GetModuleHandleA("ScriptHookRDR2.dll");
        if (!h)
            h = LoadLibraryA("ScriptHookRDR2.dll");
        if (!h)
            return false;

        nativeInit       = (FnNativeInit)GetProcAddress(h, "?nativeInit@@YAX_K@Z");
        nativePush64     = (FnNativePush64)GetProcAddress(h, "?nativePush64@@YAX_K@Z");
        nativeCall       = (FnNativeCall)GetProcAddress(h, "?nativeCall@@YAPEA_KXZ");
        scriptWait       = (FnScriptWait)GetProcAddress(h, "?scriptWait@@YAXK@Z");
        scriptRegister   = (FnScriptRegister)GetProcAddress(h, "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z");
        scriptUnregister = (FnScriptUnreg)GetProcAddress(h, "?scriptUnregister@@YAXPEAUHINSTANCE__@@@Z");

        return nativeInit && nativePush64 && nativeCall && scriptWait && scriptRegister && scriptUnregister;
    }

    template <typename T>
    inline void Push(T value)
    {
        static_assert(sizeof(T) <= sizeof(uint64_t), "native argument too large");
        uint64_t raw = 0;
        std::memcpy(&raw, &value, sizeof(T));
        nativePush64(raw);
    }

    template <typename R = void, typename... Args>
    inline R Invoke(uint64_t hash, Args... args)
    {
        nativeInit(hash);
        (Push(args), ...);
        uint64_t *result = nativeCall();
        if constexpr (!std::is_void_v<R>)
            return *reinterpret_cast<R *>(result);
    }
}

inline void WAIT(DWORD ms) { sh::scriptWait(ms); }
