// xp_srvadmin, ported from NWNX4 to the NWN2ServerMod C ABI.
//
// Original: Copyright (C) 2008 Ken Johnson (Skywing), GPL v2 or later. This port keeps the
// behaviour and the script-facing contract, with the changes to the 64-bit server's dialog noted
// below.
//
// This plugin touches no engine internals at all. It drives the server's own admin GUI with
// window messages: find the dialog by class name, then click its buttons and fill its fields.
//
//     NWNXSetString("SRVADMIN", "BOOTPLAYER",             sAccountName, 0, "");
//     NWNXSetString("SRVADMIN", "BANPLAYERNAME",          sAccountName, 0, "");
//     NWNXSetString("SRVADMIN", "BANPLAYERIP",            sAccountName, 0, "");
//     NWNXSetString("SRVADMIN", "BROADCASTSERVERMESSAGE", sMessage,     0, "");
//     NWNXSetString("SRVADMIN", "SETPLAYERPASSWORD",      sPassword,    0, "");
//     NWNXSetString("SRVADMIN", "SETDMPASSWORD",          sPassword,    0, "");
//     NWNXSetString("SRVADMIN", "SETADMINPASSWORD",       sPassword,    0, "");
//     NWNXSetString("SRVADMIN", "SETELC",                 "true"/"false", 0, "");
//     NWNXSetString("SRVADMIN", "SETDISABLEOVERRIDE",     "true"/"false", 0, "");   // new
//     NWNXSetString("SRVADMIN", "SETDISABLECUSTOMGUI",    "true"/"false", 0, "");   // new
//     NWNXSetString("SRVADMIN", "SHUTDOWNNWN2SERVER",     "",           0, "");
//     NWNXSetString("SRVADMIN", "ADMIN_EXTEND",           "",           0, "");    // new
//     NWNXSetString("SRVADMIN", "ADMIN_REBOOT",           "",           0, "");    // new
//
// Every action also answers through NWNXGetInt with the same function name, returning 1 on
// success and 0 on failure, so a script can tell whether the control was actually found. The
// NWNX4 original was fire-and-forget and gave no way to know.
//
// What changed for the 64-bit server:
//   - The dialog is otherwise identical. Eleven of the twelve control IDs the original used are
//     unchanged, including the window class name.
//   - BANPLAYERCDKEY is GONE. The Ban CD-Key button no longer exists - CD keys are irrelevant on
//     EE - and Ban IP moved down from 0x404 into its place at 0x403. Ported unchanged, the
//     original would click 0x403 for a CD-key ban and silently ban the player's IP instead.
//     The function now refuses and says so.
//   - Two controls the original never knew about are exposed: Disable client Override folders
//     and Disable client custom GUI.
//   - Checkboxes are set by reading the current state and clicking only if it differs, rather
//     than BM_SETCHECK. BM_SETCHECK updates the box without notifying the dialog, so the server
//     may never apply the setting. Clicking does both.
//
// Also adds scheduled restarts, set up in xp_srvadmin.yml.
#include <Plugin.hpp>
#include <Logger.h>
#include <Data.h>
#include <Yaml.h>

