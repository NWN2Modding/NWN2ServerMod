#pragma once
// Turning an NWScript object id into the engine's CNWSCreature.
//
// CServerExoApp::GetCreatureByGameObjectID is the function that does it, and for a long time it
// looked unreachable: it is one of 36 byte-identical siblings generated from the same template
// (GetCreatureByGameObjectID, GetItemByGameObjectID, GetDoorByGameObjectID and so on), differing
// only in a RIP-relative displacement. No byte pattern can tell them apart.
//
// It is reachable through a caller instead. CNWSCreature::AddPickPocketActions has a prologue that
// patterns uniquely, and its second direct call is to the function we want:
//
//     +0x46  call CNWSCreatureStats::GetCanUseSkill
//     +0x5A  call CServerExoApp::GetCreatureByGameObjectID      <- this one
//
// Verified at the same offset in two different Patch 3 server builds, so the rel32 displacement at
// +0x5B is read and resolved rather than the function being searched for directly.
//
// Two further facts from its disassembly, both of which make it easy to call:
//
//   cmp edx, 0x7f000000          it rejects OBJECT_INVALID itself
//   mov rax, [rip + ...]         it loads the app manager from a global
//
// so the "this" slot is dead - the function never reads RCX - and no CServerExoApp pointer has to
// be found. The same shape as CVirtualMachine::RunScript, which the loader already calls this way.
#include <Plugin.hpp>
#include <Logger.h>

#include <cstdint>
#include <string>

namespace nwn2ports
{
    /// Resolves and calls CServerExoApp::GetCreatureByGameObjectID.
    ///
    /// Returns null until Resolve has succeeded, and for any id the engine does not recognise.
    /// The returned pointer is the engine's own object: valid for the duration of the call that
    /// obtained it, and never to be freed.
    class CreatureLookup
    {
    public:
        /// The prologue of CNWSCreature::AddPickPocketActions - the anchor, not the target.
        static constexpr const char* kAnchorPattern =
            "48 89 5C 24 18 89 54 24 10 57 48 81 EC E0 00 00 00 48 8B 01 41 0F B6 F8";

        /// Where the call to GetCreatureByGameObjectID sits inside that function.
        static constexpr size_t kCallOffset = 0x5A;

        /// Finds the function through its caller. False if the anchor could not be found or the
        /// bytes at the call site are not a direct call.
        bool Resolve(nwn2::PluginHost host, Logger& logger)
        {
            auto* addresses = host.QueryService<NWN2AddressService>();
            if (!addresses || !addresses->FindUnique)
            {
                logger(Logger::Level::Error,
                       "this loader does not provide IAddressService, so creature lookup is "
                       "unavailable.");
                return false;
            }

            NWN2Result error{};
            auto* anchor = static_cast<const unsigned char*>(
                addresses->FindUnique(addresses->self, kAnchorPattern, &error));

            if (!anchor)
            {
                logger(Logger::Level::Error,
                       "could not find CNWSCreature::AddPickPocketActions, which is the anchor for "
                       "creature lookup: {}", error.message ? error.message : "no match");
                return false;
            }

            const unsigned char* callSite = anchor + kCallOffset;
            if (*callSite != 0xE8)
            {
                logger(Logger::Level::Error,
                       "the anchor's call site does not hold a direct call (found {:02X}); this "
                       "build lays the function out differently.", *callSite);
                return false;
            }

            // E8 rel32: the target is relative to the end of the instruction.
            int32_t displacement = 0;
            std::memcpy(&displacement, callSite + 1, sizeof(displacement));
            auto* target = callSite + 5 + displacement;

            _getCreature = reinterpret_cast<GetCreatureFunc>(const_cast<unsigned char*>(target));

            logger("CServerExoApp::GetCreatureByGameObjectID at 0x{:016X}, via its caller at "
                   "0x{:016X}", reinterpret_cast<uintptr_t>(target),
                   reinterpret_cast<uintptr_t>(anchor));
            return true;
        }

        bool Resolved() const { return _getCreature != nullptr; }

        /// The engine's CNWSCreature for an object id, or null. The engine rejects OBJECT_INVALID
        /// itself, so no guard is needed here beyond having resolved the function.
        void* operator()(uint32_t objectId) const
        {
            return _getCreature ? _getCreature(nullptr, objectId) : nullptr;
        }

    private:
        // The first parameter is a dead "this" slot: the function loads the app manager from a
        // global and never reads RCX.
        using GetCreatureFunc = void*(__fastcall*)(void* deadThis, uint32_t objectId);

        GetCreatureFunc _getCreature = nullptr;
    };
}
