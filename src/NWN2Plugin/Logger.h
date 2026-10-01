#pragma once
#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <iostream>
#include <fstream>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <format>
#include <source_location>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <mutex>

#include "Data.h"

/// <summary>
/// Thread-safe formatted logger that can fan out to a log file, stdout/stderr, and
/// <c>OutputDebugString</c> simultaneously.
/// </summary>
/// <remarks>
/// A plugin's log is how an administrator watches what it is doing, so it has to stay readable
/// over a server's lifetime rather than growing without bound. The file is rotated once it passes
/// <see cref="SetMaxFileSize"/>, and messages below <see cref="SetMinimumLevel"/> are dropped.
/// Both have defaults, so a plugin that never calls either is still size-bounded.
///
/// This is the plugin SDK's copy. <c>NWN2Shared/Logger.h</c> is the loader's own and is
/// deliberately left alone - the loader's log path and verbosity come from nwn2mod.config.
/// </remarks>
class Logger
{
public:
    /// <summary>The severity a log line was written at, most severe first.</summary>
    /// <remarks>
    /// The order matters: a message is written when it is at or above the configured minimum, so
    /// setting <c>Info</c> also writes <c>Warning</c> and <c>Error</c>. This is how NWNX4's
    /// <c>log_level</c> behaved.
    /// </remarks>
    enum class Level {
        /// Not a severity a message can be logged at - only a minimum, meaning "write nothing".
        None = -1,
        Error = 0,
        Warning = 1,
        Info = 2,
        Debug = 3,
        Trace = 4
    };

    /// <summary>Rotate the log once it passes this size, unless changed.</summary>
    static constexpr uintmax_t kDefaultMaxFileSize = 10ull * 1024 * 1024;

    /// <summary>How many rotated files to keep, as <c>.1</c> through <c>.3</c>.</summary>
    static constexpr int kDefaultBackupCount = 3;

    /// <summary>Constructs a logger with the given output targets.</summary>
    /// <param name="filePath">An optional log file to append to.</param>
    /// <param name="stdOut">Whether to also write to stdout (or stderr for <see cref="Level::Error"/>).</param>
    /// <param name="debugOutput">Whether to also write via <c>OutputDebugStringW</c>.</param>
    Logger(
        std::optional<std::filesystem::path> filePath = std::nullopt,
        bool stdOut = false,
        bool debugOutput = false)
        : _DebugOutput(debugOutput)
        , _StdOutput(stdOut)
    {
        if (filePath)
        {
            _Path = *filePath;
            _File.open(_Path, std::ios::out | std::ios::app);

            // Appending to an existing log, so start from its current size or the first rotation
            // would not happen until the process had written a whole limit's worth by itself.
            std::error_code ec;
            _Written = std::filesystem::file_size(_Path, ec);
            if (ec)
            {
                _Written = 0;
            }
        }
    }

    /// <summary>Closes the log file, if one is open.</summary>
    ~Logger()
    {
        if (_File.is_open())
        {
            _File.close();
        }
    }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /// <summary>Drops any message less severe than <paramref name="level"/>.</summary>
    /// <remarks>
    /// Call this after reading the plugin's config. The logger generally has to exist before the
    /// config can be read - reading it may itself want to log - so verbosity is applied afterwards
    /// rather than passed to the constructor.
    /// </remarks>
    void SetMinimumLevel(Level level)
    {
        std::lock_guard<std::mutex> lock(_Lock);
        _Minimum = level;
    }

    /// <summary>Rotates the log once it passes <paramref name="bytes"/>.</summary>
    /// <param name="bytes">The size to rotate at, or 0 to never rotate.</param>
    /// <param name="backups">How many rotated files to keep.</param>
    void SetMaxFileSize(uintmax_t bytes, int backups = kDefaultBackupCount)
    {
        std::lock_guard<std::mutex> lock(_Lock);
        _MaxFileSize = bytes;
        _Backups = backups < 0 ? 0 : backups;
    }

    /// <summary>Parses a size with an optional <c>K</c>, <c>M</c> or <c>G</c> suffix, as NWNX4's
    /// <c>log_max_file_size</c> accepted.</summary>
    /// <param name="text">For example <c>"10M"</c>, <c>"512K"</c> or <c>"1048576"</c>.</param>
    /// <returns>The size in bytes, or empty if it could not be parsed. <c>"0"</c> means no limit.</returns>
    static std::optional<uintmax_t> ParseSize(std::string_view text)
    {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        {
            text.remove_prefix(1);
        }
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        {
            text.remove_suffix(1);
        }

        if (text.empty())
        {
            return std::nullopt;
        }

        uintmax_t multiplier = 1;
        char suffix = static_cast<char>(std::toupper(static_cast<unsigned char>(text.back())));
        if (suffix == 'B' && text.size() > 1)
        {
            text.remove_suffix(1);
            suffix = static_cast<char>(std::toupper(static_cast<unsigned char>(text.back())));
        }

        if (suffix == 'K' || suffix == 'M' || suffix == 'G')
        {
            multiplier = suffix == 'K' ? 1024ull
                       : suffix == 'M' ? 1024ull * 1024
                                       : 1024ull * 1024 * 1024;
            text.remove_suffix(1);
        }

        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        {
            text.remove_suffix(1);
        }

        if (text.empty())
        {
            return std::nullopt;
        }

        uintmax_t value = 0;
        for (char c : text)
        {
            if (c < '0' || c > '9')
            {
                return std::nullopt;
            }
            value = value * 10 + static_cast<uintmax_t>(c - '0');
        }

        return value * multiplier;
    }

