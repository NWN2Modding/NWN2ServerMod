#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincred.h>
#include <ntsecapi.h>
#include <shlobj.h>
#undef max
#undef min

#include "Service.h"
#include "NWN2Server.h"

#include <NWN2Shared.h>


#include <chrono>
#include <filesystem>
#include <format>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>

namespace
{
    /// Closes the handle when it goes out of scope.
    struct ScHandle
    {
        SC_HANDLE handle = nullptr;
        ScHandle(SC_HANDLE h) : handle(h) {}
        ~ScHandle() { if (handle) { ::CloseServiceHandle(handle); } }
        ScHandle(const ScHandle&) = delete;
        ScHandle& operator=(const ScHandle&) = delete;
        operator SC_HANDLE() const { return handle; }
    };

    template <typename... Args>
    void Say(std::format_string<Args...> fmt, Args&&... args)
    {
        std::cout << std::format(fmt, std::forward<Args>(args)...) << std::endl;
    }

    /// Prints a failed call, with a hint for the common ones.
    int Fail(const char* what, DWORD error = ::GetLastError())
    {
        if (error == ERROR_SERVICE_DOES_NOT_EXIST)
        {
            Say("That service is not installed. Check the -serviceno, or install it with -installservice.");
            return 1;
        }
        Say("{} failed: {} (0x{:08X})", what, GetErrorMessage(error), error);
        if (error == ERROR_ACCESS_DENIED)
        {
            Say("Run this from a Command Prompt opened with 'Run as administrator'.");
        }
        return 1;
    }