#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
    // Control IDs in the server's admin dialog, confirmed against nwn2server64.exe 1.0.23.2136.
    constexpr int IDC_ELC_CHECKBOX         = 0x3EF;
    constexpr int IDC_PLAYERPASSWORD_EDIT  = 0x3F9;
    constexpr int IDC_DMPASSWORD_EDIT      = 0x3FA;
    constexpr int IDC_ADMINPASSWORD_EDIT   = 0x3FB;
    constexpr int IDC_SENDMESSAGE_EDIT     = 0x3FC;
    constexpr int IDC_PLAYERLIST_LISTBOX   = 0x3FE;
    constexpr int IDC_SENDMESSAGE_BUTTON   = 0x400;
    constexpr int IDC_BOOT_BUTTON          = 0x401;
    constexpr int IDC_BANNAME_BUTTON       = 0x402;
    constexpr int IDC_BANIP_BUTTON         = 0x403;   // was 0x404 before Ban CD-Key was removed
    constexpr int IDC_DISABLEOVERRIDE_BOX  = 0x409;   // not present in the 32-bit server
    constexpr int IDC_DISABLECUSTOMGUI_BOX = 0x40A;   // not present in the 32-bit server

    HMODULE GetOwnModule()
    {
        HMODULE hModule = nullptr;
        ::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetOwnModule),
            &hModule);
        return hModule;
    }

    Logger& GetLogger()
    {
        static Logger logger(
            std::filesystem::path(GetFullModulePath(GetOwnModule())).replace_extension(L".log"),
            true,
            true);
        return logger;
    }

    HWND g_serverWindow = nullptr;

    BOOL CALLBACK FindServerWindowProc(HWND hwnd, LPARAM lParam)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId())
        {
            return TRUE;
        }

        wchar_t className[256];
        if (GetClassNameW(hwnd, className, 256)
            && wcscmp(className, L"Exo - BioWare Corp., (c) 1999 - Generic Blank Application") == 0)
        {
            *(HWND*)lParam = hwnd;
            return FALSE;
        }

        return TRUE;
    }

    /// The server's admin dialog, cached once found. It is created during startup and lives for
    /// the life of the process.
    HWND ServerWindow()
    {
        if (g_serverWindow)
        {
            return g_serverWindow;
        }

        // Deliberately not cached until found. Plugins load before the server builds its UI, so
        // the window does not exist yet at load time and only appears once the server is up.
        HWND hwnd = nullptr;
        EnumWindows(FindServerWindowProc, (LPARAM)&hwnd);
        if (hwnd)
        {
            g_serverWindow = hwnd;
        }

        return hwnd;
    }

    /// Sends a keystroke the way the dialog's own TabbingProc subclass expects it. It accepts
    /// VK_RETURN and ignores the scan code, but the full down/up pair is sent so that any other
    /// subclass or hook sees a well-formed sequence.
    void SendKeyStroke(HWND control, UINT vkCode)
    {
        UINT scanCode = MapVirtualKey(vkCode, MAPVK_VK_TO_VSC);
        SendMessageW(control, WM_KEYDOWN, (WPARAM)vkCode, (LPARAM)((scanCode << 16)));
        SendMessageW(control, WM_KEYUP, (WPARAM)vkCode, (LPARAM)((scanCode << 16) | (1 << 30) | (1u << 31)));
    }

    /// Sets an edit field and presses Return, which is what makes the dialog read the value back
    /// into the server's own state. Setting the text alone is not enough.
    bool SetTextField(HWND control, const char* text)
    {
        if (SendMessageA(control, WM_SETTEXT, 0, (LPARAM)text) != TRUE)
        {
            return false;
        }

        SendKeyStroke(control, VK_RETURN);
        return true;
    }

    /// Clicks a checkbox only if it is not already in the wanted state. BM_SETCHECK would change
    /// the box without telling the dialog, so the server would not act on it.
    ///
    /// The box is read back after the click. Without that, this reports the state it asked for
    /// rather than the state the dialog ended up in, and a click the dialog ignored looks
    /// identical to one it honoured.
    bool SetCheckbox(int controlId, bool wanted, const char* what)
    {
        HWND window = ServerWindow();
        if (!window) { GetLogger()("{}: the server's admin window was not found.", what); return false; }

        HWND box = GetDlgItem(window, controlId);
        if (!box) { GetLogger()("{}: control 0x{:03X} is not in this server's dialog.", what, controlId); return false; }

        bool current = SendMessage(box, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (current == wanted)
        {
            GetLogger()("{} -> {} (already)", what, wanted ? "enabled" : "disabled");
            return true;
        }

        SendMessage(box, BM_CLICK, 0, 0);

        bool actual = SendMessage(box, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (actual != wanted)
        {
            GetLogger()("{}: the dialog ignored the click - still {}.", what, actual ? "enabled" : "disabled");
            return false;
        }

        GetLogger()("{} -> {}", what, actual ? "enabled" : "disabled");
        return true;
    }

    /// Selects a player in the list, then clicks the given button. Boot and the bans all work
    /// this way: the button acts on whoever is selected.
    bool ActOnPlayer(const char* playerName, int buttonId, const char* what)
    {
        if (!playerName || !*playerName) { GetLogger()("{}: no player name given.", what); return false; }

        HWND window = ServerWindow();
        if (!window) { GetLogger()("{}: the server's admin window was not found.", what); return false; }

        HWND list = GetDlgItem(window, IDC_PLAYERLIST_LISTBOX);
        HWND button = GetDlgItem(window, buttonId);
        if (!list || !button) { GetLogger()("{}: the player list or button 0x{:03X} is missing.", what, buttonId); return false; }

        int index = (int)SendMessageA(list, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)playerName);
        if (index == LB_ERR)
        {
            GetLogger()("{}: '{}' is not in the player list.", what, playerName);
            return false;
        }

        if (ListBox_SetCurSel(list, index) == LB_ERR)
        {
            GetLogger()("{}: could not select '{}' in the player list.", what, playerName);
            return false;
        }

        SendMessage(button, BM_CLICK, 0, 0);
        GetLogger()("{} '{}'", what, playerName);
        return true;
    }

    bool SetPassword(int controlId, const char* password, const char* what)
    {
        HWND window = ServerWindow();
        if (!window) { GetLogger()("{}: the server's admin window was not found.", what); return false; }

        HWND edit = GetDlgItem(window, controlId);
        if (!edit) { GetLogger()("{}: control 0x{:03X} is missing.", what, controlId); return false; }

        // Deliberately not logged - this is a password.
        bool ok = SetTextField(edit, password ? password : "");
        GetLogger()("{} -> {}", what, ok ? "set" : "FAILED");
        return ok;
    }

    bool BroadcastMessage(const char* message)
    {
        if (!message) { return false; }

        HWND window = ServerWindow();
        if (!window) { GetLogger()("broadcast: the server's admin window was not found."); return false; }

        HWND edit = GetDlgItem(window, IDC_SENDMESSAGE_EDIT);
        HWND button = GetDlgItem(window, IDC_SENDMESSAGE_BUTTON);
        if (!edit || !button) { GetLogger()("broadcast: the message field or button is missing."); return false; }

        SetWindowTextA(edit, message);
        SendMessage(button, BM_CLICK, 0, 0);
        SetWindowTextA(edit, "");

        GetLogger()("broadcast '{}'", message);
        return true;
    }

    /// Same as the window's close box. Clicking Shutdown from a script dropped players into a timeout.
    bool Shutdown()
    {
        HWND window = ServerWindow();
        if (!window) { GetLogger()("shutdown: the server's admin window was not found."); return false; }

        GetLogger()("shutting the server down");
        PostMessageW(window, WM_CLOSE, 0, 0);
        return true;
    }

    bool IsTrue(const char* value)
    {
        return value && _stricmp(value, "true") == 0;
    }

    // Scheduled restarts

    struct Warning
    {
        int minutes = 0;
        std::string message;
    };

    struct Settings
    {
        // restart:
        bool enabled = false;
        double everyHours = 12.0;
        std::vector<Warning> warnings;          // largest first
        std::string shutdownScript;
        int shutdownDelaySeconds = 10;
        int bootTimeoutSeconds = 60;
        int drainSeconds = 10;
        int lockLoginsSeconds = 60;

        // admin_commands:
        int adminExtendMinutes = 60;
        int adminRebootSeconds = 60;
        std::string adminMessage = "The server will shut down in 1 minute.";
    };

    /// xp_srvadmin.yml next to the DLL. Every key is optional.
    Settings LoadSettings()
    {
        Settings s;
        auto path = std::filesystem::path(GetFullModulePath(GetOwnModule())).replace_extension(L".yml");
        if (!std::filesystem::exists(path))
        {
            return s;
        }

        try
        {
            YAML::Node root = YAML::LoadFile(path.string());
            if (YAML::Node r = root["restart"])
            {
                s.enabled              = r["enabled"].as<bool>(s.enabled);
                s.everyHours           = r["every_hours"].as<double>(s.everyHours);
                s.shutdownScript       = r["shutdown_script"].as<std::string>(s.shutdownScript);
                s.shutdownDelaySeconds = r["shutdown_delay_seconds"].as<int>(s.shutdownDelaySeconds);
                s.bootTimeoutSeconds   = r["boot_timeout_seconds"].as<int>(s.bootTimeoutSeconds);
                s.drainSeconds         = r["drain_seconds"].as<int>(s.drainSeconds);
                s.lockLoginsSeconds    = r["lock_logins_seconds"].as<int>(s.lockLoginsSeconds);
                if (YAML::Node ws = r["warnings"]; ws.IsSequence())
                {
                    for (const auto& w : ws)
                    {
                        s.warnings.push_back({ w["minutes"].as<int>(0), w["message"].as<std::string>("") });
                    }
                }
            }
            if (YAML::Node a = root["admin_commands"])
            {
                s.adminExtendMinutes = a["admin_extend"].as<int>(s.adminExtendMinutes);
                s.adminRebootSeconds = a["admin_reboot"].as<int>(s.adminRebootSeconds);
                s.adminMessage       = a["admin_message"].as<std::string>(s.adminMessage);
            }
        }
        catch (const std::exception& e)
        {
            GetLogger()("could not read {}: {}. Scheduled restarts are off.", path.string(), e.what());
            return Settings{};
        }

        std::sort(s.warnings.begin(), s.warnings.end(),
                  [](const Warning& a, const Warning& b) { return a.minutes > b.minutes; });
        return s;
    }

    using Clock = std::chrono::steady_clock;

    enum class Phase { Waiting, ScriptDelay, Boot, Drain, Closed };

    // All of this runs on the main thread (window timer and scripts), so no locking.
    Settings g_settings;
    nwn2::PluginHost g_host;
    Phase g_phase = Phase::Waiting;
    bool g_scheduled = false;
    Clock::time_point g_target;
    std::vector<bool> g_warned;
    Clock::time_point g_phaseStart;
    Clock::time_point g_lastBoot;
    const char* g_reason = "scheduled restart";
    bool g_loginsLocked = false;
    std::string g_savedPassword;

    constexpr UINT_PTR kTimerId = 0x53A1;
    constexpr auto kRebootInterval = std::chrono::seconds(10);   // before booting again
    constexpr uint32_t kModuleObject = 0;                        // the module's object id

    UINT g_msgStart = 0;
    UINT g_msgStop = 0;
    WNDPROC g_originalProc = nullptr;
    HANDLE g_stopEvent = nullptr;

    double SecondsLeft()
    {
        return std::chrono::duration<double>(g_target - Clock::now()).count();
    }

    /// Skip warnings whose time has already passed.
    void ResetWarnings()
    {
        g_warned.assign(g_settings.warnings.size(), false);
        for (size_t i = 0; i < g_warned.size(); ++i)
        {
            g_warned[i] = g_settings.warnings[i].minutes * 60.0 >= SecondsLeft();
        }
    }

    /// Temporary player password so nobody new joins right before a restart.
    void LockLogins()
    {
        HWND edit = GetDlgItem(ServerWindow(), IDC_PLAYERPASSWORD_EDIT);
        if (g_loginsLocked || !edit)
        {
            return;
        }

        char saved[256] = {};
        GetWindowTextA(edit, saved, sizeof(saved));

        std::random_device random;
        std::string password = std::format("restart-{:08x}{:08x}", random(), random());
        if (SetTextField(edit, password.c_str()))
        {
            g_savedPassword = saved;
            g_loginsLocked = true;
            GetLogger()("logins locked for the restart (temporary player password).");
        }
    }

    /// Puts the real password back (on extend, and before closing).
    void UnlockLogins()
    {
        HWND edit = GetDlgItem(ServerWindow(), IDC_PLAYERPASSWORD_EDIT);
        if (!g_loginsLocked || !edit)
        {
            return;
        }

        SetTextField(edit, g_savedPassword.c_str());
        g_loginsLocked = false;
        GetLogger()("logins unlocked; the player password is back as it was.");
    }

    /// Boots everyone by row. Last row first in case a boot removes its row right away.
    int BootAll()
    {
        HWND window = ServerWindow();
        HWND list = GetDlgItem(window, IDC_PLAYERLIST_LISTBOX);
        HWND button = GetDlgItem(window, IDC_BOOT_BUTTON);
        int count = list ? ListBox_GetCount(list) : 0;
        if (!button || count <= 0)
        {
            return 0;
        }

        for (int i = count - 1; i >= 0; --i)
        {
            ListBox_SetCurSel(list, i);
            SendMessage(button, BM_CLICK, 0, 0);
        }

        g_lastBoot = Clock::now();
        GetLogger()("booting {} player(s).", count);
        return count;
    }

    int PlayerCount()
    {
        HWND list = GetDlgItem(ServerWindow(), IDC_PLAYERLIST_LISTBOX);
        return list ? ListBox_GetCount(list) : 0;
    }

    void BeginShutdown(const char* reason)
    {
        if (g_phase != Phase::Waiting)
        {
            return;
        }

        g_reason = reason;
        GetLogger()("shutdown sequence started ({}).", reason);
        if (g_settings.lockLoginsSeconds > 0)
        {
            LockLogins();
        }

        if (!g_settings.shutdownScript.empty())
        {
            GetLogger()("running shutdown_script '{}'.", g_settings.shutdownScript);
            auto result = g_host.RunScript(g_settings.shutdownScript.c_str(), kModuleObject);
            if (!nwn2::Succeeded(result))
            {
                GetLogger()("shutdown_script failed: {}", result.message ? result.message : "no detail");
            }
        }

        g_phase = Phase::ScriptDelay;
        g_phaseStart = Clock::now();
    }

    bool AdminExtend()
    {
        if (g_phase != Phase::Waiting || !g_scheduled)
        {
            GetLogger()("ADMIN_EXTEND: no restart to extend (none scheduled, or already shutting down).");
            return false;
        }

        g_target += std::chrono::minutes(g_settings.adminExtendMinutes);
        ResetWarnings();
        GetLogger()("ADMIN_EXTEND: restart moved back {} minutes ({:.0f} minutes from now).",
                    g_settings.adminExtendMinutes, SecondsLeft() / 60.0);
        return true;
    }

    bool AdminReboot()
    {
        if (g_phase != Phase::Waiting)
        {
            GetLogger()("ADMIN_REBOOT: the shutdown has already begun.");
            return false;
        }

        auto target = Clock::now() + std::chrono::seconds(g_settings.adminRebootSeconds);
        if (!g_scheduled || target < g_target)
        {
            g_target = target;
        }
        g_scheduled = true;
        g_reason = "admin reboot";
        g_warned.assign(g_settings.warnings.size(), true);   // admin_message replaces the countdown

        BroadcastMessage(g_settings.adminMessage.c_str());
        GetLogger()("ADMIN_REBOOT: restarting in {:.0f} seconds.", SecondsLeft());
        return true;
    }

    /// Every 250 ms.
    void Tick()
    {
        auto now = Clock::now();

        switch (g_phase)
        {
            case Phase::Waiting:
            {
                if (!g_scheduled)
                {
                    return;
                }

                // Only announce the latest if several passed at once.
                double left = SecondsLeft();
                const Warning* announce = nullptr;
                for (size_t i = 0; i < g_warned.size(); ++i)
                {
                    if (!g_warned[i] && left <= g_settings.warnings[i].minutes * 60.0)
                    {
                        g_warned[i] = true;
                        announce = &g_settings.warnings[i];
                    }
                }
                if (announce && !announce->message.empty())
                {
                    BroadcastMessage(announce->message.c_str());
                }

                if (g_settings.lockLoginsSeconds > 0)
                {
                    if (left <= g_settings.lockLoginsSeconds) { LockLogins(); }
                    else                                     { UnlockLogins(); }   // after ADMIN_EXTEND
                }

                if (left <= 0)
                {
                    BeginShutdown(g_reason);
                }
                return;
            }

            case Phase::ScriptDelay:
                if (now - g_phaseStart >= std::chrono::seconds(g_settings.shutdownDelaySeconds))
                {
                    BootAll();
                    g_phase = Phase::Boot;
                    g_phaseStart = now;
                }
                return;

            case Phase::Boot:
            {
                int left = PlayerCount();
                if (left == 0 || now - g_phaseStart >= std::chrono::seconds(g_settings.bootTimeoutSeconds))
                {
                    if (left > 0)
                    {
                        GetLogger()("{} player(s) still listed after {} seconds; closing anyway.",
                                    left, g_settings.bootTimeoutSeconds);
                    }
                    GetLogger()("waiting {} seconds for exit scripts.", g_settings.drainSeconds);
                    g_phase = Phase::Drain;
                    g_phaseStart = now;
                }
                else if (now - g_lastBoot >= kRebootInterval)
                {
                    BootAll();
                }
                return;
            }

            case Phase::Drain:
                // Late joiner: boot them and restart the wait.
                if (PlayerCount() > 0 && now - g_lastBoot >= kRebootInterval)
                {
                    BootAll();
                    g_phaseStart = now;
                }
                if (now - g_phaseStart >= std::chrono::seconds(g_settings.drainSeconds))
                {
                    g_phase = Phase::Closed;
                    UnlockLogins();
                    GetLogger()("closing ({}).", g_reason);
                    Shutdown();
                }
                return;

            case Phase::Closed:
                return;
        }
    }

    /// Subclassed so the timer runs on the window's (main) thread.
    LRESULT CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == g_msgStart)
        {
            SetTimer(hwnd, kTimerId, 250, nullptr);
            if (g_settings.enabled)
            {
                g_scheduled = true;
                g_target = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(g_settings.everyHours * 3600.0));
                ResetWarnings();
                GetLogger()("next restart in {:.0f} minutes.", SecondsLeft() / 60.0);
            }
            return 0;
        }
        if (msg == g_msgStop)
        {
            BeginShutdown("service stop");
            return 0;
        }
        if (msg == WM_TIMER && wParam == kTimerId)
        {
            Tick();
            return 0;
        }

        return CallWindowProcW(g_originalProc, hwnd, msg, wParam, lParam);
    }

    /// Waits for the window, hooks it, then passes service stops to the window thread.
    void WatchThread()
    {
        HWND hwnd = nullptr;
        while (!(hwnd = ServerWindow()))
        {
            Sleep(1000);
        }

        g_originalProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&DialogProc)));
        if (!g_originalProc)
        {
            GetLogger()("could not hook the admin window (error {}); restarts will not run.", GetLastError());
            return;
        }
        PostMessageW(hwnd, g_msgStart, 0, 0);

        while (WaitForSingleObject(g_stopEvent, INFINITE) == WAIT_OBJECT_0)
        {
            PostMessageW(hwnd, g_msgStop, 0, 0);
        }
    }

    class XpSrvAdmin : public nwn2::PluginBase
    {
    public:
        XpSrvAdmin()
        {
            GetLogger()("xp_srvadmin loaded.");
        }

        const char* GetPluginId() const override
        {
            return "SRVADMIN";
        }

        void OnInitialize(nwn2::PluginHost host) override
        {
            g_host = host;
            g_settings = LoadSettings();
            GetLogger()("scheduled restarts {}{}.", g_settings.enabled ? "on, every " : "off",
                        g_settings.enabled ? std::format("{} h", g_settings.everyHours) : "");

            g_msgStart = RegisterWindowMessageW(L"NWN2ServerMod.SrvAdmin.Start");
            g_msgStop = RegisterWindowMessageW(L"NWN2ServerMod.SrvAdmin.Stop");

            // Set by the NWN2ServerMod service on stop.
            std::wstring name = L"Local\\NWN2ServerMod.Shutdown." + std::to_wstring(GetCurrentProcessId());
            g_stopEvent = CreateEventW(nullptr, FALSE, FALSE, name.c_str());

            std::thread(WatchThread).detach();
        }

        void OnNWNXSetString(const char* function, const char* param1, int param2, const char* value) override
        {
            Dispatch(function, param1);
        }

        bool OnNWNXGetInt(const char* function, const char* param1, int param2, int& outValue) override
        {
            outValue = Dispatch(function, param1) ? 1 : 0;
            return true;
        }

        void OnNWNXGetString(const char* function, const char* param1, int param2, nwn2::StringResult& result) override
        {
            std::string_view fn(function ? function : "");

            if (fn == "GET SUBCLASS")    { result.Set("SRVADMIN"); }
            else if (fn == "GET VERSION")     { result.Set("0.1.0"); }
            else if (fn == "GET DESCRIPTION") { result.Set("This plugin provides server administration functions."); }
        }

    private:
        /// Returns whether the action was carried out.
        bool Dispatch(const char* function, const char* param1)
        {
            std::string_view fn(function ? function : "");

            if (fn.empty())
            {
                GetLogger()("no function specified.");
                return false;
            }

            if (fn == "BOOTPLAYER")             return ActOnPlayer(param1, IDC_BOOT_BUTTON, "boot");
            if (fn == "BANPLAYERNAME")          return ActOnPlayer(param1, IDC_BANNAME_BUTTON, "ban by name");
            if (fn == "BANPLAYERIP")            return ActOnPlayer(param1, IDC_BANIP_BUTTON, "ban by IP");
            if (fn == "BROADCASTSERVERMESSAGE") return BroadcastMessage(param1);
            if (fn == "SETPLAYERPASSWORD")      return SetPassword(IDC_PLAYERPASSWORD_EDIT, param1, "player password");
            if (fn == "SETDMPASSWORD")          return SetPassword(IDC_DMPASSWORD_EDIT, param1, "DM password");
            if (fn == "SETADMINPASSWORD")       return SetPassword(IDC_ADMINPASSWORD_EDIT, param1, "admin password");
            if (fn == "SETELC")                 return SetCheckbox(IDC_ELC_CHECKBOX, IsTrue(param1), "enforce legal characters");
            if (fn == "SETDISABLEOVERRIDE")     return SetCheckbox(IDC_DISABLEOVERRIDE_BOX, IsTrue(param1), "disable client override folders");
            if (fn == "SETDISABLECUSTOMGUI")    return SetCheckbox(IDC_DISABLECUSTOMGUI_BOX, IsTrue(param1), "disable client custom GUI");
            if (fn == "SHUTDOWNNWN2SERVER")     return Shutdown();
            if (fn == "ADMIN_EXTEND")           return AdminExtend();
            if (fn == "ADMIN_REBOOT")           return AdminReboot();

            if (fn == "BANPLAYERCDKEY")
            {
                // Refusing rather than falling through: the button this used to click is gone, and
                // the one that took its ID bans by IP instead.
                GetLogger()("BANPLAYERCDKEY is not available - the 64-bit server has no Ban CD-Key "
                            "button. Use BANPLAYERIP or BANPLAYERNAME.");
                return false;
            }

            GetLogger()("unknown function '{}'.", fn);
            return false;
        }
    };
}

NWN2_EXPORT_PLUGIN(XpSrvAdmin)
