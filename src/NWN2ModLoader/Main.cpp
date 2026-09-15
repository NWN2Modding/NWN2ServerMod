#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <string>
#include <filesystem>
#include <fstream>
#include <NWN2Shared.h>

#include "NWN2Mod.h"

#pragma comment(lib, "Detours.lib")

/// <summary>
/// Entry point run on the remote thread the launcher starts inside the target process. Loads the
/// config at <paramref name="lpParam"/> and initializes <see cref="NWN2Mod"/>.
/// </summary>
/// <param name="lpParam">A pointer to a null-terminated wide string: the full config file path.</param>
/// <returns>0 on success, or an error code from <see cref="NWN2Mod::Initialize(std::wstring_view)"/>.</returns>
DWORD WINAPI ModInitializationThread(LPVOID lpParam)
{
    std::wstring_view path((const wchar_t*)lpParam);
    auto result = NWN2Mod::Initialize(path);
    if (!result)
    {
        return result.error();
    }

    return 0;
}

/// <summary>
/// Writes the address of <see cref="ModInitializationThread"/> into the shared <c>Local\NWN2Shared</c>
/// memory map, so the launcher can read it back and start a second remote thread pointed at it.
/// </summary>
/// <returns><see langword="TRUE"/> on success; otherwise <see langword="FALSE"/>.</returns>
BOOL WriteAddress()
{
    MemoryMap map;

    if (!map.Open(L"Local\\NWN2Shared"))
    {
        return FALSE;
    }

    auto ptrReturn = map.GetAddress(0, 8);
    if (!ptrReturn)
    {
        return FALSE;
    }

    auto param = (size_t*)ptrReturn.value();

    *param = (size_t)&ModInitializationThread;

    map.UnmapAddressd(ptrReturn.value());

    return TRUE;
}

/// <summary>Standard DLL entry point; publishes <see cref="ModInitializationThread"/>'s address on attach.</summary>
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        if (!WriteAddress())
        {
            return FALSE;
        }
    }
    return TRUE;
}