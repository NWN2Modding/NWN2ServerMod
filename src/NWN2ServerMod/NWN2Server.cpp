#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef max

#include "NWN2Server.h"
#include <memory>
#include <vector>
#include <string>
#include <psapi.h>
#include <iostream>
#include <NWN2Shared.h>

std::wstring NWN2Server::GetLoaderDll(Config config)
{
    if (config.loader_dll)
    {
        return ::ToWString(config.loader_dll.value());
    }

    std::filesystem::path processPath = ::GetFullModulePath();

    std::wstring loaderPath = processPath.parent_path().append("NWN2ModLoader.dll");

    return loaderPath;
}

std::expected<void, std::string> NWN2Server::LoadServer(std::wstring configPath)
{
    auto configResult = Config::FromFile(configPath);
    if (!configResult)
    {
        return std::unexpected(std::format("Failed to load config '{}': {}", ::ToString(configPath), configResult.error()));
    }

    auto config = *configResult;
    
    Logger log(
        config.server_log.transform([](auto v) { return::ToWString(v); }),
        config.std_log.value_or(true),
        config.debug_log.value_or(false));

    log("****** Initializing NWN2ServerMod ******");

    log("Starting Process: {}", config.server_exe);

    std::wstring serverExe = ::ToWString(config.server_exe);
    std::wstring workingDir;

    if (config.server_directory)
    {
        workingDir = ::ToWString(config.server_directory.value());
    }
    else
    {
        std::filesystem::path exePath(config.server_exe);

        workingDir = exePath.parent_path().wstring() + L"\\";
    }

    auto startResult = StartProcess(serverExe, workingDir, ::ToWString(config.server_args));
    if (!startResult)
    {
        log("Failed to start process: {}", startResult.error());
        return std::unexpected(startResult.error());
    }

    auto pi = startResult.value();

    Handle hProcess = pi.hProcess;
    Handle hThread = pi.hThread;

    auto hookResult = LoadHook(hProcess, GetLoaderDll(config), log);
    if (!hookResult)
    {
        log("Failed to inject Loader DLL: {}", hookResult.error());

        ::TerminateProcess(hProcess, -1);
        return std::unexpected(hookResult.error());
    }

    auto loadResult = LoadLoader(hProcess, *hookResult, configPath);
    if (!loadResult)
    {
        log("Loader initialization failed: {}", loadResult.error());

        ::TerminateProcess(hProcess, -1);
        return std::unexpected(loadResult.error());
    }

    log("Initialization completed.");

    // Initialization complete.
    // Wake up the server thread so it can do it's work.
    if (ResumeThread(hThread) == -1)
    {
        DWORD error = ::GetLastError();
        auto errorMessage = std::format("ResumeThread failed: {} (0x{:08X}", GetErrorMessage(error), error);

        log("{}", errorMessage);

        ::TerminateProcess(hProcess, -1);
        return std::unexpected(errorMessage);
    }

    log("NWN2Server64 running.");
    
    return {};
}

std::expected<PROCESS_INFORMATION, std::string> NWN2Server::StartProcess(
    std::wstring processPath,
    std::wstring processDir,
    std::wstring processArgs)
{
    STARTUPINFOW si = { 0 };
    PROCESS_INFORMATION pi = { 0 };
    si.cb = sizeof(si);

    // Force the local environment to recognize the target App ID before spawning the process. This stops
    // steam from opening the Client App instead of NWN2Server64.exe.
    // NWN2EE AppId = 2738630
    SetEnvironmentVariableW(L"SteamAppId", L"2738630");
    SetEnvironmentVariableW(L"SteamGameId", L"2738630");

    
    auto cmdLine = std::format(L"\"{}\" {}", processPath, processArgs);
    // Create the target process suspended. We don't want it to run until hooked.
    BOOL success = ::CreateProcessW(
        processPath.c_str(),    // Application Name
        cmdLine.data(),  // Command line arguments
        NULL,                 // Process attributes
        NULL,                 // Thread attributes
        FALSE,                // Inherit handles
        CREATE_SUSPENDED,     // CRITICAL: Start frozen before entry point
        NULL,                 // Environment
        processDir.c_str(),                 // Current directory
        &si,                  // Startup info
        &pi                   // Process information
    );

    if (!success)
    {
        DWORD error = ::GetLastError();

        return std::unexpected(std::format("CreateProcessW failed: {} (0x{:08X})", GetErrorMessage(error), error));
    }

    return pi;
}


