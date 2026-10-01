#pragma once
// A structural check for GFF V3.2, the format every NWN2 game object serializes to.
//
// This exists because a magic-bytes check is not enough. A real incident on Sundren's database
// produced rows of exactly 9 bytes - "UTI V3.28" - where a CSV export had cut every object off at
// its first NUL. Those rows carry a correct file type and a correct "V3.2" version, so anything
// that only compares the first eight bytes declares them valid and hands the engine a stump.
//
// Checking the header's own table offsets against the real length catches that, and anything else
// that arrives short, without needing to parse the object.
#include <cstdint>
#include <cstring>
#include <string>

namespace nwn2ports
{
    /// Why a blob is not a usable GFF object, or empty if it is.
    ///
    /// This does not prove the object is intact - only that its header is self-consistent and its
    /// tables lie inside the data. That is enough to tell a truncated or mangled row from a
    /// healthy one, which is the case worth logging.
    inline std::string GffRejectReason(const uint8_t* data, size_t size)
    {
        // 8 bytes of type and version, then twelve 32-bit offsets and counts.
        constexpr size_t kHeaderSize = 56;

        if (!data)
        {
            return "no data";
        }

        if (size < kHeaderSize)
        {
            return "only " + std::to_string(size) + " bytes; a GFF header alone is "
                 + std::to_string(kHeaderSize);
        }

        if (std::memcmp(data + 4, "V3.2", 4) != 0)
        {
            return "no GFF V3.2 version marker";
        }

        uint32_t header[12];
        std::memcpy(header, data + 8, sizeof(header));

        // Offset/count pairs: structs, fields, labels, field data, field indices, list indices.
        // Entry sizes for the first three; the rest are byte counts.
        const struct { const char* name; uint32_t offset; uint32_t count; uint32_t entrySize; } tables[] = {
            { "struct",        header[0],  header[1],  12 },
            { "field",         header[2],  header[3],  12 },
            { "label",         header[4],  header[5],  16 },
            { "field data",    header[6],  header[7],  1  },
            { "field indices", header[8],  header[9],  1  },
            { "list indices",  header[10], header[11], 1  },
        };

        for (const auto& table : tables)
        {
            uint64_t span = static_cast<uint64_t>(table.count) * table.entrySize;
            uint64_t end  = static_cast<uint64_t>(table.offset) + span;

            if (end > size)
            {
                return std::string(table.name) + " table runs past the end of the data ("
                     + std::to_string(end) + " > " + std::to_string(size) + ")";
            }
        }

        return {};
    }

    /// True if the blob's header is self-consistent and its tables fit inside it.
    inline bool GffLooksValid(const uint8_t* data, size_t size)
    {
        return GffRejectReason(data, size).empty();
    }
}
