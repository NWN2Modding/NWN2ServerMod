// xp_mysql, ported from NWNX4 to the NWN2ServerMod C ABI.
//
// Original: Copyright (C) 2007 Ingmar Stieger (Papillon), GPL v2 or later. The script-facing
// contract is unchanged; the plugin interface, the SCORCO write path and several bugs are not.
//
// SQL from NWScript, via nwnx_sql.nss - the same surface xp_sqlite provides, since both derive
// from DbPluginBase. See that file for the function list.
//
// What changed from the NWNX4 version:
//   - DBPlugin subclass            -> nwn2ports::DbPluginBase, which owns the dispatch
//   - xp_mysql.ini                 -> xp_mysql.yml, matching nwn2mod.config
//   - NWN2_HeapMgr for RCO buffers -> the loader's BinaryDataResult sink
//   - sprintf of the SCO statement -> a prepared statement with the object bound as a parameter
//
// That last one is the significant change, and it is worth explaining.
//
// NWNX4 built the SCO statement like this:
//
//     len     = mysql_real_escape_string(&mysql, Data + 1, pData, Length);
//     Data[0] = Data[len + 1] = 39;                      // wrap it in single quotes
//     sprintf(pSQL, scorcoSQL.c_str(), Data);            // scorcoSQL is the FORMAT STRING
//
// Three problems in four lines:
//
//   1. The script's statement is used as a printf format string. Any '%' in it that is not the
//      intended %s is interpreted - %n writes to memory - and a stray '%' in a column name or a
//      LIKE pattern corrupts the query.
//   2. sprintf is unbounded while the buffer is sized MAXSQL (1024) plus the escaped object, so a
//      long SETSCORCOSQL statement overflows the heap.
//   3. The object is inserted as a quoted string literal escaped against the connection charset.
//      On latin1 every byte is a valid character so binary data survives; on utf8mb4 - MySQL 8's
//      default, and NWNX4's own ini default - arbitrary bytes are not valid UTF-8 and the server
//      rejects or mangles them. This is what ties a module using SCORCO to an old MySQL.
//
// This port binds the object as a MYSQL_TYPE_LONG_BLOB parameter instead: no escaping, no format
// string, no charset involvement, and the length travels with the data. The script's '%s' is
// rewritten to '?' exactly as the SQLite port does, so no module script has to change.
#include "../common/DbPluginBase.h"
#include "../common/LogConfig.h"
#include "../common/Gff.h"

#include <Data.h>
#include <Yaml.h>

#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <mysql.h>
// CR_SERVER_GONE_ERROR and CR_SERVER_LOST, the two "the connection died" codes worth retrying.
// They live here rather than in mysql.h.
#include <errmsg.h>

#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace
{
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

    /// The xp_mysql.yml schema. Every field is optional; the defaults match NWNX4's ini.
    struct XpMySqlConfig
    {
        std::optional<std::string> plugin_class;
        std::optional<std::string> server;
        std::optional<int> port;
        std::optional<std::string> user;
        std::optional<std::string> password;
        std::optional<std::string> schema;
        std::optional<std::string> charset;

        /// Lets one EXEC run several statements separated by semicolons. Off by default: it turns
        /// a missed GET ESCAPE STRING from a data leak into arbitrary statement execution.
        std::optional<bool> multi_statements;

        /// Log verbosity: none, error, warning, info, debug or trace. NWNX4's key and values.
        std::optional<std::string> log_level;

        /// Rotate the log past this size. Accepts a K, M or G suffix; 0 disables rotation.
        std::optional<std::string> log_max_file_size;
    };
}

/// <summary>Lets <c>YAML::Node</c> decode <see cref="XpMySqlConfig"/>.</summary>
template <>
struct YAML::convert<XpMySqlConfig>
{
    static bool decode(const Node& node, XpMySqlConfig& c)
    {
        if (!node.IsMap())
        {
            return false;
        }

        c.plugin_class     = node["class"].as<std::optional<std::string>>(std::nullopt);
        c.server           = node["server"].as<std::optional<std::string>>(std::nullopt);
        c.port             = node["port"].as<std::optional<int>>(std::nullopt);
        c.user             = node["user"].as<std::optional<std::string>>(std::nullopt);
        c.password         = node["password"].as<std::optional<std::string>>(std::nullopt);
        c.schema           = node["schema"].as<std::optional<std::string>>(std::nullopt);
        c.charset          = node["charset"].as<std::optional<std::string>>(std::nullopt);
        c.multi_statements = node["multi_statements"].as<std::optional<bool>>(std::nullopt);
        c.log_level         = node["log_level"].as<std::optional<std::string>>(std::nullopt);
        c.log_max_file_size = node["log_max_file_size"].as<std::optional<std::string>>(std::nullopt);
        return true;
    }
};