std::expected<void *, std::string> NWN2Server::LoadHook(HANDLE hProcess, std::wstring hookPath, Logger& log)
{
    size_t strSize = (hookPath.size() + 1) * sizeof(wchar_t);

    // We need minimum of 8 bytes for pointer of next proc to run
    size_t allocSize = std::max(strSize, 8ull);

    // Allocate memory in the remote server process for the DLL path string
    LPVOID remoteDllAlloc = VirtualAllocEx(

        hProcess,
        NULL,
        allocSize,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
    );
    if (!remoteDllAlloc)
    {
        DWORD error = ::GetLastError();

        return std::unexpected(std::format("VirtualAllocEx failed: {} (0x{:08X})", GetErrorMessage(error), error));
    }

    // Auto Delete the Virtual Memory Allocation so we don't have to remember to clean it up.
    // That's how memory leaks happen!
    // Define the stateful lambda custom deleter explicitly
    auto remoteDeleter = [hProcess](void* ptr) { if (ptr) { ::VirtualFreeEx(hProcess, ptr, 0, MEM_RELEASE); } };

    // Pass 'void' and feed the lambda object directly into the constructor
    std::unique_ptr<void, decltype(remoteDeleter)> remoteMemoryGuard(remoteDllAlloc, remoteDeleter);

    // Write the local DLL path string into the newly allocated remote memory space
    if (!WriteProcessMemory(hProcess, remoteDllAlloc, hookPath.data(), strSize, NULL))
    {
        DWORD error = ::GetLastError();

        return std::unexpected(std::format("WriteProcessMemory failed: {} (0x{:08X})", GetErrorMessage(error), error));
    }

    // Get kernel32.dll. This will always have the same module address in every process in modern windows.
    // So getting it for our current process will get the kernel32.dll in the nwn2server64.exe too.
    auto kernelModule = GetModuleHandleA("kernel32.dll");
    if (!kernelModule)
    {
        DWORD error = ::GetLastError();

        return std::unexpected(std::format("GetModuleHandleA failed: {} (0x{:08X})", GetErrorMessage(error), error));
    }

    // Get the real kernel32 memory address of LoadLibraryA. We will use this for the remote thread
    // as it just happens to take 1 pointer argument and return a BOOL (DWORD) which is what
    // CreateRemoteThread needs. Brilliant!
    LPVOID loadLibraryAddr = (LPVOID)GetProcAddress(kernelModule, "LoadLibraryW");
    if (!loadLibraryAddr)
    {
        // This SHOULD never fail, but you never know on windows. Better to error than crash.
        return std::unexpected("LoadLibraryW not found in kernel32.dll.");
    }
    
    MemoryMap map;

    auto memResult = map.Create(L"Local\\NWN2Shared", 8);
    if(!memResult)
    {
        return std::unexpected(memResult.error());
    }

    // Spin up a new thread inside the server pointing directly to LoadLibraryA
    // Use Handle so it automatically cleans up.
    Handle hRemoteThread = CreateRemoteThread(
        hProcess,
        NULL,
        0,
        (LPTHREAD_START_ROUTINE)loadLibraryAddr,
        remoteDllAlloc,
        0,
        NULL
    );
    if (!hRemoteThread)
    {
        DWORD error = ::GetLastError();

        return std::unexpected(std::format("CreateRemoteThread for LoadLibraryW failed: {} (0x{:08X}", GetErrorMessage(error), error));
    }

    // Wait for the remote thread to complete. Let's time out so we don't have a deadlock. It should be pretty fast but
    // windows can be bogged down, so we use 5 minutes.
    DWORD wait = ::WaitForSingleObject(hRemoteThread, 300000);
    if (wait != WAIT_OBJECT_0)
    {
        switch(wait)
        {
            case WAIT_TIMEOUT: return std::unexpected("Timeout waiting for LoadLibraryW thread.");
            case WAIT_ABANDONED: return std::unexpected("Abandoned handle on LoadLibraryW thread.");
            default:
            {
                DWORD error = ::GetLastError();

                return std::unexpected(std::format("WaitForSingleObject error on LoadLibraryW thread: {} (0x{:08X}", GetErrorMessage(error), error));
            }
        }
    }

    // After thread completes, get the result code.
    DWORD exitCode;
    if (!::GetExitCodeThread(hRemoteThread, &exitCode))
    {
        DWORD error = ::GetLastError();

        return std::unexpected(std::format("GetExitCodeThread failed: {} (0x{:08X})", GetErrorMessage(error), error));
    }
    
    if (exitCode == 0)
    {
        return std::unexpected(std::format("Loader thread completed with exit: (0x{:08X})", exitCode));
    }

    // After LoadLibrary completes, the memory SHOULD contain the address of the next remote procedure which is filled
    // by DllMain of the Hook DLL.
    auto ptrResult = map.GetAddress(0, 8);
    if (!ptrResult)
    {
        return std::unexpected(ptrResult.error());
    }

    uint64_t remoteProcAddress = *(uint64_t *)ptrResult.value();

    map.UnmapAddressd(ptrResult.value());

    return (void *)remoteProcAddress;
}

