// xp_objectattributes, ported from NWNX4 to the NWN2ServerMod C ABI.
//
// Original: Copyright (C) 2008-2011 Skywing, GPL v2 or later for NWNX4's use.
//
// Changes a creature's appearance at runtime - head, hair, tail, wing and facial hair variations,
// body/head/hair tints, and racial type. Scripts call it through nwnx_objectattributes.nss:
//
//     NWNXSetString("OBJECTATTRIBUTES", "SetHeadVariation", "", ObjectToInt(oCreature), "3");
//     NWNXSetString("OBJECTATTRIBUTES", "SetBodyTint", "", ObjectToInt(oCreature),
//                   "NWN2_TintSet[0xRRGGBBAA, 0xRRGGBBAA, 0xRRGGBBAA]");
//
// What changed from the NWNX4 version:
//   Plugin subclass                -> nwn2::PluginBase
//   NWN2Lib's reconstructed layout -> the engine's actual structure layout
//   a hardcoded CAppManager pointer -> CreatureLookup, which finds the engine's own accessor
//
// On the offsets: the original reached fields through Skywing's hand-reconstructed 32-bit struct,
// writing at `this + CrAppearance + CaWingVariation` and the like. None of those survive the move
// to x64, so every one here was re-derived against the engine's own layout. They are listed below
// with the field names they correspond to, which is what makes them checkable by anyone else.
//
// Each setter writes the creature's own copy and the stats copy, as the original did - the engine
// keeps both and they are read in different places.
#include <Plugin.hpp>
#include <Logger.h>
#include <Data.h>
#include <Yaml.h>

#include "../common/LogConfig.h"
#include "../common/EngineAccess.h"

#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace
{
    // CNWSCreature's own copies.
    namespace creature
    {
        constexpr size_t kStats                = 0x20D8;  // CNWSCreature::m_pStats
        constexpr size_t kHeadVariation        = 0x1148;  // unsigned char
        constexpr size_t kTailVariation        = 0x1149;
        constexpr size_t kWingVariation        = 0x114A;
        constexpr size_t kHairVariation        = 0x114B;
        constexpr size_t kTint                 = 0x114C;  // NWN2_TintSet, 48 bytes
        constexpr size_t kHeadTint             = 0x117C;
        constexpr size_t kHairTint             = 0x11AC;
        constexpr size_t kFacialHairVariation  = 0x18D6;
    }

    // CNWSCreatureStats, reached through CNWSCreature::m_pStats above.
    namespace stats
    {
        constexpr size_t kRace                 = 0x0018;  // unsigned short
        constexpr size_t kTint                 = 0x084C;  // NWN2_TintSet
        constexpr size_t kHeadTint             = 0x087C;
        constexpr size_t kHairTint             = 0x08AC;
        constexpr size_t kHairVariation        = 0x08E8;  // unsigned char
        constexpr size_t kFacialHairVariation  = 0x08E9;
        constexpr size_t kHeadVariation        = 0x08EE;
        constexpr size_t kTailVariation        = 0x0FD0;
        constexpr size_t kWingVariation        = 0x0FD1;
    }

    /// NWN2_TintSet is Color[3], and Color is four floats in 0..1.
    struct Color { float r, g, b, a; };
    struct TintSet { Color colors[3]; };
    static_assert(sizeof(TintSet) == 48, "NWN2_TintSet is 48 bytes");

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

    /// True if size bytes at address are committed and readable. Every write below is to an offset
    /// only known to hold for the builds this was developed against, so a wrong offset has to fail
    /// rather than corrupt the server.
    bool IsWritable(const void* address, size_t size)
    {
        if (!address)
        {
            return false;
        }

        MEMORY_BASIC_INFORMATION info{};
        if (::VirtualQuery(address, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT)
        {
            return false;
        }

        constexpr DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY
                                 | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!(info.Protect & writable) || (info.Protect & PAGE_GUARD))
        {
            return false;
        }

        auto start = static_cast<const unsigned char*>(info.BaseAddress);
        auto want  = static_cast<const unsigned char*>(address) + size;
        return want <= start + info.RegionSize;
    }

    template <typename T>
    bool Poke(void* object, size_t offset, const T& value)
    {
        auto at = static_cast<unsigned char*>(object) + offset;
        if (!IsWritable(at, sizeof(T)))
        {
            return false;
        }

        std::memcpy(at, &value, sizeof(T));
        return true;
    }

    /// CNWSCreature::m_pStats, or null if it cannot be read.
    void* StatsOf(void* creatureObject)
    {
        auto at = static_cast<const unsigned char*>(creatureObject) + creature::kStats;
        if (!IsWritable(const_cast<unsigned char*>(at), sizeof(void*)))
        {
            return nullptr;
        }

        return *reinterpret_cast<void* const*>(at);
    }

    /// Parses NWNX4's tint string: NWN2_TintSet[0xRRGGBBAA, 0xRRGGBBAA, 0xRRGGBBAA].
    /// Each byte becomes a float in 0..1, which is how the engine stores it.
    bool ParseTintSet(const char* text, TintSet& out)
    {
        if (!text)
        {
            return false;
        }

        unsigned long packed[3]{};
        if (std::sscanf(text, "NWN2_TintSet[0x%08lX, 0x%08lX, 0x%08lX]",
                        &packed[0], &packed[1], &packed[2]) != 3)
        {
            return false;
        }

        for (int i = 0; i < 3; ++i)
        {
            out.colors[i].r = static_cast<float>((packed[i] >> 24) & 0xFF) / 255.0f;
            out.colors[i].g = static_cast<float>((packed[i] >> 16) & 0xFF) / 255.0f;
            out.colors[i].b = static_cast<float>((packed[i] >>  8) & 0xFF) / 255.0f;
            out.colors[i].a = static_cast<float>((packed[i] >>  0) & 0xFF) / 255.0f;
        }

        return true;
    }

    struct XpObjectAttributesConfig
    {
        std::optional<std::string> plugin_class;
        std::optional<std::string> log_level;
        std::optional<std::string> log_max_file_size;
    };
}

