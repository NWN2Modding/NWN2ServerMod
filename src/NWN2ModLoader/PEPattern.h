#pragma once
#include <expected>
#include <string>
#include <vector>

/// <summary>
/// IDA-style byte-pattern scanner used to locate NWN2Server64.exe's internal (unexported,
/// non-symboled) functions and globals at runtime.
/// </summary>
class PEPattern
{
public:
    /// <summary>Finds a pattern and returns a fixed offset into the matched region.</summary>
    /// <param name="module">The module name to search, e.g. <c>L"NWN2Server64.exe"</c>.</param>
    /// <param name="pattern">An IDA-style hex pattern, with <c>??</c> as a wildcard byte.</param>
    /// <returns>The address of the fixed offset within the match, or an error message.</returns>
    static std::expected<void*, std::string> ExtractCommandTableOffset(std::wstring_view module, std::string_view pattern);

    /// <summary>Searches an entire module's memory image for the first match of a byte pattern.</summary>
    /// <param name="moduleName">The module name to search, e.g. <c>L"NWN2Server64.exe"</c>.</param>
    /// <param name="pattern">An IDA-style hex pattern, with <c>??</c> as a wildcard byte.</param>
    /// <returns>The address of the first match, or an error message if none was found.</returns>
    static std::expected<void*, std::string> FindPattern(std::wstring_view moduleName, std::string_view pattern);

    /// <summary>Searches a bounded range of a module's memory image for the first match of a byte pattern.</summary>
    /// <param name="moduleName">The module name to search, e.g. <c>L"NWN2Server64.exe"</c>.</param>
    /// <param name="offset">The offset from the module base to start searching at.</param>
    /// <param name="max">The maximum number of bytes, from <paramref name="offset"/>, to search within.</param>
    /// <param name="pattern">An IDA-style hex pattern, with <c>??</c> as a wildcard byte.</param>
    /// <returns>The address of the first match, or an error message if none was found.</returns>
    static std::expected<void*, std::string> FindPatternRange(std::wstring_view moduleName, DWORD offset, DWORD max, std::string_view pattern);

    /// <summary>Parses an IDA-style hex pattern string into per-byte values, with <c>-1</c> marking a wildcard.</summary>
    /// <param name="signature">An IDA-style hex pattern, with <c>??</c> as a wildcard byte.</param>
    /// <returns>The parsed byte/wildcard sequence, or an error message if the pattern text is malformed.</returns>
    static std::expected<std::vector<int>, std::string> ParseSignature(std::string_view signature);

private:
    /// <summary>Converts a single hex digit character to its numeric value.</summary>
    template <typename T>
    static T ToHexValue(char c)
    {
        if (c >= 'A' && c <= 'F')
        {
            return ((T)0xA) + ((T)(c - 'A'));
        }
        else if (c >= 'a' && c <= 'f')
        {
            return ((T)0xA) + ((T)(c - 'a'));
        }
        else if (c >= '0' && c <= '9')
        {
            return ((T)(c - '0'));
        }

        return (T)0;
    }
};
