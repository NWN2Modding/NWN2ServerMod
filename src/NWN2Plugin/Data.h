#pragma once
#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <filesystem>
#include <string>
#include <fstream>
#include <expected>

/// <summary>Copies as much of <paramref name="source"/> as fits into <paramref name="destination"/>, always null-terminating.</summary>
/// <param name="destination">The fixed-size buffer to copy into.</param>
/// <param name="source">The source text.</param>
template <size_t N>
void SafeStringCopy(char(&destination)[N], std::string_view source)
{
    size_t copyLength = (std::min)(source.length(), N - 1);

    std::copy_n(source.data(), copyLength, destination);

    destination[copyLength] = '\0';
}

/// <summary>Copies as much of <paramref name="source"/> as fits into <paramref name="destination"/>, always null-terminating.</summary>
/// <param name="destination">The fixed-size buffer to copy into.</param>
/// <param name="source">The source text.</param>
template <size_t N>
void SafeStringCopy(wchar_t(&destination)[N], std::wstring_view source)
{
    size_t copyLength = (std::min)(source.length(), N - 1);

    std::copy_n(source.data(), copyLength, destination);

    destination[copyLength] = L'\0';
}

/// <summary>Converts a UTF-8 string to a wide (UTF-16) string.</summary>
/// <param name="src">The UTF-8 source text.</param>
/// <returns>The converted text, or an empty string on failure.</returns>
std::wstring ToWString(std::string_view src);

/// <summary>Converts a wide (UTF-16) string to a UTF-8 string.</summary>
/// <param name="src">The wide source text.</param>
/// <returns>The converted text, or an empty string on failure.</returns>
std::string ToString(std::wstring_view src);

/// <summary>Gets the full path of a loaded module.</summary>
/// <param name="hModule">The module to query, or <see langword="NULL"/> for the current process's executable.</param>
/// <returns>The module's full path, or an empty string on failure.</returns>
std::wstring GetFullModulePath(HMODULE hModule = NULL);

/// <summary>Formats a Win32 error code as a human-readable message.</summary>
/// <param name="error">A Win32 error code, e.g. from <c>GetLastError()</c>.</param>
/// <returns>The formatted message, with trailing newlines stripped.</returns>
std::string GetErrorMessage(DWORD error);

/// <summary>Writes text to a file, creating or truncating it.</summary>
/// <param name="filepath">The full path to write to.</param>
/// <param name="content">The text to write.</param>
/// <returns>An unexpected error message on failure.</returns>
std::expected<void, std::string> WriteStringToFile(const std::string& filepath, const std::string& content);
