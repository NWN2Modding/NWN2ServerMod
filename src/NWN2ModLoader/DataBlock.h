#pragma once
#include <cstdint>
#include <cstdlib>

/// <summary>
/// Minimal reconstructed mirror of the engine's internal <c>DataBlock</c>, used as the parameter
/// shape for the <c>SetBinaryData</c>/<c>GetBinaryData</c> hook signatures.
/// </summary>
/// <remarks>
/// The real <c>DataBlock</c> allocates with <c>malloc()</c>, copies with <c>memmove()</c>, and
/// frees with <c>free()</c> when <see cref="m_owning"/> is set. This mirrors that exactly - not
/// <c>new[]</c>/<c>delete[]</c> - so instances constructed here (see
/// <c>NWN2Mod::HookGetBinaryData</c>'s plugin path) behave identically to engine-constructed ones.
/// Only instances constructed here are ever destroyed through this struct; engine-owned instances
/// (e.g. the <c>pDataBlock</c> passed into <c>SetBinaryData</c>) are never affected by it.
/// </remarks>
struct DataBlock
{
    uint8_t* m_data;        // 0x00: Pointer to the binary payload
    uint64_t m_used;        // 0x08: The actual size of the payload in bytes
    uint64_t m_allocated;   // 0x10: The total allocated capacity of the buffer
    bool     m_owning;      // 0x18: Whether this instance frees the memory on destruction

    /// <summary>Frees <see cref="m_data"/> if this instance owns it.</summary>
    ~DataBlock()
    {
        if (m_owning && m_data)
        {
            std::free(m_data);
        }
    }
};

/// <summary>
/// Mirrors the layout of <c>std::shared_ptr&lt;DataBlock&gt;</c>, which <c>GetBinaryData</c> writes
/// through a hidden return-value pointer (the second <c>__fastcall</c> argument) rather than
/// returning in RAX/RDX.
/// </summary>
struct DataBlockPtr
{
    DataBlock* m_pDataBlock;   // 0x00: shared_ptr::_Ptr
    void*      m_pRefCount;    // 0x08: shared_ptr::_Rep (control block)
};

