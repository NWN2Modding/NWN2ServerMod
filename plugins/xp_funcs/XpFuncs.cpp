// xp_funcs, ported from NWNX4 to the NWN2ServerMod C ABI.
//
// Original: Copyright (C) 2010 Andrew Brockert (Zebranky), GPL v2 or later.
//
// The original had exactly one function, GetCreatureSoundSet, and it reached into the engine three
// ways to get it: a hardcoded CAppManager pointer, CServerExoApp::GetCreatureByGameObjectID, and
// CNWSCreature::GetSoundSet. On x64 the middle one cannot be resolved by signature - it is one of
// 36 byte-identical siblings generated from the same template, differing only in a RIP-relative
// displacement.
//
// None of that is needed. SoundSetFile is a field in the creature's own serialized form, so this
// port asks the engine to serialize the creature through SCORCO and reads the field out of the
// GFF. No engine addresses, no byte patterns, nothing that differs between the GOG and Steam
// builds, and nothing that a future patch could move.
//
// It is also arguably closer to the truth than the original. SCORCO serializes live state rather
// than the blueprint, so anything that changed a creature's soundset at runtime is reflected here.
//
// The cost is that a member read became a serialization of the whole creature - tens of KB. That
// is irrelevant for the occasional soundset lookup and would hurt in a loop over every creature in
// an area. See the README.
//
// Scripts call this through nwnx_funcs.nss, which hides the two-step:
//
//     StoreCampaignObject("FUNCS", "soundset", oCreature);   // hands us the serialized creature
//     NWNXGetInt("FUNCS", "GETSOUNDSET", "", 0);             // reads the field back out
#include <Plugin.hpp>
#include <Logger.h>
#include <Data.h>

