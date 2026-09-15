#pragma once
#include <cinttypes>
#include <cstddef>

/// <summary>
/// Minimal reconstructed mirror of the engine's internal <c>CExoString</c>, used only as a
/// parameter shape for hook signatures.
/// </summary>
class CExoString
{
public:
    /// <summary>The null-terminated string buffer.</summary>
    char *m_sString;

    /// <summary>The allocated size of <see cref="m_sString"/>, in bytes.</summary>
    uint32_t m_nBufferLength;
};

