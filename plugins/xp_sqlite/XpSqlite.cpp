// xp_sqlite, ported from NWNX4 to the NWN2ServerMod C ABI.
//
// Original: Copyright (C) 2006-2007 Ingmar Stieger (Papillon), GPL v2 or later. This port keeps
// the script-facing contract identical; the plugin interface and the SQLite version changed.
//
// SQL from NWScript, via nwnx_sql.nss:
//
//     NWNXSetString("SQL", "EXEC", sQuery, 0, "");        run a statement
//     NWNXGetInt   ("SQL", "FETCH", "", 0);               step to the next row
//     NWNXGetString("SQL", "GETDATA", "", nCol);          read a column of the current row
//     NWNXGetString("SQL", "GET ESCAPE STRING", s, 0);    quote a value for use in a query
//     NWNXGetInt   ("SQL", "GET AFFECTED ROWS", "", 0);
//     NWNXGetInt   ("SQL", "GET INSID", "", 0);           last inserted rowid
//
// and SCORCO for whole objects, where SETSCORCOSQL parks the statement that the following
// StoreCampaignObject or RetrieveCampaignObject then runs.
//
// What changed from the NWNX4 version:
//   - DBPlugin subclass              -> nwn2ports::DbPluginBase, which owns the dispatch
//   - xp_sqlite.ini                  -> xp_sqlite.yml, matching nwn2mod.config
//   - NWN2_HeapMgr for RCO buffers   -> the loader's BinaryDataResult sink
//   - SQLite 3.3.17, locally patched -> stock SQLite, unmodified (see the note below)
//
// On the SQLite version: NWNX4 shipped 3.3.17 from 2007 with a patch to sqlite3.c that invented
// an extended error code, SQLITE_ERROR_OPENSTMT, so that a refused COMMIT would arrive as an
// SQLITE_ERROR variant rather than SQLITE_BUSY. This port uses stock SQLite instead and handles
// the real SQLITE_BUSY, so nothing has to be patched into the library and it can be updated by
// dropping in a newer amalgamation.
#include "../common/DbPluginBase.h"
#include "../common/LogConfig.h"

#include <Data.h>
#include <Yaml.h>

#include "lib/sqlite3.h"

#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <filesystem>
#include <optional>
#include <string>

namespace
{
    /// This DLL's own module handle, so the log, the config and the database all land next to the
    /// DLL rather than next to whatever process loaded it.
    HMODULE GetOwnModule()
    {
        HMODULE hModule = nullptr;
        ::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&GetOwnModule),
            &hModule);
        return hModule;
    }

    std::filesystem::path OwnPath()
    {
        return std::filesystem::path(GetFullModulePath(GetOwnModule()));
    }

    Logger& GetLogger()
    {
        static Logger logger(OwnPath().replace_extension(L".log"), true, true);
        return logger;
    }

    /// The xp_sqlite.yml schema. Every field is optional, so a missing file just means defaults.
    struct XpSqliteConfig
    {
        /// The function class scripts call this plugin by. NWNX4 called this key "class" too.
        std::optional<std::string> plugin_class;

        /// The database file. A relative path is resolved against this DLL's directory.
        std::optional<std::string> file;

        /// Wrap every statement in one transaction spanning the server's whole run. See the
        /// warning in the sample config - a crash loses everything since startup.
        std::optional<bool> wrap_transaction;

        /// Log verbosity: none, error, warning, info, debug or trace. NWNX4's key and values.
        std::optional<std::string> log_level;

        /// Rotate the log past this size. Accepts a K, M or G suffix; 0 disables rotation.
        std::optional<std::string> log_max_file_size;

        /// PRAGMA journal_mode. Unset leaves SQLite's own default (delete).
        std::optional<std::string> journal_mode;

        /// PRAGMA synchronous. Unset leaves SQLite's own default (full).
        std::optional<std::string> synchronous;
    };
}