    /// <summary>Turns a config string such as <c>"info"</c> into a <see cref="Level"/>.</summary>
    /// <returns>The level, or empty if the name is not recognised.</returns>
    static std::optional<Level> ParseLevel(std::string_view name)
    {
        if (name == "none")    { return Level::None; }
        if (name == "error")   { return Level::Error; }
        if (name == "warning") { return Level::Warning; }
        if (name == "info")    { return Level::Info; }
        if (name == "debug")   { return Level::Debug; }
        if (name == "trace")   { return Level::Trace; }
        return std::nullopt;
    }

    /// <summary>Logs a formatted message at the given level. Equivalent to <see cref="log(Level, std::format_string&lt;Args...&gt;, Args&amp;&amp;...)"/>.</summary>
    template <typename... Args>
    void operator()(Level level, std::format_string<Args...> fmt, Args&&... args)
    {
        log(level, fmt, std::forward<Args>(args)...);
    }

    /// <summary>Logs a formatted message at <see cref="Level::Info"/>. Equivalent to <see cref="log(std::format_string&lt;Args...&gt;, Args&amp;&amp;...)"/>.</summary>
    template <typename... Args>
    void operator()(std::format_string<Args...> fmt, Args&&... args)
    {
        log(Level::Info, fmt, std::forward<Args>(args)...);
    }

    /// <summary>Logs a formatted message at <see cref="Level::Info"/>.</summary>
    /// <param name="fmt">A <c>std::format</c> format string.</param>
    /// <param name="args">The format arguments.</param>
    template <typename... Args>
    void log(std::format_string<Args...> fmt, Args&&... args)
    {
        log(Level::Info, fmt, std::forward<Args>(args)...);
    }

    /// <summary>Logs a formatted message to every configured output target.</summary>
    /// <param name="level">The severity to log at.</param>
    /// <param name="fmt">A <c>std::format</c> format string.</param>
    /// <param name="args">The format arguments.</param>
    template <typename... Args>
    void log(Level level,
        std::format_string<Args...> fmt,
        Args&&... args)
    {
        // Checked before formatting, so a suppressed message costs only the call.
        if (level > _Minimum)
        {
            return;
        }

        std::string msg = std::format(fmt, std::forward<Args>(args)...);

        auto now = std::chrono::system_clock::now();

        std::string logLine = std::format("[{:%Y-%m-%d %H:%M:%S}] [{}] {}\n",
            now,
            LevelToString(level),
            msg);

        std::lock_guard<std::mutex> lock(_Lock);

        if (_StdOutput)
        {
            if (level == Level::Error)
            {
                std::clog << logLine;
            }
            else
            {
                std::cout << logLine;
            }
        }

        if (_File.is_open())
        {
            RotateIfNeeded(logLine.size());

            _File << logLine;
            _File.flush(); // Ensure visibility on crash
            _Written += logLine.size();
        }

        if (_DebugOutput)
        {
            auto debugLine = ToWString(logLine);
            OutputDebugStringW(debugLine.c_str());
        }
    }

    /// <summary>Formats a source location as <c>"file:line"</c>, for use in log messages.</summary>
    /// <param name="loc">The location to format; defaults to the caller's own location.</param>
    static std::string Source(const std::source_location loc = std::source_location::current())
    {
        std::filesystem::path file_path(loc.file_name());
        std::string filename = file_path.filename().string();

        return std::format("{}:{}", filename, loc.line());
    }

private:
    bool _DebugOutput;
    bool _StdOutput;
    std::ofstream _File;
    std::mutex _Lock;

    std::filesystem::path _Path;
    uintmax_t _Written = 0;
    uintmax_t _MaxFileSize = kDefaultMaxFileSize;
    int _Backups = kDefaultBackupCount;
    Level _Minimum = Level::Info;

    /// <summary>
    /// Renames the current log aside and starts a fresh one if the next line would take it past
    /// the limit. The caller holds the lock.
    /// </summary>
    /// <remarks>
    /// Best-effort: if a rename fails, most likely because something else has the file open,
    /// logging carries on rather than being lost. An oversized log is a smaller problem than a
    /// silent one.
    /// </remarks>
    void RotateIfNeeded(size_t incoming)
    {
        if (_MaxFileSize == 0 || _Path.empty() || _Written + incoming <= _MaxFileSize)
        {
            return;
        }

        _File.close();

        std::error_code ec;

        if (_Backups > 0)
        {
            // Drop the oldest, then shuffle each backup down a slot: .2 -> .3, .1 -> .2
            std::filesystem::remove(Backup(_Backups), ec);

            for (int i = _Backups - 1; i >= 1; --i)
            {
                std::filesystem::rename(Backup(i), Backup(i + 1), ec);
            }

            std::filesystem::rename(_Path, Backup(1), ec);
        }
        else
        {
            std::filesystem::remove(_Path, ec);
        }

        // Truncate rather than append: if the rename above failed this is still the file being
        // written to, and letting it grow is the thing rotation exists to prevent.
        _File.open(_Path, std::ios::out | std::ios::trunc);
        _Written = 0;
    }

    std::filesystem::path Backup(int index) const
    {
        std::filesystem::path path = _Path;
        path += std::format(".{}", index);
        return path;
    }

    static constexpr const char* LevelToString(Level level)
    {
        switch (level)
        {
        case Level::None: return "-";
        case Level::Error: return "E";
        case Level::Warning: return "W";
        case Level::Info: return "I";
        case Level::Debug: return "D";
        case Level::Trace: return "T";
        }

        return "UNKNOWN";
    }
};
