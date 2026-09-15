#pragma once
#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/// <summary>RAII wrapper around a Win32 <c>HANDLE</c>: closes it automatically, move-only.</summary>
class Handle
{
public:
    /// <summary>Constructs an empty handle.</summary>
    Handle()
        : _Handle(nullptr)
    {
    }

    /// <summary>Takes ownership of an existing handle.</summary>
    /// <param name="handle">The handle to own.</param>
    Handle(HANDLE handle)
        : _Handle(handle)
    {
    }

    /// <summary>Takes ownership of an existing handle, replacing any previously owned handle without closing it.</summary>
    /// <param name="handle">The handle to own.</param>
    Handle& operator =(HANDLE handle)
    {
        _Handle = handle;

        return *this;
    }

    /// <summary>Transfers ownership from another <see cref="Handle"/>.</summary>
    Handle(Handle&& other) noexcept
    {
        _Handle = other._Handle;
        other._Handle = NULL;
    }

    /// <summary>Transfers ownership from another <see cref="Handle"/>.</summary>
    Handle& operator =(Handle&& other) noexcept
    {
        _Handle = other._Handle;
        other._Handle = NULL;

        return *this;
    }

    Handle(const Handle &) = delete;
    Handle& operator =(const Handle&) = delete;

    /// <summary>Gets the underlying Win32 handle.</summary>
    operator HANDLE() const { return _Handle; }

    /// <summary>Closes the handle now, if one is owned.</summary>
    void Close()
    {
        if (_Handle)
        {
            ::CloseHandle(_Handle);
            _Handle = nullptr;
        }
    }

    /// <summary>Closes the handle, if one is owned.</summary>
    ~Handle()
    {
        if (_Handle)
        {
            ::CloseHandle(_Handle);
        }
    }
private:
    HANDLE _Handle;
};