    std::optional<SERVICE_STATUS_PROCESS> QueryStatus(SC_HANDLE service)
    {
        SERVICE_STATUS_PROCESS status{};
        DWORD needed = 0;
        if (!::QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status),
                                    sizeof(status), &needed))
        {
            return std::nullopt;
        }
        return status;
    }

    /// Waits while the service is pending, with a note every 15s.
    DWORD WaitWhile(SC_HANDLE service, DWORD pending, std::chrono::seconds limit)
    {
        auto start = std::chrono::steady_clock::now();
        auto lastNote = start;
        for (;;)
        {
            auto status = QueryStatus(service);
            if (!status || status->dwCurrentState != pending)
            {
                return status ? status->dwCurrentState : 0;
            }

            auto now = std::chrono::steady_clock::now();
            if (now - start >= limit)
            {
                return pending;
            }
            if (now - lastNote >= std::chrono::seconds(15))
            {
                lastNote = now;
                Say("  still waiting ({}s)...",
                    std::chrono::duration_cast<std::chrono::seconds>(now - start).count());
            }
            ::Sleep(500);
        }
    }

    /// ".\name" for a bare local account; CreateService wants it qualified.
    std::wstring QualifyAccount(std::wstring account)
    {
        if (account.find(L'\\') == std::wstring::npos && account.find(L'@') == std::wstring::npos)
        {
            account = L".\\" + account;
        }
        return account;
    }

    /// Windows' password box. A console prompt doesn't work from a windowed exe.
    std::optional<std::wstring> AskPassword(const std::wstring& account, const std::wstring& service)
    {
        wchar_t user[CREDUI_MAX_USERNAME_LENGTH + 1] = {};
        wchar_t password[CREDUI_MAX_PASSWORD_LENGTH + 1] = {};
        wcsncpy_s(user, account.c_str(), _TRUNCATE);

        std::wstring message = std::format(L"Password for {}, which {} will run as.", account, service);
        CREDUI_INFOW info{ sizeof(info), nullptr, message.c_str(), L"NWN2ServerMod", nullptr };
        BOOL save = FALSE;
        DWORD result = ::CredUIPromptForCredentialsW(&info, service.c_str(), nullptr, 0,
            user, ARRAYSIZE(user), password, ARRAYSIZE(password), &save,
            CREDUI_FLAGS_GENERIC_CREDENTIALS | CREDUI_FLAGS_DO_NOT_PERSIST
            | CREDUI_FLAGS_KEEP_USERNAME | CREDUI_FLAGS_ALWAYS_SHOW_UI);

        std::optional<std::wstring> answer;
        if (result == NO_ERROR)
        {
            answer = password;
        }
        ::SecureZeroMemory(password, sizeof(password));
        return answer;
    }

    /// False only for a bad name or password.
    bool PasswordWorks(const std::wstring& account, const std::wstring& password)
    {
        size_t slash = account.find(L'\\');
        std::wstring domain = slash == std::wstring::npos ? L"" : account.substr(0, slash);
        std::wstring user = slash == std::wstring::npos ? account : account.substr(slash + 1);

        HANDLE token = nullptr;
        if (::LogonUserW(user.c_str(), domain.empty() ? nullptr : domain.c_str(), password.c_str(),
                         LOGON32_LOGON_NETWORK, LOGON32_PROVIDER_DEFAULT, &token))
        {
            ::CloseHandle(token);
            return true;
        }
        return ::GetLastError() != ERROR_LOGON_FAILURE;
    }

    /// Grants "Log on as a service". CreateService doesn't do it for you.
    DWORD GrantServiceLogon(const std::wstring& account)
    {
        std::wstring lookup = account.starts_with(L".\\") ? account.substr(2) : account;

        BYTE sid[SECURITY_MAX_SID_SIZE];
        DWORD sidSize = sizeof(sid);
        wchar_t domain[256];
        DWORD domainSize = ARRAYSIZE(domain);
        SID_NAME_USE use;
        if (!::LookupAccountNameW(nullptr, lookup.c_str(), sid, &sidSize, domain, &domainSize, &use))
        {
            return ::GetLastError();
        }

        LSA_OBJECT_ATTRIBUTES attributes{};
        LSA_HANDLE policy = nullptr;
        NTSTATUS status = ::LsaOpenPolicy(nullptr, &attributes, POLICY_LOOKUP_NAMES | POLICY_CREATE_ACCOUNT, &policy);
        if (status == 0)
        {
            wchar_t name[] = L"SeServiceLogonRight";
            LSA_UNICODE_STRING right{ sizeof(name) - sizeof(wchar_t), sizeof(name), name };
            status = ::LsaAddAccountRights(policy, sid, &right, 1);
            ::LsaClose(policy);
        }
        return ::LsaNtStatusToWinError(status);
    }

    /// The server exits quietly without the account's nwn2player.ini.
    std::optional<std::string> CheckGameFolder()
    {
        wchar_t user[256] = {};
        DWORD userSize = ARRAYSIZE(user);
        ::GetUserNameW(user, &userSize);

        PWSTR documents = nullptr;
        if (FAILED(::SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)))
        {
            return std::format("Can't find the Documents folder for {}.", ::ToString(user));
        }
        std::filesystem::path ini = std::filesystem::path(documents) / L"Neverwinter Nights 2" / L"nwn2player.ini";
        ::CoTaskMemFree(documents);

        if (std::filesystem::exists(ini))
        {
            return std::nullopt;
        }
        return std::format("The service runs as {}, and {} doesn't exist. The server needs it and exits "
            "without it. Reinstall with -account <the account the server normally runs as>.",
            ::ToString(user), ini.string());
    }

    std::wstring g_name;
    std::wstring g_configPath;
    int g_serviceNo = 1;

    SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
    SERVICE_STATUS g_status{};
    std::mutex g_statusLock;
    HANDLE g_stopEvent = nullptr;

    /// Pending states bump the checkpoint so Windows knows a long stop is still going.
    void Report(DWORD state, DWORD waitHintMs = 0, DWORD specificError = 0)
    {
        std::lock_guard guard(g_statusLock);

        g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        g_status.dwCurrentState = state;
        g_status.dwWin32ExitCode = specificError ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR;
        g_status.dwServiceSpecificExitCode = specificError;
        g_status.dwWaitHint = waitHintMs;
        g_status.dwControlsAccepted = state == SERVICE_RUNNING
            ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN
            : 0;
        g_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED)
            ? 0
            : g_status.dwCheckPoint + 1;

        if (g_statusHandle)
        {
            ::SetServiceStatus(g_statusHandle, &g_status);
        }
    }

    DWORD WINAPI Handler(DWORD control, DWORD, LPVOID, LPVOID)
    {
        switch (control)
        {
            case SERVICE_CONTROL_STOP:
            case SERVICE_CONTROL_PRESHUTDOWN:
                Report(SERVICE_STOP_PENDING, 10000);
                ::SetEvent(g_stopEvent);
                return NO_ERROR;

            case SERVICE_CONTROL_INTERROGATE:
                return NO_ERROR;

            default:
                return ERROR_CALL_NOT_IMPLEMENTED;
        }
    }

    struct WindowSearch
    {
        DWORD processId;
        HWND found;
    };

    BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM lParam)
    {
        auto* search = reinterpret_cast<WindowSearch*>(lParam);
        DWORD pid = 0;
        ::GetWindowThreadProcessId(hwnd, &pid);

        wchar_t className[256];
        if (pid == search->processId && ::GetClassNameW(hwnd, className, 256)
            && wcscmp(className, L"Exo - BioWare Corp., (c) 1999 - Generic Blank Application") == 0)
        {
            search->found = hwnd;
            return FALSE;
        }
        return TRUE;
    }

    /// Lets xp_srvadmin run its shutdown if it's loaded, otherwise closes the window like NWNX4.
    /// Ends the process if it's still up after the timeout.
    void StopServer(NWN2Server::Running& server, Logger& log, std::chrono::seconds timeout)
    {
        WindowSearch search{ server.ProcessId, nullptr };
        ::EnumWindows(FindWindowProc, reinterpret_cast<LPARAM>(&search));

        std::wstring eventName = L"Local\\NWN2ServerMod.Shutdown." + std::to_wstring(server.ProcessId);
        Handle shutdownEvent = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName.c_str());

        if (!search.found)
        {
            // No window yet means nobody's on and nothing to save.
            log("Service stop: the server is still starting (no window yet); ending it.");
            timeout = std::chrono::seconds(0);
        }
        else if (shutdownEvent)
        {
            ::SetEvent(shutdownEvent);
            log("Service stop: asked xp_srvadmin for a clean shutdown (players saved and booted first). "
                "Waiting up to {}s.", timeout.count());
        }
        else
        {
            ::PostMessageW(search.found, WM_CLOSE, 0, 0);
            log("Service stop: xp_srvadmin is not loaded, so the server window was closed directly. "
                "Waiting up to {}s.", timeout.count());
        }

        auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;)
        {
            if (::WaitForSingleObject(server.Process, 1000) == WAIT_OBJECT_0)
            {
                DWORD code = 0;
                ::GetExitCodeProcess(server.Process, &code);
                log("Server exited (code {}).", code);
                return;
            }

            Report(SERVICE_STOP_PENDING, 5000);
            if (std::chrono::steady_clock::now() >= deadline)
            {
                break;
            }
        }

        if (timeout.count() > 0)
        {
            log(Logger::Level::Warning, "The server did not exit within {}s; ending the process.", timeout.count());
        }
        ::TerminateProcess(server.Process, 1);
        ::WaitForSingleObject(server.Process, 10000);
    }

    /// server_log if set, otherwise NWN2ServerMod-<n>.log next to the exe.
    std::filesystem::path ServiceLogPath(const Config* config)
    {
        if (config && config->server_log)
        {
            return ::ToWString(*config->server_log);
        }
        std::filesystem::path exe = ::GetFullModulePath();
        return exe.parent_path() / std::format(L"NWN2ServerMod-{}.log", g_serviceNo);
    }

    void WINAPI ServiceMain(DWORD, LPWSTR*)
    {
        g_statusHandle = ::RegisterServiceCtrlHandlerExW(g_name.c_str(), Handler, nullptr);
        if (!g_statusHandle)
        {
            return;
        }

        Report(SERVICE_START_PENDING, 30000);

        auto configResult = Config::FromFile(g_configPath);
        if (!configResult)
        {
            Logger early(ServiceLogPath(nullptr), false, false);
            early(Logger::Level::Error, "Service {}: could not load config '{}': {}", ::ToString(g_name),
                  ::ToString(g_configPath), configResult.error());
            Report(SERVICE_STOPPED, 0, 1);
            return;
        }

        const Config& config = *configResult;
        Logger log(ServiceLogPath(&config), false, config.debug_log.value_or(false));

        const bool restartOnExit = config.restart_on_exit.value_or(true);
        const auto restartDelay = std::chrono::seconds(std::max(0, config.restart_delay.value_or(5)));
        const auto stopTimeout = std::chrono::seconds(std::max(0, config.stop_timeout.value_or(180)));

        // 5 quick exits in a row means it can't stay up. Stop instead of looping.
        constexpr int kMaxQuickExits = 5;
        constexpr auto kQuickExit = std::chrono::minutes(5);

        g_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

        log("****** Service {} started. Config: {} ******", ::ToString(g_name), ::ToString(g_configPath));

        if (auto problem = CheckGameFolder())
        {
            log(Logger::Level::Error, "{} Not starting the server.", *problem);
            Report(SERVICE_STOPPED, 0, 3);
            return;
        }
        log("restart_on_exit {}, restart_delay {}s, stop_timeout {}s.", restartOnExit,
            restartDelay.count(), stopTimeout.count());

        Report(SERVICE_RUNNING);

        int quickExits = 0;
        DWORD stopError = 0;

        for (;;)
        {
            auto started = std::chrono::steady_clock::now();
            auto running = NWN2Server::Launch(config, g_configPath, log);
            if (!running)
            {
                log(Logger::Level::Error, "Could not start the server: {}", running.error());
            }
            else
            {
                HANDLE waits[] = { g_stopEvent, running->Process };
                DWORD which = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (which == WAIT_OBJECT_0)
                {
                    StopServer(*running, log, stopTimeout);
                    break;
                }

                DWORD code = 0;
                ::GetExitCodeProcess(running->Process, &code);
                auto ran = std::chrono::duration_cast<std::chrono::minutes>(std::chrono::steady_clock::now() - started);
                log("Server exited (code {}) after {} minute(s).", code, ran.count());

                if (!restartOnExit)
                {
                    log("restart_on_exit is off, so the service stops too.");
                    break;
                }
            }

            quickExits = std::chrono::steady_clock::now() - started < kQuickExit ? quickExits + 1 : 0;
            if (quickExits >= kMaxQuickExits)
            {
                log(Logger::Level::Error, "The server failed to start or exited within {} minutes {} times "
                    "in a row. Not starting it again; fix the cause and start the service.",
                    kQuickExit.count(), kMaxQuickExits);
                stopError = 2;
                break;
            }

            log("Starting the server again in {}s.", restartDelay.count());
            if (::WaitForSingleObject(g_stopEvent, static_cast<DWORD>(restartDelay.count() * 1000)) == WAIT_OBJECT_0)
            {
                break;
            }
        }

        log("****** Service {} stopped. ******", ::ToString(g_name));
        Report(SERVICE_STOPPED, 0, stopError);
    }
}

