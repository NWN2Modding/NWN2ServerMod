#pragma once
#include <string>

/// Runs NWN2ServerMod as a Windows service, like NWNX4: starts the server, restarts it when it
/// exits, and shuts it down cleanly on a stop. One service (NWN2ServerMod-<n>) per config.
/// Each call returns the exit code, 0 on success.
namespace ServiceMode
{
    std::wstring Name(int serviceNo);

    /// Asks for the account's password.
    int Install(int serviceNo, const std::wstring& configPath, const std::wstring& account);

    /// Stops it first if it's running.
    int Uninstall(int serviceNo);

    int Start(int serviceNo);

    /// Waits for the server's clean shutdown too.
    int Stop(int serviceNo);

    /// -runservice. Windows calls this; returns once the service stops.
    int Run(int serviceNo, const std::wstring& configPath);
}
