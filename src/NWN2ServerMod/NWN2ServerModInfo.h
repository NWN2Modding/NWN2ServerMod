#pragma once

/// <summary>Fixed-size launch parameters, sized generously for full Windows file paths.</summary>
struct NWN2ServerModInfo
{
    /// <summary>Full path to the loader DLL.</summary>
    wchar_t Loader[2048];

    /// <summary>Full path to the target server executable.</summary>
    wchar_t TargetExe[2048];

    /// <summary>Name of the global synchronization event.</summary>
    wchar_t GlobalEvent[128];
};