/// <summary>Lets <c>YAML::Node</c> decode <see cref="XpObjectAttributesConfig"/>.</summary>
template <>
struct YAML::convert<XpObjectAttributesConfig>
{
    static bool decode(const Node& node, XpObjectAttributesConfig& c)
    {
        if (!node.IsMap())
        {
            return false;
        }

        c.plugin_class      = node["class"].as<std::optional<std::string>>(std::nullopt);
        c.log_level         = node["log_level"].as<std::optional<std::string>>(std::nullopt);
        c.log_max_file_size = node["log_max_file_size"].as<std::optional<std::string>>(std::nullopt);
        return true;
    }
};

namespace
{
    XpObjectAttributesConfig LoadConfig()
    {
        auto path = OwnPath().replace_extension(L".yml");
        if (!std::filesystem::exists(path))
        {
            return {};
        }

        auto config = Yaml::FromFile<XpObjectAttributesConfig>(path);
        if (!config)
        {
            GetLogger()("could not read {}: {}. Using defaults.", path.string(), config.error());
            return {};
        }

        return *config;
    }

    const XpObjectAttributesConfig& Config()
    {
        static XpObjectAttributesConfig config = LoadConfig();
        return config;
    }

    class XpObjectAttributes : public nwn2::PluginBase
    {
    public:
        XpObjectAttributes() : _pluginId(Config().plugin_class.value_or("OBJECTATTRIBUTES")) {}

        const char* GetPluginId() const override { return _pluginId.c_str(); }

        void OnInitialize(nwn2::PluginHost host) override
        {
            nwn2ports::ApplyLogConfig(GetLogger(), Config());

            if (_creatures.Resolve(host, GetLogger()))
            {
                GetLogger()("ready.");
            }
            else
            {
                GetLogger()(Logger::Level::Error,
                            "creature lookup is unavailable, so every call will be refused.");
            }
        }