/// <summary>Lets <c>YAML::Node</c> decode <see cref="XpSqliteConfig"/>.</summary>
template <>
struct YAML::convert<XpSqliteConfig>
{
    static bool decode(const Node& node, XpSqliteConfig& c)
    {
        if (!node.IsMap())
        {
            return false;
        }

        // "class" is the key NWNX4 used; it cannot be a field name here.
        c.plugin_class     = node["class"].as<std::optional<std::string>>(std::nullopt);
        c.file             = node["file"].as<std::optional<std::string>>(std::nullopt);
        c.wrap_transaction = node["wrap_transaction"].as<std::optional<bool>>(std::nullopt);
        c.journal_mode     = node["journal_mode"].as<std::optional<std::string>>(std::nullopt);
        c.synchronous      = node["synchronous"].as<std::optional<std::string>>(std::nullopt);
        c.log_level         = node["log_level"].as<std::optional<std::string>>(std::nullopt);
        c.log_max_file_size = node["log_max_file_size"].as<std::optional<std::string>>(std::nullopt);
        return true;
    }
};

namespace
{
    /// Reads xp_sqlite.yml from next to the DLL. A missing file is not an error - it means run on
    /// defaults - but a malformed one is, and is logged rather than silently ignored.
    XpSqliteConfig LoadConfig()
    {
        auto path = OwnPath().replace_extension(L".yml");
        if (!std::filesystem::exists(path))
        {
            GetLogger()("no config at {}; using defaults.", path.string());
            return {};
        }

        auto config = Yaml::FromFile<XpSqliteConfig>(path);
        if (!config)
        {
            GetLogger()("could not read {}: {}. Using defaults.", path.string(), config.error());
            return {};
        }

        return *config;
    }

    /// Read once. The base class needs the function class before the constructor body runs, so
    /// without this the file would be parsed twice and a missing one reported twice.
    const XpSqliteConfig& Config()
    {
        static XpSqliteConfig config = LoadConfig();
        return config;
    }

    class XpSqlite : public nwn2ports::DbPluginBase
    {
    public:
        XpSqlite()
            : DbPluginBase(Config().plugin_class.value_or("SQL"), GetLogger())
            , _config(Config())
        {
nwn2ports::ApplyLogConfig(GetLogger(), _config);

            _wrapTransaction = _config.wrap_transaction.value_or(false);

            // A relative database path is relative to this DLL, not to the server's working
            // directory. The engine keeps its own campaign databases in the install directory, so
            // defaulting there would invite a collision.
            std::filesystem::path dbPath(_config.file.value_or("sqlite.db"));
            if (dbPath.is_relative())
            {
                dbPath = OwnPath().parent_path() / dbPath;
            }
            _dbFile = dbPath.string();

            GetLogger()("xp_sqlite loaded, built against SQLite {}.", SQLITE_VERSION);
            GetLogger()("database file is {}", _dbFile);

            if (_wrapTransaction)
            {
                GetLogger()("WARNING: wrap_transaction is on. Everything written since server "
                            "start is lost if the server does not shut down cleanly.");
            }

            Connect();
        }

        ~XpSqlite() override { Disconnect(); }

    protected:
        const char* GetSubclass() const override { return "SQLite"; }
        const char* GetVersion() const override { return "1.1.0"; }
        const char* GetDescription() const override
        {
            return "This plugin provides database storage, using SQLite as the database engine.";
        }

        bool Execute(const char* query) override
        {
            if (!_db)
            {
                GetLogger()("Execute: no database connection.");
                return false;
            }

            GetLogger()("Executing: {}", query);

            sqlite3_stmt* newStmt = nullptr;
            int rc = sqlite3_prepare_v2(_db, query, -1, &newStmt, nullptr);
            if (rc != SQLITE_OK)
            {
                GetLogger()("SQL error preparing '{}': {}", query, sqlite3_errmsg(_db));
                SafeFinalize(&newStmt);

                // Throw away the last result set if a SELECT failed, so a later FETCH does not
                // walk rows belonging to a different query.
                if (_strnicmp(query, "SELECT", 6) == 0)
                {
                    SafeFinalize(&_stmt);
                }

                return false;
            }

            rc = sqlite3_step(newStmt);
            switch (rc & 0xff)
            {
                case SQLITE_DONE:
                    // A statement that returned an empty result set still has column names, unlike
                    // one that returns no result set at all (an UPDATE, say). Clearing _stmt in
                    // that case is what tells FETCH there is no data.
                    if (sqlite3_column_name(newStmt, 0) != nullptr)
                    {
                        SafeFinalize(&_stmt);
                    }
                    SafeFinalize(&newStmt);
                    return true;

                case SQLITE_ROW:
                    SafeFinalize(&_stmt);
                    _stmt = newStmt;
                    _firstFetch = true;
                    return true;

                default:
                    // NWNX4 handled only SQLITE_ERROR here and fell out of the switch to "success"
                    // for anything else. A COMMIT refused because writes are still open returns
                    // SQLITE_BUSY, so that fall-through reported a commit that did not happen as
                    // though it had. Anything that is not DONE or ROW is a failure.
                    GetLogger()("SQL error running '{}': {} ({})",
                                query, sqlite3_errmsg(_db), sqlite3_extended_errcode(_db));
                    SafeFinalize(&newStmt);
                    return false;
            }
        }