namespace
{
    XpMySqlConfig LoadConfig()
    {
        auto path = OwnPath().replace_extension(L".yml");
        if (!std::filesystem::exists(path))
        {
            GetLogger()("no config at {}; using defaults.", path.string());
            return {};
        }

        auto config = Yaml::FromFile<XpMySqlConfig>(path);
        if (!config)
        {
            GetLogger()("could not read {}: {}. Using defaults.", path.string(), config.error());
            return {};
        }

        return *config;
    }

    /// Read once; the base class needs the function class before the constructor body runs.
    const XpMySqlConfig& Config()
    {
        static XpMySqlConfig config = LoadConfig();
        return config;
    }

    class XpMySql : public nwn2ports::DbPluginBase
    {
    public:
        XpMySql()
            : DbPluginBase(Config().plugin_class.value_or("SQL"), GetLogger())
            , _config(Config())
        {
            _server   = _config.server.value_or("localhost");
            _port     = _config.port.value_or(0);
            _user     = _config.user.value_or("");
            _password = _config.password.value_or("");
            _schema   = _config.schema.value_or("nwn2");
nwn2ports::ApplyLogConfig(GetLogger(), _config);

            _charset  = _config.charset.value_or("");

            GetLogger()("xp_mysql loaded, built against {} {}.",
                        mysql_get_client_info(), MARIADB_PACKAGE_VERSION);
            GetLogger()("server '{}' port {} schema '{}' charset '{}'",
                        _server, _port, _schema, _charset.empty() ? "(server default)" : _charset);

            // Never log the password itself, only whether there is one.
            GetLogger()("user '{}' password {}", _user, _password.empty() ? "none" : "set");

            if (!Connect())
            {
                GetLogger()("initial connection failed; EXEC will retry on first use.");
            }
        }

        ~XpMySql() override { Disconnect(); }

    protected:
        const char* GetSubclass() const override { return "MySQL"; }
        const char* GetVersion() const override { return "1.1.0"; }
        const char* GetDescription() const override
        {
            return "This plugin provides database storage, using MySQL or MariaDB as the server.";
        }

        bool Execute(const char* query) override
        {
            if (!_connection && !Reconnect())
            {
                GetLogger()("Execute: not connected.");
                return false;
            }

            // Consume anything a previous multi-statement query left behind, or the connection
            // goes out of sync with the server.
            DrainResults();

            GetLogger()("Executing: {}", query);

            if (mysql_query(_connection, query) != 0)
            {
                unsigned int error = mysql_errno(_connection);
                GetLogger()("SQL error: {} ({})", mysql_error(_connection), error);

                // Throw away the last result set if a SELECT failed, so a later FETCH does not
                // walk rows belonging to a different query.
                if (_strnicmp(query, "SELECT", 6) == 0)
                {
                    ClearResult();
                }

                // The server dropping the connection is the one error worth retrying: a long-idle
                // PW hits it routinely after the server's wait_timeout.
                if ((error == CR_SERVER_GONE_ERROR || error == CR_SERVER_LOST) && Reconnect())
                {
                    if (mysql_query(_connection, query) != 0)
                    {
                        GetLogger()("SQL error after reconnect: {} ({})",
                                    mysql_error(_connection), mysql_errno(_connection));
                        return false;
                    }
                }
                else
                {
                    return false;
                }
            }

            return StoreResult();
        }

