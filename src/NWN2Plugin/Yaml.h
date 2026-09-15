#pragma once
#include <expected>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <yaml-cpp/yaml.h>

#pragma comment(lib, "yaml-cpp.lib")

namespace YAML {
    /// <summary>Lets <c>YAML::Node</c> (de)serialize <c>std::optional&lt;T&gt;</c>: missing/null decodes to empty, empty encodes as <c>null</c>.</summary>
    template <typename T>
    struct convert<std::optional<T>> {
        /// <summary>Encodes a <c>std::optional&lt;T&gt;</c> to YAML, as <c>null</c> when empty.</summary>
        static Node encode(const std::optional<T>& opt)
        {
            if (!opt.has_value())
            {
                return Node(YAML::NodeType::Null);
            }

            return Node(*opt);
        }

        /// <summary>Decodes a <c>std::optional&lt;T&gt;</c> from YAML, as empty when the node is missing or <c>null</c>.</summary>
        static bool decode(const Node& node, std::optional<T>& opt)
        {
            if (!node.IsDefined() || node.IsNull())
            {
                opt = std::nullopt;
            }
            else
            {
                opt = node.as<T>();
            }

            return true;
        }
    };
}

/// <summary>
/// A thin <c>std::expected</c>-returning wrapper around yaml-cpp, so any type with a
/// <c>YAML::convert&lt;T&gt;</c> specialization (see <see cref="Config"/> for an example) can be
/// parsed from YAML the same way <c>nwn2mod.config</c> itself is - this is the entry point a
/// plugin should use to load its own YAML config.
/// </summary>
namespace Yaml {
    /// <summary>Parses <typeparamref name="T"/> from a YAML string.</summary>
    /// <param name="src">The YAML text.</param>
    /// <returns>The parsed value, or an error message on failure.</returns>
    template <typename T>
    std::expected<T, std::string> FromString(std::string_view src)
    {
        try
        {
            return YAML::Load(std::string(src)).as<T>();
        }
        catch (YAML::Exception& e)
        {
            return std::unexpected(e.what());
        }
        catch (std::exception& e)
        {
            return std::unexpected(e.what());
        }
        catch (...)
        {
            return std::unexpected("unexpected exception");
        }
    }

    /// <summary>Parses <typeparamref name="T"/> from a YAML file.</summary>
    /// <param name="path">The full path to the file.</param>
    /// <returns>The parsed value, or an error message on failure.</returns>
    template <typename T>
    std::expected<T, std::string> FromFile(std::filesystem::path path)
    {
        try
        {
            std::ifstream file(path);
            return YAML::Load(file).as<T>();
        }
        catch (YAML::Exception& e)
        {
            return std::unexpected(e.what());
        }
        catch (std::exception& e)
        {
            return std::unexpected(e.what());
        }
        catch (...)
        {
            return std::unexpected("unexpected exception");
        }
    }
}