        int Fetch(const char* param) override
        {
            // The original stepped without checking, and sqlite3_step(nullptr) is a crash.
            if (!_stmt)
            {
                return false;
            }

            int rc;
            if (_firstFetch)
            {
                // Execute already stepped once to find out whether there were rows at all.
                _firstFetch = false;
                rc = SQLITE_ROW;
            }
            else
            {
                rc = sqlite3_step(_stmt);
                if ((rc & 0xff) == SQLITE_ERROR)
                {
                    GetLogger()("SQL error fetching: {}", sqlite3_errmsg(_db));
                }
            }

            if ((rc & 0xff) == SQLITE_ROW)
            {
                return true;
            }

            SafeFinalize(&_stmt);
            return false;
        }

        int GetData(int column, std::string& out) override
        {
            out.clear();

            if (!_stmt)
            {
                GetLogger()("GetData: no statement has been executed.");
                return -1;
            }

            const char* text = reinterpret_cast<const char*>(sqlite3_column_text(_stmt, column));
            if (!text)
            {
                return -1;
            }

            // column_bytes, not strlen: a TEXT value is allowed to contain embedded nulls, and
            // std::string carries them where the original fixed buffer could not.
            out.assign(text, static_cast<size_t>(sqlite3_column_bytes(_stmt, column)));
            return 0;
        }

        int GetAffectedRows() override { return _db ? sqlite3_changes(_db) : -1; }

        void GetEscapeString(const char* str, std::string& out) override
        {
            out.clear();

            if (!str || !*str)
            {
                return;
            }

            char* quoted = sqlite3_mprintf("%q", str);
            if (quoted)
            {
                out = quoted;
                sqlite3_free(quoted);
            }
        }

        int GetErrno() override { return _db ? sqlite3_errcode(_db) : 0; }

        const char* GetErrorMessage() override { return _db ? sqlite3_errmsg(_db) : nullptr; }

        int GetLastInsertID() override
        {
            // NWNX's script API is 32-bit, so a rowid past 2^31 cannot be represented. SQLite
            // returns an int64; truncating matches the original and the script contract.
            return _db ? static_cast<int>(sqlite3_last_insert_rowid(_db)) : 0;
        }

        bool WriteScorcoData(const uint8_t* data, size_t size) override
        {
            if (!_db)
            {
                GetLogger()("SCO: no database connection.");
                return false;
            }

            GetLogger()("SCO query: {}", _scorcoSql);

            SafeFinalize(&_stmt);

            // nwnx_sql.nss writes these statements with %s where the blob goes, which is MySQL's
            // placeholder. Rewrite it to SQLite's.
            std::string sql = _scorcoSql;
            auto match = sql.find("%s");
            if (match != std::string::npos)
            {
                sql.replace(match, 2, "?1");
            }

            int rc = sqlite3_prepare_v2(_db, sql.c_str(), -1, &_stmt, nullptr);
            if (rc != SQLITE_OK)
            {
                GetLogger()("SCO: cannot prepare '{}': {}", sql, sqlite3_errmsg(_db));
                SafeFinalize(&_stmt);
                return false;
            }

            // SQLITE_STATIC: the engine's buffer outlives the step below, so SQLite need not copy.
            rc = sqlite3_bind_blob(_stmt, 1, data, static_cast<int>(size), SQLITE_STATIC);
            if (rc != SQLITE_OK)
            {
                GetLogger()("SCO: cannot bind blob: {}", sqlite3_errmsg(_db));
                SafeFinalize(&_stmt);
                return false;
            }

            rc = sqlite3_step(_stmt);
            SafeFinalize(&_stmt);

            if ((rc & 0xff) != SQLITE_DONE)
            {
                GetLogger()("SCO: store failed: {} ({})", sqlite3_errmsg(_db), sqlite3_extended_errcode(_db));
                return false;
            }

            return true;
        }

