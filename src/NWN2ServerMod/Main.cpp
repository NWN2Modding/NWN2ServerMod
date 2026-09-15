
#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <iostream>
#include <string>
#include <filesystem>
#include "NWN2ServerModInfo.h"

#include <NWN2Shared.h>
#include "NWN2Server.h"

/// <summary>Splits the current process's command line into arguments, argv[0] included.</summary>
/// <returns>The parsed arguments, or an empty vector if the command line couldn't be parsed.</returns>
std::vector<std::wstring> GetArguments()
{
    std::vector<std::wstring> argsOut;

    auto args = ::GetCommandLineW();
    if (args == nullptr)
    {
        return argsOut;
    }

    int argc;
    auto argv = ::CommandLineToArgvW(args, &argc);
    if (!argv)
    {
        return argsOut;
    }

    for(int i = 0; i < argc; ++i)
    {
        argsOut.push_back(argv[i]);
    }

    ::LocalFree(argv);
    
    return argsOut;
}


/// <summary>Resolves the config path: the first command-line argument if given, otherwise <c>nwn2mod.config</c> next to this executable.</summary>
std::wstring GetConfigPath()
{
    std::wstring configPath;

    auto args = ::GetArguments();
    if (args.size() > 1)
    {
        configPath = args[1];
    }
    else
    {
        auto processPath = ::GetFullModulePath();
        std::filesystem::path osPath(processPath);

        configPath = osPath.parent_path().append("nwn2mod.config");
    }

    return configPath;
}

/// <summary>Application entry point: resolves the config path and runs <see cref="NWN2Server::LoadServer"/>.</summary>
/// <returns>0 on success; 1 if launching the server failed.</returns>
int WINAPI wWinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int)
{
    auto configPath = ::GetConfigPath();

    auto result = NWN2Server::LoadServer(configPath);
    if (!result)
    {
        std::clog << result.error();
        return 1;
    }

    return 0;
}