namespace ServiceMode
{
    std::wstring Name(int serviceNo)
    {
        return std::format(L"NWN2ServerMod-{}", serviceNo);
    }

    int Install(int serviceNo, const std::wstring& configPath, const std::wstring& account)
    {
        if (!std::filesystem::is_regular_file(configPath))
        {
            Say("No config file at '{}'. Pass its path after -installservice, or put nwn2mod.config "
                "next to NWN2ServerMod.exe.", ::ToString(configPath));
            return 1;
        }

        auto config = Config::FromFile(configPath);
        if (!config)
        {
            Say("Could not load config '{}': {}", ::ToString(configPath), config.error());
            return 1;
        }

        // LocalSystem can't run the server, even with -home: it exits before writing a log.
        if (account.empty() || _wcsicmp(account.c_str(), L"LocalSystem") == 0)
        {
            Say("Give the account the server normally runs under: -installservice -account <user>.");
            Say("The server needs that account's Documents\\Neverwinter Nights 2 and won't run as LocalSystem.");
            return 1;
        }

        std::wstring exe = ::GetFullModulePath();
        std::wstring name = Name(serviceNo);
        std::wstring display = std::format(L"NWN2ServerMod Service {}", serviceNo);
        std::wstring command = std::format(L"\"{}\" -serviceno {} -runservice \"{}\"", exe, serviceNo, configPath);

        ScHandle manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
        if (!manager)
        {
            return Fail("OpenSCManager");
        }

        // After OpenSCManager, so a non-admin prompt fails before asking for the password.
        std::wstring user = QualifyAccount(account);
        auto answer = AskPassword(user, name);
        if (!answer)
        {
            Say("No password given; nothing installed.");
            return 1;
        }
        std::wstring password = std::move(*answer);

        if (!PasswordWorks(user, password))
        {
            ::SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
            Say("Wrong user name or password for {}; nothing installed.", ::ToString(user));
            return 1;
        }

        if (DWORD error = GrantServiceLogon(user); error != NO_ERROR)
        {
            ::SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
            return Fail("Granting \"Log on as a service\"", error);
        }

        // Manual, like NWNX4.
        ScHandle service = ::CreateServiceW(manager, name.c_str(), display.c_str(), SERVICE_ALL_ACCESS,
            SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, command.c_str(),
            nullptr, nullptr, nullptr, user.c_str(), password.c_str());
        ::SecureZeroMemory(password.data(), password.size() * sizeof(wchar_t));
        if (!service)
        {
            DWORD error = ::GetLastError();
            if (error == ERROR_SERVICE_EXISTS)
            {
                Say("{} is already installed. Use -uninstallservice -serviceno {} first to reinstall it.",
                    ::ToString(name), serviceNo);
                return 1;
            }
            return Fail("CreateService", error);
        }

        std::wstring description = L"NWN2ServerMod Service Instance - x64 Server";
        SERVICE_DESCRIPTIONW descriptionInfo{ description.data() };
        if (!::ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &descriptionInfo))
        {
            Say("Warning: couldn't set the description (error {}).", ::GetLastError());
        }