        bool ReadScorcoData(const char* param, nwn2::BinaryDataResult& result) override
        {
            if (!_db)
            {
                GetLogger()("RCO: no database connection.");
                return false;
            }

            GetLogger()("RCO query: {}", _scorcoSql);

            // FETCHMODE means "keep reading the result set the last call opened", so the statement
            // is only prepared for the first object.
            if (std::strcmp(param, "FETCHMODE") != 0)
            {
                SafeFinalize(&_stmt);

                int rc = sqlite3_prepare_v2(_db, _scorcoSql.c_str(), -1, &_stmt, nullptr);
                if (rc != SQLITE_OK)
                {
                    GetLogger()("RCO: cannot prepare '{}': {}", _scorcoSql, sqlite3_errmsg(_db));
                    SafeFinalize(&_stmt);
                    return false;
                }

                _firstFetch = false;
            }

            if (!Fetch(nullptr) || !_stmt || sqlite3_column_count(_stmt) == 0)
            {
                GetLogger()("RCO: empty result set.");
                return false;
            }

            const void* blob = sqlite3_column_blob(_stmt, 0);
            if (!blob)
            {
                GetLogger()("RCO: column is null.");
                return false;
            }

            int size = sqlite3_column_bytes(_stmt, 0);

            // The sink allocates; the loader owns and frees. This is where NWNX4 reached for the
            // engine's heap manager, and the whole reason it needed one.
            uint8_t* buffer = result.Allocate(static_cast<size_t>(size));
            if (!buffer)
            {
                GetLogger()("RCO: could not allocate {} bytes for the result.", size);
                return false;
            }

            std::memcpy(buffer, blob, static_cast<size_t>(size));
            return true;
        }

    private:
        bool Connect()
        {
            int rc = sqlite3_open(_dbFile.c_str(), &_db);
            if (rc != SQLITE_OK)
            {
                GetLogger()("could not open database: {}", _db ? sqlite3_errmsg(_db) : "out of memory");
                sqlite3_close(_db);
                _db = nullptr;
                return false;
            }

            sqlite3_extended_result_codes(_db, true);

            // Left unset, SQLite's own defaults apply - journal_mode=delete and synchronous=full,
            // which is the most durable combination it offers. Nothing here quietly trades
            // durability for speed; an admin has to ask for that.
            if (_config.journal_mode)
            {
                RunPragma("journal_mode", *_config.journal_mode);
            }

            if (_config.synchronous)
            {
                RunPragma("synchronous", *_config.synchronous);
            }

            if (_wrapTransaction)
            {
                RunSimple("BEGIN");
            }

            return true;
        }

        void Disconnect()
        {
            SafeFinalize(&_stmt);

            if (_db && _wrapTransaction)
            {
                RunSimple("COMMIT");
            }

            if (_db)
            {
                sqlite3_close(_db);
                _db = nullptr;
            }
        }

        /// A pragma whose value cannot be bound as a parameter, so it is spliced in. Only values
        /// from the config reach this, and anything SQLite does not recognise is simply ignored.
        void RunPragma(std::string_view name, const std::string& value)
        {
            char* sql = sqlite3_mprintf("PRAGMA %w = %Q", std::string(name).c_str(), value.c_str());
            if (!sql)
            {
                return;
            }

            GetLogger()("{}", sql);
            RunSimple(sql);
            sqlite3_free(sql);
        }

        /// Runs a statement that returns nothing worth reading, logging rather than reporting.
        void RunSimple(const char* sql)
        {
            char* error = nullptr;
            if (sqlite3_exec(_db, sql, nullptr, nullptr, &error) != SQLITE_OK)
            {
                GetLogger()("'{}' failed: {}", sql, error ? error : "unknown error");
                sqlite3_free(error);
            }
        }

        static void SafeFinalize(sqlite3_stmt** stmt)
        {
            if (*stmt)
            {
                sqlite3_finalize(*stmt);
                *stmt = nullptr;
            }
        }

        XpSqliteConfig _config;
        std::string _dbFile;
        bool _wrapTransaction = false;

        sqlite3* _db = nullptr;
        sqlite3_stmt* _stmt = nullptr;
        bool _firstFetch = false;
    };
}

NWN2_EXPORT_PLUGIN(XpSqlite)
