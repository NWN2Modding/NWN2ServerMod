#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef min

#include <Psapi.h>
#include "PEPattern.h"
#include <format>

std::expected<std::vector<int>, std::string> PEPattern::ParseSignature(std::string_view signature)
{
    char first = 0;
    std::vector<int> bytes;

    for (auto c : signature)
    {
        if (isspace(c))
        {
            if (first != 0)
            {
                return std::unexpected("Invalid pattern sequence: unexpected space");
            }

            continue;
        }
        else if (c == '?')
        {
            switch (first)
            {
                case '?': bytes.push_back(-1); first = 0; break;
                case '\0': first = '?'; break;
                default: return std::unexpected("Invalid pattern sequence: expected ?");
            }
        }
        else if(isxdigit(c))
        {
            if (first == 0)
            {
                first = c;
            }
            else if (isxdigit(first))
            {
                bytes.push_back((ToHexValue<int>(first) * 16) + ToHexValue<int>(c));

                first = 0;
            }
            else
            {
                return std::unexpected("Invalid pattern sequence: expected hex digit");
            }
        }
        else
        {
            return std::unexpected(std::format("invalid pattern sequence: unexpected character '{}'", c));
        }
    }

    return bytes;
}

std::expected<void*, std::string> PEPattern::FindPattern(std::wstring_view moduleName, std::string_view pattern)
{
    HMODULE hModule = GetModuleHandleW(std::wstring(moduleName).c_str());
    if (!hModule)
    {
        auto error = ::GetLastError();

        return std::unexpected(std::format("GetModuleHandleW failed: 0x{:08X}", error));
    }

    MODULEINFO moduleInfo;
    if (!GetModuleInformation(GetCurrentProcess(), hModule, &moduleInfo, sizeof(moduleInfo)))
    {
        auto error = ::GetLastError();

        return std::unexpected(std::format("GetModuleInformation failed: 0x{:08X}", error));
    }

    uint8_t* baseAddress = reinterpret_cast<uint8_t*>(moduleInfo.lpBaseOfDll);
    DWORD moduleSize = moduleInfo.SizeOfImage;

    auto patternResult = ParseSignature(pattern);
    if (!patternResult)
    {
        return std::unexpected(std::format("Failed to parse byte pattern: {}", patternResult.error()));
    }

    auto patternSize = patternResult.value().size();
    auto &patternBytes = patternResult.value();
    // Scan through the module memory
    for (DWORD i = 0; i < moduleSize - patternSize; ++i)
    {
        bool found = true;
        for (size_t j = 0; j < patternSize; ++j)
        {
            // Check if byte matches, or if it's a wildcard (-1)
            if (patternBytes[j] != -1 && baseAddress[i + j] != patternBytes[j])
            {
                found = false;
                break;
            }
        }

        if (found)
        {
            return &baseAddress[i];
        }
    }

    return std::unexpected("signature not found");
}


std::expected<void*, std::string> PEPattern::FindPatternRange(std::wstring_view moduleName, DWORD offset, DWORD max, std::string_view pattern)
{
    HMODULE hModule = GetModuleHandleW(std::wstring(moduleName).c_str());
    if (!hModule)
    {
        auto error = ::GetLastError();

        return std::unexpected(std::format("GetModuleHandleW failed: 0x{:08X}", error));
    }

    MODULEINFO moduleInfo;
    if (!GetModuleInformation(GetCurrentProcess(), hModule, &moduleInfo, sizeof(moduleInfo)))
    {
        auto error = ::GetLastError();

        return std::unexpected(std::format("GetModuleInformation failed: 0x{:08X}", error));
    }

    uint8_t* baseAddress = reinterpret_cast<uint8_t*>(moduleInfo.lpBaseOfDll);
    DWORD moduleSize = moduleInfo.SizeOfImage;

    auto patternResult = ParseSignature(pattern);
    if (!patternResult)
    {
        return std::unexpected(std::format("Failed to parse byte pattern: {}", patternResult.error()));
    }

    DWORD patternSize = (DWORD)patternResult.value().size();

    // Check if it fits
    if ((uintptr_t)(baseAddress + offset + patternSize) > (uintptr_t)(baseAddress + (uintptr_t)moduleSize))
    {
        return std::unexpected("Offset and pattern outside of search area.");
    }

    if (patternSize > max || patternSize > moduleSize)
    {
        return std::unexpected("Pattern outside of search area.");
    }

    DWORD patternMax = std::min((offset + max) - patternSize, moduleSize - patternSize);
    
    
    auto& patternBytes = patternResult.value();

    // Scan through the module memory
    for (DWORD i = offset; i < patternMax; ++i)
    {
        bool found = true;
        for (size_t j = 0; j < patternSize; ++j)
        {
            // Check if byte matches, or if it's a wildcard (-1)
            if (patternBytes[j] != -1 && baseAddress[i + j] != patternBytes[j])
            {
                found = false;
                break;
            }
        }

        if (found)
        {
            return &baseAddress[i];
        }
    }

    return std::unexpected("signature not found");
}

std::expected<void *, std::string> PEPattern::ExtractCommandTableOffset(std::wstring_view module, std::string_view pattern)
{
    auto findResult = FindPattern(module, pattern);
    if (!findResult)
    {
        return std::unexpected(findResult.error());
    }


    auto offset = (uint8_t *)findResult.value();
    // The instruction we want is: 49 8d 50 18 (lea rdx, [r8+18h])
    // It begins exactly 7 bytes into our matched pattern.
    // The offset '18' is the 4th byte of that specific instruction.
    // Therefore, 7 + 3 = byte index 10.

    size_t memberOffset = (size_t)(offset + 10);


    return (void *)memberOffset;
}