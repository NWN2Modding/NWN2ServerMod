#pragma once
#include <expected>
#include <filesystem>
#include <string>
#include <NWN2Shared.h>

/// <summary>
/// Launches and prepares <c>NWN2Server64.exe</c> for modding: starts it suspended, injects the
/// loader DLL, waits for it to finish hooking, then resumes the server.
/// </summary>
class NWN2Server
{
public:
    /// <summary>Loads the config at <paramref name="configPath"/> and runs the full launch/inject/resume sequence.</summary>
    /// <param name="configPath">The full path to the config file to load.</param>
    /// <returns>An unexpected error message on failure. Any failure terminates the partially-initialized server.</returns>
    static std::expected<void, std::string> LoadServer(std::wstring configPath);

private:
    /// <summary>Starts the server executable suspended, with the Steam app/game ID environment variables set.</summary>
    /// <param name="processPath">The full path to the server executable.</param>
    /// <param name="processDir">The working directory to start it in.</param>
    /// <param name="processArgs">The command-line arguments to pass.</param>
    /// <returns>The new process's <c>PROCESS_INFORMATION</c>, or an error message on failure.</returns>
    static std::expected<PROCESS_INFORMATION, std::string> StartProcess(std::wstring processPath, std::wstring processDir, std::wstring processArgs);

    /// <summary>Injects the loader DLL into the target process via a remote <c>LoadLibraryW</c> thread.</summary>
    /// <param name="hProcess">The target process.</param>
    /// <param name="hookPath">The full path to the loader DLL.</param>
    /// <param name="log">The logger to report progress/errors to.</param>
    /// <returns>The address of the loader's initialization thread proc (read back from the shared memory map), or an error message.</returns>
    static std::expected<void *, std::string> LoadHook(HANDLE hProcess, std::wstring hookPath, Logger &log);

    /// <summary>Runs the loader's initialization thread proc inside the target process and waits for it to finish.</summary>
    /// <param name="hProcess">The target process.</param>
    /// <param name="remoteProc">The address returned by <see cref="LoadHook"/>.</param>
    /// <param name="configPath">The full path to the config file, passed through to the loader.</param>
    /// <returns>An unexpected error message on failure, including a non-zero exit code from the loader thread.</returns>
    static std::expected<void, std::string> LoadLoader(HANDLE hProcess, void *remoteProc, std::wstring configPath);

    /// <summary>Resolves the loader DLL path to use: the configured path, or <c>NWN2ModLoader.dll</c> next to this executable.</summary>
    /// <param name="config">The loaded config.</param>
    static std::wstring GetLoaderDll(Config config);
};
