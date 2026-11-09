
#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <iostream>
#include <sstream>
#include <string>
#include <filesystem>
#include "NWN2ServerModInfo.h"

#include <NWN2Shared.h>
#include "NWN2Server.h"
#include "Service.h"

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


/// <summary>What the command line asked for.</summary>
struct Options
{
    enum class Action { Launch, Install, Uninstall, Start, Stop, RunService };

    Action action = Action::Launch;
    int serviceNo = 1;
    std::wstring account;
    std::wstring configPath;
    std::wstring error;
};

/// <summary>Parses the command line. The first non-flag is the config path, otherwise nwn2mod.config
/// next to the exe. Made absolute since a service starts in System32.</summary>
Options ParseOptions()
{
    Options options;
    auto args = ::GetArguments();

    for (size_t i = 1; i < args.size(); ++i)
    {
        const std::wstring& arg = args[i];
        auto set = [&](Options::Action action) {
            if (options.action != Options::Action::Launch)
            {
                options.error = L"Give only one of -installservice, -uninstallservice, -startservice, -stopservice.";
            }
            options.action = action;
        };

        if (_wcsicmp(arg.c_str(), L"-installservice") == 0)        { set(Options::Action::Install); }
        else if (_wcsicmp(arg.c_str(), L"-uninstallservice") == 0) { set(Options::Action::Uninstall); }
        else if (_wcsicmp(arg.c_str(), L"-startservice") == 0)     { set(Options::Action::Start); }
        else if (_wcsicmp(arg.c_str(), L"-stopservice") == 0)      { set(Options::Action::Stop); }
        else if (_wcsicmp(arg.c_str(), L"-runservice") == 0)       { set(Options::Action::RunService); }
        else if (_wcsicmp(arg.c_str(), L"-account") == 0)
        {
            if (i + 1 >= args.size())
            {
                options.error = L"-account needs a user name.";
            }
            else
            {
                options.account = args[++i];
            }
        }
        else if (_wcsicmp(arg.c_str(), L"-serviceno") == 0)
        {
            if (i + 1 >= args.size() || (options.serviceNo = _wtoi(args[i + 1].c_str())) <= 0)
            {
                options.error = L"-serviceno needs a number of 1 or more.";
            }
            ++i;
        }
        else if (!arg.empty() && arg[0] == L'-')
        {
            options.error = L"Unknown option: " + arg;
        }
        else if (options.configPath.empty())
        {
            options.configPath = arg;
        }
    }

    if (options.configPath.empty())
    {
        std::filesystem::path osPath(::GetFullModulePath());
        options.configPath = osPath.parent_path().append("nwn2mod.config");
    }
    options.configPath = std::filesystem::absolute(options.configPath).wstring();

    return options;
}

/// <summary>Output for the service commands. A windowed exe has no console, so this writes to a
/// redirect, the Command Prompt it was run from, or a message box.</summary>
class CommandOutput
{
public:
    CommandOutput()
    {
        HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD type = (out && out != INVALID_HANDLE_VALUE) ? ::GetFileType(out) : FILE_TYPE_UNKNOWN;
        if (type == FILE_TYPE_PIPE || type == FILE_TYPE_DISK)
        {
            return;
        }

        if (::AttachConsole(ATTACH_PARENT_PROCESS))
        {
            FILE* stream = nullptr;
            freopen_s(&stream, "CONOUT$", "w", stdout);
            freopen_s(&stream, "CONOUT$", "w", stderr);
            std::cout.clear();
            std::cout << std::endl;
            return;
        }

        _Saved = std::cout.rdbuf(_Buffer.rdbuf());
    }

    /// <summary>Shows the message box, if that's where output went.</summary>
    int Finish(int exitCode)
    {
        std::cout.flush();
        if (_Saved)
        {
            std::cout.rdbuf(_Saved);
            auto text = ::ToWString(_Buffer.str());
            ::MessageBoxW(nullptr, text.c_str(), L"NWN2ServerMod",
                MB_OK | (exitCode == 0 ? MB_ICONINFORMATION : MB_ICONERROR));
        }
        return exitCode;
    }

private:
    std::ostringstream _Buffer;
    std::streambuf* _Saved = nullptr;
};

/// <summary>Application entry point.</summary>
/// <returns>0 on success; 1 on failure.</returns>
int WINAPI wWinMain(
    HINSTANCE,
    HINSTANCE,
    PWSTR,
    int)
{
    auto options = ::ParseOptions();

    if (options.action == Options::Action::RunService)
    {
        return ServiceMode::Run(options.serviceNo, options.configPath);
    }

    if (!options.error.empty())
    {
        CommandOutput output;
        std::cout << ::ToString(options.error) << "\n\n"
            << "NWN2ServerMod.exe [config]                                  start the server and exit\n"
            << "NWN2ServerMod.exe -installservice -account <user> [-serviceno N] [config]\n"
            << "NWN2ServerMod.exe -startservice   [-serviceno N]\n"
            << "NWN2ServerMod.exe -stopservice    [-serviceno N]\n"
            << "NWN2ServerMod.exe -uninstallservice [-serviceno N]\n";
        return output.Finish(1);
    }

    if (options.action != Options::Action::Launch)
    {
        CommandOutput output;
        switch (options.action)
        {
            case Options::Action::Install:   return output.Finish(ServiceMode::Install(options.serviceNo, options.configPath, options.account));
            case Options::Action::Uninstall: return output.Finish(ServiceMode::Uninstall(options.serviceNo));
            case Options::Action::Start:     return output.Finish(ServiceMode::Start(options.serviceNo));
            case Options::Action::Stop:      return output.Finish(ServiceMode::Stop(options.serviceNo));
            default:                         break;
        }
    }

    auto result = NWN2Server::LoadServer(options.configPath);
    if (!result)
    {
        std::clog << result.error();
        return 1;
    }

    return 0;
}
