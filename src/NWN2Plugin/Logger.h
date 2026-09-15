#pragma once
#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <iostream>
#include <fstream>
#include <filesystem>
#include <optional>
#include <string>
#include <format>
#include <source_location>
#include <chrono>
#include <mutex>

#include "Data.h"

/// <summary>
/// Thread-safe formatted logger that can fan out to a log file, stdout/stderr, and
/// <c>OutputDebugString</c> simultaneously.
/// </summary>
class Logger
{
public:
    /// <summary>The severity a log line was written at.</summary>
    enum class Level {
        Info,
        Warning,
        Error
    };

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
            _File.open(*filePath, std::ios::out | std::ios::app);
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
            _File << logLine;
            _File.flush(); // Ensure visibility on crash
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

    static constexpr const char* LevelToString(Level level)
    {
        switch (level)
        {
        case Level::Info: return "I";
        case Level::Warning: return "W";
        case Level::Error: return "E";
        }

        return "UNKNOWN";
    }
};
