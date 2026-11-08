#include "PlayerIdentity.h"
#include "PEPattern.h"

#include <detours.h>

#include <cstring>
#include <string_view>

namespace
{
    // CExoString [sizeof = 16]: char* m_sString; unsigned long m_nBufferLength.
    struct CExoStringLayout
    {
        char* m_sString;
        uint32_t m_nBufferLength;
        uint32_t _pad;
    };

    // UserInfo [sizeof = 112]. From the Patch 3 GOG client PDB, which carries the full server
    // symbol set, cross-checked against nwn2server64.exe.
    constexpr size_t kPlatformName = 0x00;
    constexpr size_t kCustomName = 0x10;
    constexpr size_t kUniqueId = 0x40;
    constexpr size_t kPlatformUserId = 0x50;
    constexpr size_t kModernUserId = 0x58;
    constexpr size_t kPlatform = 0x68;

    /// UserInfo::EvaluateUniqueNameAndId. 50 bytes identical in both Patch 3 builds, then a
    /// RIP-relative displacement; matches exactly once in each.
    constexpr const char* kEvaluatePattern =
        "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D 6C 24 F0 "
        "48 81 EC 10 01 00 00 4C 8B F9 45 33 E4 4C 89 64 24 70 44 89 64 24 78 "
        "4C 8B 41 50 48 8D 15";

    const char* PlatformLabel(uint8_t platform)
    {
        switch (platform)
        {
            case 0:  return "Invalid";
            case 76: return "Legacy";
            case 77: return "Service";
            case 78: return "NintendoSwitch";
            case 80: return "Playstation";
            case 83: return "Steam";
            case 88: return "Xbox";
            default: return "?";
        }
    }

    std::string_view Text(const void* userInfo, size_t offset)
    {
        auto* s = reinterpret_cast<const CExoStringLayout*>(
            static_cast<const unsigned char*>(userInfo) + offset);
        return s->m_sString ? std::string_view(s->m_sString) : std::string_view{};
    }

    uint64_t PlatformUserIdOf(const void* userInfo)
    {
        uint64_t value = 0;
        std::memcpy(&value, static_cast<const unsigned char*>(userInfo) + kPlatformUserId,
                    sizeof(value));
        return value;
    }
}

namespace nwn2mod
{
    PlayerIdentity::EvaluateFunc PlayerIdentity::_realEvaluate = nullptr;
    std::shared_ptr<Logger> PlayerIdentity::_logger;
    bool PlayerIdentity::_installed = false;
    std::unordered_map<std::string, PlayerIdentityRecord> PlayerIdentity::_records;
    std::mutex PlayerIdentity::_lock;

    // ---- the parsed identity ------------------------------------------------------------------

    void PlayerIdentity::EvaluateHook(void* userInfo)
    {
        _realEvaluate(userInfo);

        if (!userInfo)
        {
            return;
        }

        const std::string_view platformName = Text(userInfo, kPlatformName);
        const std::string_view customName = Text(userInfo, kCustomName);

        // The deserialising constructor calls this twice: once after setting the platform user id
        // but before the names and tag, and again once the object is complete. The first pass has
        // no names at all - skip it. Any real identity carries at least a platform name.
        if (platformName.empty() && customName.empty())
        {
            return;
        }

        const uint64_t platformUserId = PlatformUserIdOf(userInfo);
        const uint8_t platform = *(static_cast<const unsigned char*>(userInfo) + kPlatform);

        {
            std::lock_guard<std::mutex> guard(_lock);
            auto& record = _records[std::string(platformName)];
            record.platformName = platformName;
            record.platformUserId = std::to_string(platformUserId);
            record.platform = platform;
        }

        if (_logger)
        {
            (*_logger)("player identity: platform_name='{}' platform={} ({}) platform_user_id={} "
                       "modern_user_id='{}' unique_id='{}'",
                       platformName, PlatformLabel(platform), platform, platformUserId,
                       Text(userInfo, kModernUserId), Text(userInfo, kUniqueId));
        }
    }

    // ---- the script-facing query ----------------------------------------------------------------

    const char* PlayerIdentity::Query(const char* function, const char* param1)
    {
        if (std::string_view(function ? function : "") != "PLATFORM_ID")
        {
            return nullptr;   // not ours; let the plugins have it
        }

        // The result outlives this call only until the next one, which is all the engine needs -
        // it copies the string out immediately.
        static std::string result;
        result.clear();

        std::lock_guard<std::mutex> guard(_lock);
        auto it = _records.find(param1 ? param1 : "");
        if (it != _records.end())
        {
            result = it->second.platformUserId;
        }

        return result.c_str();
    }

    // ---- install ------------------------------------------------------------------------------

    bool PlayerIdentity::Install(std::shared_ptr<Logger> logger)
    {
        if (_installed)
        {
            return true;
        }

        _logger = std::move(logger);

        auto evaluate = PEPattern::FindPatternUnique(L"NWN2Server64.exe", kEvaluatePattern);
        if (!evaluate)
        {
            if (_logger)
            {
                (*_logger)("log_player_identities: could not find "
                           "UserInfo::EvaluateUniqueNameAndId ({}); identities are not recorded.",
                           evaluate.error());
            }
            return false;
        }

        _realEvaluate = reinterpret_cast<EvaluateFunc>(*evaluate);

        ::DetourTransactionBegin();
        ::DetourUpdateThread(::GetCurrentThread());
        ::DetourAttach(&(PVOID&)_realEvaluate, reinterpret_cast<PVOID>(&EvaluateHook));
        LONG attached = ::DetourTransactionCommit();

        if (attached != NO_ERROR)
        {
            if (_logger)
            {
                (*_logger)("log_player_identities: Detours refused to attach (error {}); "
                           "identities are not recorded.", attached);
            }
            _realEvaluate = nullptr;
            return false;
        }

        _installed = true;

        if (_logger)
        {
            (*_logger)("log_player_identities: recording each connecting player's platform "
                       "identity. Scripts can read it with "
                       "NWNXGetString(\"NWN2MOD\", \"PLATFORM_ID\", GetPCPlayerName(oPC), 0).");
        }

        return true;
    }

    void PlayerIdentity::Uninstall()
    {
        if (!_installed)
        {
            return;
        }

        ::DetourTransactionBegin();
        ::DetourUpdateThread(::GetCurrentThread());
        ::DetourDetach(&(PVOID&)_realEvaluate, reinterpret_cast<PVOID>(&EvaluateHook));
        ::DetourTransactionCommit();

        _installed = false;
        _realEvaluate = nullptr;
    }
}