std::expected<void, std::string> NWN2Server::LoadLoader(HANDLE hProcess, void* remoteProc, std::wstring configPath)
{
    size_t strSize = (configPath.size() + 1) * sizeof(wchar_t);

    // Allocate memory in the remote server process for the config path string
    LPVOID remoteConfigAlloc = VirtualAllocEx(
        hProcess,
        NULL,
        strSize,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
    );
    if (!remoteConfigAlloc)
    {
        DWORD error = ::GetLastError();

        return std::unexpected(std::format("VirtualAllocEx failed: {} (0x{:08X}", GetErrorMessage(error), error));
    }

    // Auto Delete the Virtual Memory Allocation so we don't have to remember to clean it up.
    // That's how memory leaks happen!
    // Define the stateful lambda custom deleter explicitly
    auto remoteDeleter = [hProcess](void* ptr) { if (ptr) { ::VirtualFreeEx(hProcess, ptr, 0, MEM_RELEASE); } };

    // Pass 'void' and feed the lambda object directly into the constructor
    std::unique_ptr<void, decltype(remoteDeleter)> remoteMemoryGuard(remoteConfigAlloc, remoteDeleter);

    // Write the local DLL path string into the newly allocated remote memory space
    if (!WriteProcessMemory(hProcess, remoteConfigAlloc, configPath.data(), strSize, NULL))
    {
        DWORD error = ::GetLastError();

        return std::unexpected(std::format("WriteProcessMemory failed: {} (0x{:08X}", GetErrorMessage(error), error));
    }

    // Spin up a new thread inside the server pointing directly to LoadLibraryA
    // Use Handle so it automatically cleans up.
    Handle hRemoteThread = CreateRemoteThread(
        hProcess,
        NULL,
        0,
        (LPTHREAD_START_ROUTINE)remoteProc,
        remoteConfigAlloc,
        0,
        NULL
    );
    if (!hRemoteThread)
    {
        DWORD error = ::GetLastError();

        return std::unexpected(std::format("CreateRemoteThread failed: {} (0x{:08X})", GetErrorMessage(error), error));
    }

    // Wait for the remote thread to complete. Let's time out so we don't have a deadlock. It should be fairly fast but
    // windows can be bogged down, so we use 5 minutes.
    DWORD wait = ::WaitForSingleObject(hRemoteThread, 300000);
    if (wait != WAIT_OBJECT_0)
    {
        switch (wait)
        {
            case WAIT_TIMEOUT: return std::unexpected("Timeout waiting for LoadLibraryW thread.");
            case WAIT_ABANDONED: return std::unexpected("Abandoned handle on LoadLibraryW thread.");
            default:
            {
                DWORD error = ::GetLastError();

                return std::unexpected(std::format("WaitForSingleObject error on LoadLibraryW thread: {} (0x{:08X}", GetErrorMessage(error), error));
            }
        }
    }

    // After thread completes, get the result code.
    DWORD exitCode;
    if (!::GetExitCodeThread(hRemoteThread, &exitCode))
    {
        DWORD error = ::GetLastError();

        return std::unexpected(std::format("GetExitCodeThread failed: {} (0x{:08X})", GetErrorMessage(error), error));
    }

    if (exitCode != 0)
    {
        return std::unexpected(std::format("Hook thread completed with error: (0x{:08X})", exitCode));
    }

    return {};
}