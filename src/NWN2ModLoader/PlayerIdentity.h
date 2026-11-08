#pragma once
// Records the platform identity of every connecting player, and exposes it to NWScript.
//
// EE replaced the pre-EE login name and CD key with a UserInfo struct. Of its fields only four
// cross: the connect packet carries one string, parsed as "%s.%s.%llu.%c" - platform name,
// modern user id, platform user id, platform tag. The platform user id is the only stable
// per-account identifier a server receives, and none of it reaches a script on its own:
//
//   - GetPCPublicCDKey() returns "" for everybody. Nothing in the engine calls
//     CNetLayerPlayerInfo::AddCDKey or SetCDKey any more, so m_lstKeys is always empty,
//     GetNumCDKeys() is always 0, and ExecuteCommandGetPCPublicCDKey takes its empty branch.
//   - the platform user id is not surfaced anywhere.
//
// This only observes. The servervault folder, logins and the engine's own GetPCPublicCDKey() are
// all untouched.
//
// One hook, on UserInfo::EvaluateUniqueNameAndId, which runs once the identity has been parsed.

#include <Logger.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace nwn2mod
{
    /// What was recorded for one connecting account.
    struct PlayerIdentityRecord
    {
        std::string platformName;    ///< the store account name, or a 1.23 client's login name
        std::string platformUserId;  ///< decimal; "0" when the client sent no platform identity
        uint8_t platform = 0;        ///< UserPlatform tag
    };

    /// Records the identity of each connecting player and answers NWScript queries about it.
    class PlayerIdentity
    {
    public:
        /// Attaches the hook. False if it could not be found or attached. 
        static bool Install(std::shared_ptr<Logger> logger);

        /// Detaches. Safe to call when not installed.
        static void Uninstall();

        static bool Installed() noexcept { return _installed; }

        /// Answers NWNXGetString for the loader's reserved class. Returns nullptr when the
        /// function name is not one of ours, so the caller can fall through to the plugins.
        ///
        ///   PLATFORM_ID   param1 is the platform name (what GetPCPlayerName returns). The platform
        ///                 user id as decimal; "0" if the client sent none (a 1.23 client), "" if
        ///                 no player of that name has connected since the server started.
        static const char* Query(const char* function, const char* param1);

    private:
        static void EvaluateHook(void* userInfo);

        using EvaluateFunc = void (*)(void*);

        static EvaluateFunc _realEvaluate;
        static std::shared_ptr<Logger> _logger;
        static bool _installed;

        /// platform name -> record. Small, and only written on connect.
        static std::unordered_map<std::string, PlayerIdentityRecord> _records;

        /// Guards _records between the connect path and the NWNX query.
        static std::mutex _lock;
    };
}