        // Windows shutdown gets the same time as a service stop.
        DWORD stopSeconds = static_cast<DWORD>(std::max(0, config->stop_timeout.value_or(180)));
        SERVICE_PRESHUTDOWN_INFO preshutdown{ (stopSeconds + 30) * 1000 };
        ::ChangeServiceConfig2W(service, SERVICE_CONFIG_PRESHUTDOWN_INFO, &preshutdown);

        // Restarts the service if it crashes. Server restarts are handled in ServiceMain.
        SC_ACTION actions[3] = {
            { SC_ACTION_RESTART, 60000 }, { SC_ACTION_RESTART, 60000 }, { SC_ACTION_RESTART, 60000 } };
        SERVICE_FAILURE_ACTIONSW failure{};
        failure.dwResetPeriod = 24 * 60 * 60;
        failure.cActions = 3;
        failure.lpsaActions = actions;
        ::ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);

        Say("Installed {} ({}).", ::ToString(name), ::ToString(display));
        Say("  runs:   {}", ::ToString(command));
        Say("  runs as {}, started manually", ::ToString(user));
        Say("Start it with -startservice -serviceno {}. Once it's tested, set it to Automatic in services.msc.", serviceNo);
        return 0;
    }

    int Stop(int serviceNo)
    {
        std::wstring name = Name(serviceNo);
        ScHandle manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!manager)
        {
            return Fail("OpenSCManager");
        }
        ScHandle service = ::OpenServiceW(manager, name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS);
        if (!service)
        {
            return Fail("OpenService");
        }

        SERVICE_STATUS status{};
        if (!::ControlService(service, SERVICE_CONTROL_STOP, &status))
        {
            DWORD error = ::GetLastError();
            if (error == ERROR_SERVICE_NOT_ACTIVE)
            {
                Say("{} is not running.", ::ToString(name));
                return 0;
            }
            return Fail("ControlService(stop)", error);
        }

        Say("Stopping {}: the server shuts down cleanly first, which can take a few minutes.", ::ToString(name));
        DWORD state = WaitWhile(service, SERVICE_STOP_PENDING, std::chrono::minutes(15));
        if (state != SERVICE_STOPPED)
        {
            Say("{} has not stopped yet; check its log.", ::ToString(name));
            return 1;
        }

        Say("{} stopped.", ::ToString(name));
        return 0;
    }

    int Uninstall(int serviceNo)
    {
        std::wstring name = Name(serviceNo);
        ScHandle manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!manager)
        {
            return Fail("OpenSCManager");
        }
        ScHandle service = ::OpenServiceW(manager, name.c_str(), DELETE | SERVICE_QUERY_STATUS);
        if (!service)
        {
            return Fail("OpenService");
        }

        auto status = QueryStatus(service);
        if (status && status->dwCurrentState != SERVICE_STOPPED)
        {
            if (int result = Stop(serviceNo); result != 0)
            {
                return result;
            }
        }

        if (!::DeleteService(service))
        {
            return Fail("DeleteService");
        }

        Say("Uninstalled {}.", ::ToString(name));
        return 0;
    }

    int Start(int serviceNo)
    {
        std::wstring name = Name(serviceNo);
        ScHandle manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!manager)
        {
            return Fail("OpenSCManager");
        }
        ScHandle service = ::OpenServiceW(manager, name.c_str(), SERVICE_START | SERVICE_QUERY_STATUS);
        if (!service)
        {
            return Fail("OpenService");
        }

        if (!::StartServiceW(service, 0, nullptr))
        {
            DWORD error = ::GetLastError();
            if (error == ERROR_SERVICE_ALREADY_RUNNING)
            {
                Say("{} is already running.", ::ToString(name));
                return 0;
            }
            return Fail("StartService", error);
        }

        DWORD state = WaitWhile(service, SERVICE_START_PENDING, std::chrono::seconds(60));
        if (state != SERVICE_RUNNING)
        {
            Say("{} did not start; check its log.", ::ToString(name));
            return 1;
        }

        Say("{} started. The server itself takes a few minutes to load the module.", ::ToString(name));
        return 0;
    }

    int Run(int serviceNo, const std::wstring& configPath)
    {
        g_serviceNo = serviceNo;
        g_name = Name(serviceNo);
        g_configPath = configPath;

        // Services start in System32. Keep relative config paths relative to the exe.
        std::filesystem::path exe = ::GetFullModulePath();
        ::SetCurrentDirectoryW(exe.parent_path().c_str());

        SERVICE_TABLE_ENTRYW table[] = {
            { g_name.data(), ServiceMain },
            { nullptr, nullptr },
        };

        if (!::StartServiceCtrlDispatcherW(table))
        {
            // Fails when run by hand instead of by Windows.
            return 1;
        }
        return 0;
    }
}