        int Fetch(const char* param) override
        {
            if (!_connection)
            {
                GetLogger()("Fetch: not connected.");
                return 0;
            }

            // "NEXT" moves to the next result set of a multi-statement query rather than the next
            // row of this one.
            if (param && std::strcmp(param, "NEXT") == 0)
            {
                ClearResult();
                _result = AdvanceToNextResultSet();
                _fieldCount = _result ? mysql_num_fields(_result) : 0;
            }

            if (_result)
            {
                _row = mysql_fetch_row(_result);
                if (_row)
                {
                    _lengths = mysql_fetch_lengths(_result);
                    return 1;
                }
            }

            _row = nullptr;
            _lengths = nullptr;
            return 0;
        }

        int GetData(int column, std::string& out) override
        {
            out.clear();

            if (!_row)
            {
                GetLogger()("GetData: no current row.");
                return -1;
            }

            if (column < 0 || static_cast<unsigned int>(column) >= _fieldCount || !_row[column])
            {
                return -1;
            }

            // The row's own lengths, not strlen: a column is allowed to contain embedded nulls,
            // and std::string carries them where the original fixed buffer could not.
            out.assign(_row[column], _lengths ? _lengths[column] : std::strlen(_row[column]));
            return 0;
        }

        int GetAffectedRows() override
        {
            // mysql_affected_rows returns (my_ulonglong)-1 on error, which becomes -1 here.
            return _connection ? static_cast<int>(mysql_affected_rows(_connection)) : -1;
        }

        void GetEscapeString(const char* str, std::string& out) override
        {
            out.clear();

            if (!_connection || !str || !*str)
            {
                return;
            }

            size_t length = std::strlen(str);

            // Worst case every character is escaped, plus a terminator.
            std::vector<char> escaped(length * 2 + 1);
            unsigned long written = mysql_real_escape_string(
                _connection, escaped.data(), str, static_cast<unsigned long>(length));

            if (written == static_cast<unsigned long>(-1))
            {
                GetLogger()("GET ESCAPE STRING failed: {}", mysql_error(_connection));
                return;
            }

            out.assign(escaped.data(), written);
        }

        int GetErrno() override { return _connection ? static_cast<int>(mysql_errno(_connection)) : 0; }

        const char* GetErrorMessage() override
        {
            return _connection ? mysql_error(_connection) : nullptr;
        }

        int GetLastInsertID() override
        {
            // NWNX's script API is 32-bit, so an id past 2^31 cannot be represented.
            return _connection ? static_cast<int>(mysql_insert_id(_connection)) : 0;
        }

