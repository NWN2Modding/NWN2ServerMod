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
#include <Plugin.hpp>
#include <Logger.h>
#include <Data.h>

#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

#include <cstring>
#include <string>
#include <string_view>

namespace
{
    // Control IDs in the server's admin dialog, confirmed against nwn2server64.exe 1.0.23.2136.
    constexpr int IDC_ELC_CHECKBOX         = 0x3EF;
    constexpr int IDC_PLAYERPASSWORD_EDIT  = 0x3F9;
    constexpr int IDC_DMPASSWORD_EDIT      = 0x3FA;
    constexpr int IDC_ADMINPASSWORD_EDIT   = 0x3FB;
    constexpr int IDC_SENDMESSAGE_EDIT     = 0x3FC;
    constexpr int IDC_PLAYERLIST_LISTBOX   = 0x3FE;
    constexpr int IDC_SHUTDOWN_BUTTON      = 0x3FF;
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

    bool Shutdown()
    {
        HWND window = ServerWindow();
        if (!window) { GetLogger()("shutdown: the server's admin window was not found."); return false; }

        HWND button = GetDlgItem(window, IDC_SHUTDOWN_BUTTON);
        if (!button) { GetLogger()("shutdown: the button is missing."); return false; }

        GetLogger()("shutting the server down");
        SendMessage(button, BM_CLICK, 0, 0);
        return true;
    }

    bool IsTrue(const char* value)
    {
        return value && _stricmp(value, "true") == 0;
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
            else if (fn == "GET VERSION")     { result.Set("0.0.3"); }
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