#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
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

    Logger& GetLogger()
    {
        static Logger logger(
            std::filesystem::path(GetFullModulePath(GetOwnModule())).replace_extension(L".log"),
            true,
            true);
        return logger;
    }

    /// A reader for GFF V3.2, the format every NWN2 game object serializes to.
    ///
    /// This parses data that arrived from outside the plugin, inside the server process, so every
    /// offset and count is checked against the real length before it is used. A malformed blob
    /// makes Parse fail; it never reads past the end.
    class GffReader
    {
    public:
        /// Validates the header and records where the tables are. False if this is not a GFF, or
        /// if any table would run off the end.
        bool Parse(const uint8_t* data, size_t size)
        {
            _data = nullptr;

            // 8 bytes of type and version, then twelve 32-bit table offsets and counts.
            constexpr size_t kHeaderSize = 56;
            if (!data || size < kHeaderSize)
            {
                return false;
            }

            if (std::memcmp(data + 4, "V3.2", 4) != 0)
            {
                return false;
            }

            uint32_t header[12];
            std::memcpy(header, data + 8, sizeof(header));

            _fieldOffset = header[2];
            _fieldCount  = header[3];
            _labelOffset = header[4];
            _labelCount  = header[5];

            if (!FitsWithin(_fieldOffset, _fieldCount, kFieldSize, size) ||
                !FitsWithin(_labelOffset, _labelCount, kLabelSize, size))
            {
                return false;
            }

            _data = data;
            _size = size;
            return true;
        }

        /// The file type, e.g. "UTC " for a creature. Empty if nothing has parsed.
        std::string_view FileType() const
        {
            return _data ? std::string_view(reinterpret_cast<const char*>(_data), 4)
                         : std::string_view();
        }

        /// Reads a field stored inline in the field entry - the integer types up to 32 bits, which
        /// is every type this plugin needs. Returns false if the label is absent or holds a type
        /// whose value lives in the field data block instead.
        bool FindInt(std::string_view label, int32_t& outValue) const
        {
            if (!_data)
            {
                return false;
            }

            for (uint32_t i = 0; i < _fieldCount; ++i)
            {
                const uint8_t* field = _data + _fieldOffset + i * kFieldSize;

                uint32_t type, labelIndex, data;
                std::memcpy(&type, field, 4);
                std::memcpy(&labelIndex, field + 4, 4);
                std::memcpy(&data, field + 8, 4);

                if (labelIndex >= _labelCount || LabelAt(labelIndex) != label)
                {
                    continue;
                }

                // GFF stores the small scalar types directly in the field entry. Anything else
                // keeps an offset here instead, and reading it as a value would be nonsense.
                switch (type)
                {
                    case kByte:  outValue = static_cast<int32_t>(data & 0xFF); return true;
                    case kChar:  outValue = static_cast<int8_t>(data & 0xFF); return true;
                    case kWord:  outValue = static_cast<int32_t>(data & 0xFFFF); return true;
                    case kShort: outValue = static_cast<int16_t>(data & 0xFFFF); return true;
                    case kDword: outValue = static_cast<int32_t>(data); return true;
                    case kInt:   outValue = static_cast<int32_t>(data); return true;
                    default:
                        GetLogger()("field '{}' is type {}, which is not an inline integer.", label, type);
                        return false;
                }
            }

            return false;
        }

    private:
        static constexpr size_t kFieldSize = 12;   // type, label index, data
        static constexpr size_t kLabelSize = 16;   // null-padded, not null-terminated

        static constexpr uint32_t kByte = 0, kChar = 1, kWord = 2, kShort = 3, kDword = 4, kInt = 5;

        /// True if count entries of entrySize starting at offset all lie inside size, without the
        /// multiplication overflowing.
        static bool FitsWithin(uint32_t offset, uint32_t count, size_t entrySize, size_t size)
        {
            size_t span = static_cast<size_t>(count) * entrySize;
            if (entrySize != 0 && span / entrySize != count)
            {
                return false;
            }

            return offset <= size && span <= size - offset;
        }

        /// A label is a fixed 16 bytes, padded with nulls rather than terminated, so a full-length
        /// label has no terminator to find.
        std::string_view LabelAt(uint32_t index) const
        {
            const char* label = reinterpret_cast<const char*>(_data + _labelOffset + index * kLabelSize);

            size_t length = 0;
            while (length < kLabelSize && label[length] != '\0')
            {
                ++length;
            }

            return std::string_view(label, length);
        }

        const uint8_t* _data = nullptr;
        size_t _size = 0;
        uint32_t _fieldOffset = 0, _fieldCount = 0, _labelOffset = 0, _labelCount = 0;
    };

    class XpFuncs : public nwn2::PluginBase
    {
    public:
        XpFuncs() { GetLogger()("xp_funcs loaded."); }

        const char* GetPluginId() const override { return "FUNCS"; }

        /// The script handed us an object. Keep the bytes so the GETs below can read fields out of
        /// them; the engine's buffer does not outlive this call.
        bool OnSetBinaryData(const char* varName, const char* player,
                             const uint8_t* data, size_t size) override
        {
            _gff = GffReader();
            _object.assign(data, data + size);

            if (!_gff.Parse(_object.data(), _object.size()))
            {
                GetLogger()("'{}': {} bytes that are not a GFF V3.2 object.", varName ? varName : "", size);
                _object.clear();
                return false;
            }

            GetLogger()("'{}': {} bytes, type '{}'.", varName ? varName : "", size, _gff.FileType());
            return true;
        }

        bool OnNWNXGetInt(const char* function, const char* param1, int32_t param2,
                          int32_t& outValue) override
        {
            std::string_view fn(function ? function : "");

            if (fn.empty())
            {
                GetLogger()("GetInt: no function specified.");
                return false;
            }

            // The original plugin's only function. Named rather than generic so existing scripts
            // keep working unchanged.
            if (fn == "GETSOUNDSET")
            {
                return ReadField("SoundSetFile", outValue);
            }

            // Everything above is one field lookup, so the general case costs nothing extra: any
            // integer field of the last stored object, by its GFF label.
            if (fn == "GETFIELDINT")
            {
                return ReadField(param1 ? param1 : "", outValue);
            }

            GetLogger()("GetInt: unknown function '{}'.", fn);
            return false;
        }

        void OnNWNXGetString(const char* function, const char* param1, int32_t param2,
                             nwn2::StringResult& result) override
        {
            std::string_view fn(function ? function : "");

            if (fn == "GET SUBCLASS")    { result.Set("FUNCS"); }
            else if (fn == "GET VERSION")     { result.Set("0.0.2"); }
            else if (fn == "GET DESCRIPTION") { result.Set("This plugin reads fields out of serialized game objects."); }
        }

        void OnUnhandledException(const char* callback, const char* what) noexcept override
        {
            GetLogger()("exception escaping {}: {}", callback ? callback : "?", what ? what : "?");
        }

    private:
        bool ReadField(std::string_view label, int32_t& outValue)
        {
            if (_object.empty())
            {
                GetLogger()("'{}' asked for before any object was stored.", label);
                return false;
            }

            if (!_gff.FindInt(label, outValue))
            {
                GetLogger()("'{}' is not an integer field of the stored object.", label);
                return false;
            }

            return true;
        }

        std::vector<uint8_t> _object;
        GffReader _gff;
    };
}

NWN2_EXPORT_PLUGIN(XpFuncs)
