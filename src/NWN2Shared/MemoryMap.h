#pragma once
#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <expected>
#include <memory>
#include <string>
#include "Handle.h"
#include "Data.h"

/// <summary>Wraps a named Win32 file-mapping object, used as a cross-process shared memory region.</summary>
class MemoryMap
{
public:
    /// <summary>Constructs an empty memory map.</summary>
    MemoryMap()
    {
    }

    /// <summary>Takes ownership of an already-open mapping handle.</summary>
    MemoryMap(Handle&& other) noexcept
        : _Handle(std::move(other))
    {
    }

    /// <summary>Transfers ownership from another <see cref="MemoryMap"/>.</summary>
    MemoryMap(MemoryMap&& other) noexcept
    {
        _Handle = std::move(other._Handle);
    }

    /// <summary>Takes ownership of an already-open mapping handle.</summary>
    MemoryMap& operator =(Handle&& other) noexcept
    {
        _Handle = std::move(other);

        return *this;
    }

    /// <summary>Transfers ownership from another <see cref="MemoryMap"/>.</summary>
    MemoryMap& operator =(MemoryMap&& other) noexcept
    {
        _Handle = std::move(other._Handle);

        return *this;
    }

    MemoryMap(const MemoryMap&) = delete;
    MemoryMap& operator =(const MemoryMap&) = delete;

    /// <summary>Gets the underlying Win32 file-mapping handle.</summary>
    operator HANDLE() const { return _Handle; }

    /// <summary>Creates a new named file-mapping object backed by the system paging file.</summary>
    /// <param name="name">The mapping's name, e.g. <c>L"Local\\NWN2Shared"</c>.</param>
    /// <param name="size">The size of the mapping, in bytes.</param>
    /// <returns>An unexpected error message on failure.</returns>
    std::expected<void, std::string> Create(std::wstring_view name, size_t size)
    {
        _Handle.Close();

        HANDLE hMapFile = CreateFileMappingW(
            INVALID_HANDLE_VALUE,    // Use paging file
            NULL,                    // Default security
            PAGE_READWRITE,          // Read/write access
            (DWORD)((0xFFFFFFFF00000000 & size) >> 32ull), // Maximum object size (high-order DWORD)
            (DWORD)(0x00000000FFFFFFFF & size),       // Maximum object size (low-order DWORD)
            name.data()          // Name of mapping object
        );

        if (hMapFile == NULL)
        {
            DWORD error = ::GetLastError();
            return std::unexpected(std::format("CreateFileMappingW failed: {} (0x{:08X}", GetErrorMessage(error), error));
        }

        _Handle = hMapFile;

        return {};
    }

    /// <summary>Opens an existing named file-mapping object.</summary>
    /// <param name="name">The mapping's name, e.g. <c>L"Local\\NWN2Shared"</c>.</param>
    /// <returns>An unexpected error message on failure.</returns>
    std::expected<void, std::string> Open(std::wstring_view name)
    {
        _Handle.Close();

        HANDLE hMapFile = OpenFileMappingW(
            FILE_MAP_ALL_ACCESS,   // Read access
            FALSE,           // Do not inherit the name
            name.data()  // Exact same name
        );

        if (hMapFile == NULL)
        {
            DWORD error = ::GetLastError();
            return std::unexpected(std::format("OpenFileMappingW failed: {} (0x{:08X}", GetErrorMessage(error), error));
        }

        _Handle = hMapFile;

        return {};
    }

    /// <summary>Maps a view of this mapping into the current process's address space.</summary>
    /// <param name="offset">The offset into the mapping to start the view at.</param>
    /// <param name="size">The number of bytes to map.</param>
    /// <returns>A pointer to the mapped view, or an error message on failure.</returns>
    std::expected<void*, std::string> GetAddress(size_t offset, size_t size)
    {
        void* ptr = MapViewOfFile(
            _Handle,
            FILE_MAP_ALL_ACCESS,
            (DWORD)((0xFFFFFFFF00000000 & offset) >> 32ull),
            (DWORD)(0x00000000FFFFFFFF & offset),
            size);
        if (!ptr)
        {
            DWORD error = ::GetLastError();
            return std::unexpected(std::format("MapViewOfFile failed: {} (0x{:08X}", GetErrorMessage(error), error));
        }

        return ptr;
    }

    /// <summary>Unmaps a view previously returned by <see cref="GetAddress"/>.</summary>
    /// <param name="ptr">The pointer returned by <see cref="GetAddress"/>.</param>
    void UnmapAddressd(void*ptr)
    {
        UnmapViewOfFile(ptr);
    }
private:
    Handle _Handle;
};