        /// Stores a serialized object using the statement SETSCORCOSQL parked, with the object
        /// bound as a parameter rather than escaped into the statement text.
        bool WriteScorcoData(const uint8_t* data, size_t size) override
        {
            if (!_connection && !Reconnect())
            {
                GetLogger()("SCO: not connected.");
                return false;
            }

            GetLogger()("SCO query: {}", _scorcoSql);

            // Scripts write these with %s where the object goes, which was printf's placeholder in
            // the original. Rewrite it to a real parameter marker.
            std::string sql = _scorcoSql;
            auto match = sql.find("%s");
            if (match == std::string::npos)
            {
                GetLogger()("SCO: the statement has no %s for the object - nothing to bind.");
                return false;
            }
            sql.replace(match, 2, "?");

            if (sql.find("%s") != std::string::npos)
            {
                GetLogger()("SCO: the statement has more than one %s; only the first is the object.");
                return false;
            }

            // A prepared statement is a separate handle, so it does not disturb the result set an
            // earlier EXEC may have left open.
            MYSQL_STMT* stmt = mysql_stmt_init(_connection);
            if (!stmt)
            {
                GetLogger()("SCO: out of memory preparing the statement.");
                return false;
            }

            bool ok = false;
            do
            {
                if (mysql_stmt_prepare(stmt, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0)
                {
                    GetLogger()("SCO: cannot prepare '{}': {}", sql, mysql_stmt_error(stmt));
                    break;
                }

                if (mysql_stmt_param_count(stmt) != 1)
                {
                    GetLogger()("SCO: '{}' takes {} parameters, expected exactly 1.",
                                sql, mysql_stmt_param_count(stmt));
                    break;
                }

                unsigned long length = static_cast<unsigned long>(size);

                MYSQL_BIND bind{};
                bind.buffer_type   = MYSQL_TYPE_LONG_BLOB;
                bind.buffer        = const_cast<uint8_t*>(data);
                bind.buffer_length = length;
                bind.length        = &length;

                if (mysql_stmt_bind_param(stmt, &bind) != 0)
                {
                    GetLogger()("SCO: cannot bind the object: {}", mysql_stmt_error(stmt));
                    break;
                }

                if (mysql_stmt_execute(stmt) != 0)
                {
                    GetLogger()("SCO: store failed: {} ({})",
                                mysql_stmt_error(stmt), mysql_stmt_errno(stmt));
                    break;
                }

                // A BLOB column holds 65535 bytes. A played character with a full inventory goes
                // past that, and outside strict sql_mode the server truncates it and carries on -
                // the object simply fails to deserialize later, with nothing logged anywhere.
                // That is silent data loss, so say so before it happens.
                if (size > 65535)
                {
                    GetLogger()("SCO: WARNING - this object is {} bytes. A BLOB column holds only "
                                "65535 and will truncate it silently unless strict sql_mode is on. "
                                "Use MEDIUMBLOB or LONGBLOB.", size);
                }

                // Truncation raises a warning rather than an error, which is exactly the case that
                // goes unnoticed. Surface the count; SHOW WARNINGS is deliberately not run here
                // because it would clobber a result set an earlier EXEC may still be reading.
                unsigned int warnings = mysql_warning_count(_connection);
                if (warnings > 0)
                {
                    GetLogger()("SCO: the server reported {} warning(s) storing {} bytes. Run "
                                "SHOW WARNINGS against this connection's last statement - a "
                                "truncated object will not load back.", warnings, size);
                }

                GetLogger()("SCO: stored {} bytes.", size);
                ok = true;
            } while (false);

            mysql_stmt_close(stmt);
            return ok;
        }

        /// Reads a serialized object. Reading was always binary-safe - the row carries its own
        /// lengths - so this keeps the original's plain query path.
        bool ReadScorcoData(const char* param, nwn2::BinaryDataResult& result) override
        {
            if (!_connection && !Reconnect())
            {
                GetLogger()("RCO: not connected.");
                return false;
            }

            GetLogger()("RCO query: {}", _scorcoSql);

            // FETCHMODE keeps reading the result set the last call opened, so only the first
            // object runs a query.
            bool fetchMode = param && std::strcmp(param, "FETCHMODE") == 0;

            if (!fetchMode)
            {
                DrainResults();
                ClearResult();

                if (mysql_query(_connection, _scorcoSql.c_str()) != 0)
                {
                    GetLogger()("RCO: query failed: {}", mysql_error(_connection));
                    return false;
                }

                _result = mysql_store_result(_connection);
                if (!_result)
                {
                    GetLogger()("RCO: no result set: {}", mysql_error(_connection));
                    return false;
                }

                _fieldCount = mysql_num_fields(_result);
            }

            if (!_result)
            {
                GetLogger()("RCO: no open result set to read from.");
                return false;
            }

            // The original freed this result even in FETCHMODE, leaving the member dangling for
            // the next Fetch or GetData. The result stays open here and is cleared by whatever
            // runs next.
            MYSQL_ROW row = mysql_fetch_row(_result);
            if (!row || _fieldCount == 0)
            {
                GetLogger()("RCO: empty result set.");
                return false;
            }

            unsigned long* lengths = mysql_fetch_lengths(_result);
            if (!row[0] || !lengths)
            {
                GetLogger()("RCO: column is null.");
                return false;
            }

            // A damaged object is worth distinguishing from a missing one, because in game they
            // look identical and only one of them is a database problem.
            //
            // The check is structural rather than a magic-bytes comparison. Sundren lost chest
            // contents to a CSV export that cut every row at its first NUL, leaving 9 bytes -
            // "UTI V3.28" - which carries a perfectly good file type and version marker and would
            // sail through a check on the first eight bytes.
            if (auto reason = nwn2ports::GffRejectReason(
                    reinterpret_cast<const uint8_t*>(row[0]), lengths[0]); !reason.empty())
            {
                GetLogger()("RCO: WARNING - the stored object is damaged, not missing: {}. "
                            "Look for a truncating export or import, or a column too small to "
                            "hold it.", reason);
            }

            uint8_t* buffer = result.Allocate(lengths[0]);
            if (!buffer)
            {
                GetLogger()("RCO: could not allocate {} bytes for the result.", lengths[0]);
                return false;
            }

            std::memcpy(buffer, row[0], lengths[0]);
            GetLogger()("RCO: read {} bytes.", lengths[0]);
            return true;
        }

    private:
        bool Connect()
        {
            if (!mysql_init(&_mysql))
            {
                GetLogger()("mysql_init failed.");
                _connection = nullptr;
                return false;
            }

            unsigned long flags = 0;
            if (_config.multi_statements.value_or(false))
            {
                // Opt-in: with this on, one injected semicolon in a concatenated query becomes a
                // second statement the server will run.
                flags |= CLIENT_MULTI_STATEMENTS;
                GetLogger()("multi-statement queries are enabled.");
            }

            _connection = mysql_real_connect(&_mysql, _server.c_str(), _user.c_str(),
                                             _password.c_str(), _schema.c_str(),
                                             _port, nullptr, flags);
            if (!_connection)
            {
                GetLogger()("connection failed: {}", mysql_error(&_mysql));
                mysql_close(&_mysql);
                _connection = nullptr;
                return false;
            }

            if (!_charset.empty() && mysql_set_character_set(_connection, _charset.c_str()) != 0)
            {
                GetLogger()("could not set character set '{}': {}", _charset, mysql_error(_connection));
            }

            GetLogger()("connected to {}.", mysql_get_server_info(_connection));
            return true;
        }

        void Disconnect()
        {
            ClearResult();

            if (_connection)
            {
                mysql_close(_connection);
            }

            // The original left this pointing at a closed handle, so the "not connected" guards
            // downstream let a dead connection through.
            _connection = nullptr;
        }

        bool Reconnect()
        {
            GetLogger()("reconnecting...");
            Disconnect();
            return Connect();
        }

        void ClearResult()
        {
            if (_result)
            {
                mysql_free_result(_result);
                _result = nullptr;
            }

            _row = nullptr;
            _lengths = nullptr;
            _fieldCount = 0;
        }

        /// Consumes any result sets still pending on the connection.
        void DrainResults()
        {
            while (_connection && mysql_more_results(_connection))
            {
                mysql_next_result(_connection);
                if (MYSQL_RES* leftover = mysql_store_result(_connection))
                {
                    mysql_free_result(leftover);
                }
            }
        }

        /// Takes the result set of the statement just run, if it produced one.
        bool StoreResult()
        {
            MYSQL_RES* fresh = mysql_store_result(_connection);

            if (fresh)
            {
                ClearResult();
                _result = fresh;
                _fieldCount = mysql_num_fields(_result);
                return true;
            }

            if (mysql_field_count(_connection) != 0)
            {
                // A SELECT that returned nothing, as opposed to a statement with no result set.
                ClearResult();
                return mysql_errno(_connection) == 0;
            }

            // Not a SELECT. Advance to the first result set that has rows, so a FETCH after a
            // multi-statement query finds data even when the first statements returned none.
            if (MYSQL_RES* next = AdvanceToNextResultSet())
            {
                ClearResult();
                _result = next;
                _fieldCount = mysql_num_fields(_result);
            }

            if (mysql_errno(_connection) != 0)
            {
                GetLogger()("error reading the result: {}", mysql_error(_connection));
                return false;
            }

            return true;
        }

        /// The next result set that actually has rows, or null.
        MYSQL_RES* AdvanceToNextResultSet()
        {
            while (_connection && mysql_more_results(_connection))
            {
                if (mysql_next_result(_connection) != 0)
                {
                    return nullptr;
                }

                if (MYSQL_RES* candidate = mysql_store_result(_connection))
                {
                    return candidate;
                }

                if (mysql_field_count(_connection) != 0)
                {
                    // A SELECT with no rows. Stop here rather than skipping past it.
                    return nullptr;
                }
            }

            return nullptr;
        }

        XpMySqlConfig _config;
        std::string _server, _user, _password, _schema, _charset;
        int _port = 0;

        MYSQL _mysql{};
        MYSQL* _connection = nullptr;
        MYSQL_RES* _result = nullptr;
        MYSQL_ROW _row = nullptr;
        unsigned long* _lengths = nullptr;
        unsigned int _fieldCount = 0;
    };
}

NWN2_EXPORT_PLUGIN(XpMySql)