        /// NWNX4's contract: the object id arrives in param2 and the new value as the string.
        void OnNWNXSetString(const char* function, const char* param1, int32_t param2,
                             const char* value) override
        {
            std::string_view fn(function ? function : "");

            if (fn.empty())
            {
                GetLogger()("SetString: no function specified.");
                return;
            }

            if (!_creatures.Resolved())
            {
                GetLogger()(Logger::Level::Error, "'{}' refused: creature lookup never resolved.", fn);
                return;
            }

            void* creature = _creatures(static_cast<uint32_t>(param2));
            if (!creature)
            {
                GetLogger()("'{}': {:08X} is not a creature.", fn, static_cast<uint32_t>(param2));
                return;
            }

            if (fn == "SetHeadVariation")        { SetVariation(creature, fn, value, creature::kHeadVariation,       stats::kHeadVariation); }
            else if (fn == "SetHairVariation")   { SetVariation(creature, fn, value, creature::kHairVariation,       stats::kHairVariation); }
            else if (fn == "SetTailVariation")   { SetVariation(creature, fn, value, creature::kTailVariation,       stats::kTailVariation); }
            else if (fn == "SetWingVariation")   { SetVariation(creature, fn, value, creature::kWingVariation,       stats::kWingVariation); }
            else if (fn == "SetFacialHairVariation") { SetVariation(creature, fn, value, creature::kFacialHairVariation, stats::kFacialHairVariation); }
            else if (fn == "SetBodyTint")        { SetTint(creature, fn, value, creature::kTint,     stats::kTint); }
            else if (fn == "SetHeadTint")        { SetTint(creature, fn, value, creature::kHeadTint, stats::kHeadTint); }
            else if (fn == "SetHairTint")        { SetTint(creature, fn, value, creature::kHairTint, stats::kHairTint); }
            else if (fn == "SetRace")            { SetRace(creature, value); }
            else
            {
                GetLogger()("SetString: unknown function '{}'.", fn);
            }
        }

        void OnNWNXGetString(const char* function, const char* param1, int32_t param2,
                             nwn2::StringResult& result) override
        {
            std::string_view fn(function ? function : "");

            if (fn == "GET SUBCLASS")         { result.Set("OBJECTATTRIBUTES"); }
            else if (fn == "GET VERSION")     { result.Set("1.0.0"); }
            else if (fn == "GET DESCRIPTION") { result.Set("This plugin edits creature appearance at runtime."); }
        }

        void OnUnhandledException(const char* callback, const char* what) noexcept override
        {
            GetLogger()(Logger::Level::Error, "exception escaping {}: {}",
                        callback ? callback : "?", what ? what : "?");
        }

    private:
        /// A one-byte appearance variation, written to the creature and its stats.
        void SetVariation(void* creature, std::string_view fn, const char* value,
                          size_t creatureOffset, size_t statsOffset)
        {
            auto variation = static_cast<uint8_t>(std::strtoul(value ? value : "", nullptr, 0));

            bool ok = Poke(creature, creatureOffset, variation);

            if (void* stats = StatsOf(creature))
            {
                ok = Poke(stats, statsOffset, variation) && ok;
            }
            else
            {
                ok = false;
            }

            Report(fn, ok, std::to_string(variation));
        }

        void SetTint(void* creature, std::string_view fn, const char* value,
                     size_t creatureOffset, size_t statsOffset)
        {
            TintSet tint{};
            if (!ParseTintSet(value, tint))
            {
                GetLogger()("'{}': '{}' is not a tint set - expected "
                            "NWN2_TintSet[0xRRGGBBAA, 0xRRGGBBAA, 0xRRGGBBAA].",
                            fn, value ? value : "");
                return;
            }

            bool ok = Poke(creature, creatureOffset, tint);

            if (void* stats = StatsOf(creature))
            {
                ok = Poke(stats, statsOffset, tint) && ok;
            }
            else
            {
                ok = false;
            }

            Report(fn, ok, value ? value : "");
        }

        /// Race lives only on the stats, which is why the original's SetRace did not touch the
        /// creature itself either.
        void SetRace(void* creature, const char* value)
        {
            auto race = static_cast<uint16_t>(std::strtoul(value ? value : "", nullptr, 0));

            void* stats = StatsOf(creature);
            bool ok = stats && Poke(stats, stats::kRace, race);

            Report("SetRace", ok, std::to_string(race));
        }

        void Report(std::string_view fn, bool ok, const std::string& value)
        {
            if (ok)
            {
                GetLogger()(Logger::Level::Debug, "{} = {}", fn, value);
            }
            else
            {
                GetLogger()(Logger::Level::Error,
                            "{} failed to write. The field offsets are only known to hold on the "
                            "builds this was developed against, and may not hold here.", fn);
            }
        }

        std::string _pluginId;
        nwn2ports::CreatureLookup _creatures;
    };
}

NWN2_EXPORT_PLUGIN(XpObjectAttributes)
