#include "Data.h"

std::wstring ToWString(std::string_view src)
{
    if (src.empty())
    {
        return L"";
    }

    int size_needed = ::MultiByteToWideChar(CP_UTF8, 0, src.data(), (int)src.size(), NULL, 0);
    if (size_needed == 0)
    {
        // Error occurred. Just eat it for the sake of composability.
        return L"";
    }

    std::wstring wstrOut(size_needed, 0);


    int size_written = ::MultiByteToWideChar(CP_UTF8, 0, src.data(), (int)src.size(), wstrOut.data(), size_needed);
    if (size_written != size_needed)
    {
        return L"";
    }

    return wstrOut;
}

std::string ToString(std::wstring_view src)
{
    if (src.empty())
    {
        return "";
    }

    int size_needed = ::WideCharToMultiByte(CP_UTF8, 0, src.data(), (int)src.size(), NULL, 0, nullptr, nullptr);
    if (size_needed == 0)
    {
        // Error occurred. Just eat it for the sake of composability.
        return "";
    }

    std::string strOut(size_needed, 0);

    int size_written = ::WideCharToMultiByte(CP_UTF8, 0, src.data(), (int)src.size(), strOut.data(), size_needed, nullptr, nullptr);
    if (size_written != size_needed)
    {
        return "";
    }

    return strOut;
}

std::wstring GetFullModulePath(HMODULE hModule)
{
    // Start with a reasonable default buffer size
    std::wstring buffer;

    buffer.resize(2048);

    while (true) {
        // Pass NULL to get the current process executable path
        DWORD copied_size = ::GetModuleFileNameW(hModule, buffer.data(), static_cast<DWORD>(buffer.size()));

        if (copied_size == 0) {
            // An actual error occurred (e.g., access denied, though rare for current process)
            return L"";
        }

        // If the return value equals the buffer size, it means the path was truncated.
        // We need to double the buffer size and try again.
        if (copied_size == buffer.size())
        {
            buffer.resize(buffer.size() * 2);
        }
        else
        {
            // Success! Shrink the wstring to the exact length of the path (excluding null terminator)
            buffer.resize(copied_size);
            break;
        }
    }

    return buffer;
}

std::string GetErrorMessage(DWORD error)
{
    // 0 is a success code
    if (error == 0)
    {
        return "No error.";
    }

    LPWSTR messageBuffer = nullptr;

    // Ask Win32 to give us the string associated with a system error id.
    DWORD size = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Use system default language
        (LPWSTR)&messageBuffer, // Cast is required by the API
        0,
        NULL);
    if (size == 0)
    {
        return "Unknown error (FormatMessageW failed).";
    }

    // Copy the buffer into a standard C++ string
    auto result = ToString(std::wstring_view(messageBuffer, size));

    // IMPORTANT: Free the memory allocated by FORMAT_MESSAGE_ALLOCATE_BUFFER
    ::LocalFree(messageBuffer);

    // FormatMessage often includes pesky trailing newlines (\r\n). 
    // It's usually a good idea to strip them out for cleaner logging.
    while (!result.empty()
        && (result.back() == L'\n' || result.back() == L'\r'))
    {
        result.pop_back();
    }

    return result;
}

std::expected<void, std::string> WriteStringToFile(const std::string& filepath, const std::string& content) {
    // Open the file in output mode (truncates existing content)
    std::ofstream file(filepath, std::ios::out | std::ios::trunc);

    if (!file.is_open()) {
        return std::unexpected(std::format("Failed to open or create file: {}", filepath));
    }

    file << content;

    // std::ofstream automatically closes when it goes out of scope, 
    // but you can check for write errors before returning.
    if (file.bad()) {
        return std::unexpected(std::format("Write operation failed for file: {}", filepath));
    }

    return {}; // Success
}