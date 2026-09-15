#pragma once
#include "Yaml.h"
#include <expected>
#include <filesystem>
#include <vector>

/// <summary>The <c>nwn2mod.config</c> YAML schema; see the README for the full field reference.</summary>
struct Config
{
    /// <summary>The full path to <c>NWN2Server64.exe</c>.</summary>
    std::string server_exe;

    /// <summary>The arguments passed to <see cref="server_exe"/> when executed.</summary>
    std::string server_args;

    /// <summary>The working directory for <see cref="server_exe"/>. Defaults to its parent path.</summary>
    std::optional<std::string> server_directory;

    /// <summary>The full path to <c>NWN2ModLoader.dll</c>. Defaults to the same parent path as the launcher executable.</summary>
    std::optional<std::string> loader_dll;

    /// <summary>The full path to a log file written to by <c>NWN2ModLoader.dll</c>. No log file is created if omitted.</summary>
    std::optional<std::string> loader_log;

    /// <summary>The full path to a log file written to by the launcher. No log file is created if omitted.</summary>
    std::optional<std::string> server_log;

    /// <summary>If <see langword="true"/>, logs are additionally emitted via <c>OutputDebugString</c>. Defaults to <see langword="false"/>.</summary>
    std::optional<bool> debug_log;

    /// <summary>If <see langword="true"/>, logs are additionally emitted to stdout/stderr. Defaults to <see langword="true"/>.</summary>
    std::optional<bool> std_log;

    /// <summary>Full paths to plugin DLLs to load, in order. No plugins are loaded if omitted.</summary>
    std::optional<std::vector<std::string>> plugins;

    /// <summary>Parses a config from a YAML string.</summary>
    /// <param name="src">The YAML text.</param>
    /// <returns>The parsed config, or an error message on failure.</returns>
    static std::expected<Config, std::string> FromString(std::string_view src);

    /// <summary>Parses a config from a YAML file.</summary>
    /// <param name="path">The full path to the config file.</param>
    /// <returns>The parsed config, or an error message on failure.</returns>
    static std::expected<Config, std::string> FromFile(std::filesystem::path path);
};

/// <summary>Lets <c>YAML::Node</c> (de)serialize <see cref="Config"/> - see <c>Yaml.h</c> for the general-purpose parsing entry points.</summary>
template <>
struct YAML::convert<Config>
{
    /// <summary>Encodes a <see cref="Config"/> to YAML.</summary>
    static Node encode(const Config& c)
    {
        Node node;
        node["server_exe"] = c.server_exe;
        node["server_args"] = c.server_args;
        node["server_directory"] = c.server_directory;
        node["loader_dll"] = c.loader_dll;
        node["loader_log"] = c.loader_log;
        node["server_log"] = c.server_log;
        node["debug_log"] = c.debug_log;
        node["std_log"] = c.std_log;
        node["plugins"] = c.plugins;
        return node;
    }

    /// <summary>Decodes a <see cref="Config"/> from YAML.</summary>
    static bool decode(const Node& node, Config& c)
    {
        if (!node.IsMap())
        {
            return false;
        }

        c.server_exe = node["server_exe"].as<std::string>();
        c.server_args = node["server_args"].as<std::string>();

        // The plain (no-fallback) Node::as<T>() throws immediately for a key that's absent
        // entirely (an "invalid" node, distinct from IsNull()) without ever calling our
        // convert<optional<T>>::decode below - so a fallback must be passed here to cover that
        // case. decode() itself still handles the "present but explicitly null" case.
        c.server_directory = node["server_directory"].as<std::optional<std::string>>(std::nullopt);
        c.loader_dll = node["loader_dll"].as<std::optional<std::string>>(std::nullopt);
        c.loader_log = node["loader_log"].as<std::optional<std::string>>(std::nullopt);
        c.server_log = node["server_log"].as<std::optional<std::string>>(std::nullopt);
        c.debug_log = node["debug_log"].as<std::optional<bool>>(std::nullopt);
        c.std_log = node["std_log"].as<std::optional<bool>>(std::nullopt);
        c.plugins = node["plugins"].as<std::optional<std::vector<std::string>>>(std::nullopt);
        return true;
    }
};

