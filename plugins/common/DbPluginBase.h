#pragma once
// Shared base for the ported NWNX4 database plugins.
//
// Original: NWNX4's src/plugins/database/dbplugin.{h,cpp}
// Copyright (C) 2007 Ingmar Stieger (Papillon), GPL v2 or later.
//
// NWNX4 had xp_sqlite and xp_mysql derive from one DBPlugin that owned the whole script-facing
// SQL surface and dispatched to virtuals for the parts that differ. That split is worth keeping:
// the contract scripts see is identical for both, and only the backend changes.
//
// What changed from the NWNX4 version:
//   Plugin/DBPlugin              -> nwn2::PluginBase
//   GetFunctionClass("SQL")      -> GetPluginId(), still taken from the config's "class" key
//   char returnBuffer[64K]       -> nwn2::StringResult, which owns the storage
//   BYTE* ReadScorcoData(...)    -> OnGetBinaryData + BinaryDataResult::Allocate
//   LogNWNX                      -> Logger
//   SimpleIniConfig (.ini)       -> Yaml.h (.yml), matching nwn2mod.config
//
// ReadScorcoData is the only one that changed shape rather than just name. NWNX4 allocated the
// buffer it returned on the *engine's* heap through NWN2_HeapMgr, because the engine freed it.
// The C ABI hands the plugin a sink the loader owns instead, so the allocation and the matching
// free both stay on the loader's side and a plugin never touches the engine's allocator at all.
#include <Plugin.hpp>
#include <Logger.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace nwn2ports
{
    /// The NWNX "SQL" function class, as nwnx_sql.nss calls it. A backend overrides the protected
    /// hooks; this class owns the dispatch so every database plugin answers identically.
    class DbPluginBase : public nwn2::PluginBase
    {
    public:
        DbPluginBase(std::string pluginId, Logger& logger)
            : _logger(logger), _pluginId(std::move(pluginId))
        {
        }

        const char* GetPluginId() const override { return _pluginId.c_str(); }

        bool OnNWNXGetInt(const char* function, const char* param1, int32_t param2,
                          int32_t& outValue) override
        {
            std::string_view fn(function ? function : "");

            if (fn.empty())
            {
                _logger("GetInt: no function specified.");
                outValue = -1;
                return true;
            }

            if (fn == "EXEC")              { outValue = Execute(param1 ? param1 : "") ? 1 : 0; return true; }
            if (fn == "FETCH")             { outValue = Fetch(param1); return true; }
            if (fn == "GET AFFECTED ROWS") { outValue = GetAffectedRows(); return true; }
            if (fn == "GET ERRNO")         { outValue = GetErrno(); return true; }
            if (fn == "GET INSID")         { outValue = GetLastInsertID(); return true; }

            // NWNX4 returned 0 for anything it did not recognise, which a script cannot tell from
            // a genuine 0. Kept, because changing it would silently alter existing scripts.
            _logger("GetInt: unknown function '{}'.", fn);
            outValue = 0;
            return true;
        }

        void OnNWNXSetString(const char* function, const char* param1, int32_t param2,
                             const char* value) override
        {
            std::string_view fn(function ? function : "");

            if (fn.empty())
            {
                _logger("SetString: no function specified.");
                return;
            }

            if (fn == "EXEC")
            {
                Execute(param1 ? param1 : "");
            }
            else if (fn == "SETSCORCOSQL")
            {
                _scorcoSql = param1 ? param1 : "";
            }
            else
            {
                _logger("SetString: unknown function '{}'.", fn);
            }
        }

        void OnNWNXGetString(const char* function, const char* param1, int32_t param2,
                             nwn2::StringResult& result) override
        {
            std::string_view fn(function ? function : "");

            if (fn.empty())
            {
                _logger("GetString: no function specified.");
                return;
            }

            if (fn == "GETDATA")
            {
                std::string value;
                GetData(param2, value);

                // NWNX4 handed back its buffer whether or not the column held anything, so a null
                // column reaches the script as "". Setting it explicitly keeps that, since leaving
                // the sink untouched would instead mean "no value".
                result.Set(value.c_str());
                return;
            }

            if (fn == "GET ESCAPE STRING")
            {
                std::string value;
                GetEscapeString(param1 ? param1 : "", value);
                result.Set(value.c_str());
                return;
            }

            if (fn == "GET ERROR MESSAGE")
            {
                const char* message = GetErrorMessage();
                if (message)
                {
                    result.Set(message);
                }
                return;
            }

            // The generic queries NWNX4's Plugin base answered for every plugin.
            if (fn == "GET SUBCLASS")    { result.Set(GetSubclass()); return; }
            if (fn == "GET VERSION")     { result.Set(GetVersion()); return; }
            if (fn == "GET DESCRIPTION") { result.Set(GetDescription()); return; }

            _logger("GetString: unknown function '{}'.", fn);
        }

        /// SCORCO's store half. The engine owns this buffer and we only read it, so nothing is
        /// allocated on the path that persists data.
        bool OnSetBinaryData(const char* varName, const char* player,
                             const uint8_t* data, size_t size) override
        {
            return WriteScorcoData(data, size);
        }

        /// SCORCO's retrieve half. varName carries the "FETCHMODE" marker the original passed
        /// through as its param.
        void OnGetBinaryData(const char* varName, const char* player,
                             nwn2::BinaryDataResult& result) override
        {
            if (!ReadScorcoData(varName ? varName : "", result))
            {
                // An untouched sink is how the ABI spells "no data"; Clear undoes a partial
                // allocation if the backend allocated and then failed.
                result.Clear();
            }
        }

        void OnUnhandledException(const char* callback, const char* what) noexcept override
        {
            _logger("exception escaping {}: {}", callback ? callback : "?", what ? what : "?");
        }

    protected:
        /// Runs a statement. True if it succeeded.
        virtual bool Execute(const char* query) { return false; }

        /// Advances to the next row. Non-zero while rows remain.
        virtual int Fetch(const char* param) { return 0; }

        /// Reads one column of the current row. 0 on success, -1 if there is no value.
        virtual int GetData(int column, std::string& out) { out.clear(); return -1; }

        virtual int GetAffectedRows() { return -1; }
        virtual void GetEscapeString(const char* str, std::string& out) { out.clear(); }
        virtual int GetErrno() { return 0; }
        virtual const char* GetErrorMessage() { return nullptr; }
        virtual int GetLastInsertID() { return 0; }

        /// Writes a serialized object, using the statement SETSCORCOSQL left in _scorcoSql.
        virtual bool WriteScorcoData(const uint8_t* data, size_t size) { return false; }

        /// Reads a serialized object into result. False means "no data".
        virtual bool ReadScorcoData(const char* param, nwn2::BinaryDataResult& result) { return false; }

        virtual const char* GetSubclass() const = 0;
        virtual const char* GetVersion() const = 0;
        virtual const char* GetDescription() const = 0;

        Logger& _logger;

        /// The statement SETSCORCOSQL parked for the next SCO/RCO call.
        std::string _scorcoSql;

    private:
        std::string _pluginId;
    };
}
