#pragma once
#include <cinttypes>
#include <cstddef>

/// <summary>
/// Minimal reconstructed layout of the engine's internal <c>CNWVirtualMachineCommands</c>, whose
/// member offsets are verified below with <c>static_assert</c>.
/// </summary>
/// <remarks>
/// Treat these offsets as reverse-engineered ABI facts, not arbitrary code - do not "clean them
/// up" without re-verifying against the actual binary.
/// </remarks>
class CNWVirtualMachineCommands
{
public:
    /// <summary>Virtual only to force the 8-byte vtable pointer at offset 0x000, matching the real class.</summary>
    virtual ~CNWVirtualMachineCommands() = default;

    bool m_bValidObjectRunScript;           // 0x008
    uint32_t m_oidObjectRunScript;          // 0x00C
    bool m_bRunscriptNotificationEnabled;   // 0x010

    /// <summary>The table of script command function pointers, indexed by <see cref="Commands"/> constant.</summary>
    void** m_pVirtualMachineCommands;       // 0x018
};

static_assert(offsetof(CNWVirtualMachineCommands, m_bValidObjectRunScript) == 0x008, "Offset mismatch!");
static_assert(offsetof(CNWVirtualMachineCommands, m_oidObjectRunScript) == 0x00C, "Offset mismatch!");
static_assert(offsetof(CNWVirtualMachineCommands, m_bRunscriptNotificationEnabled) == 0x010, "Offset mismatch!");
static_assert(offsetof(CNWVirtualMachineCommands, m_pVirtualMachineCommands) == 0x018, "Offset mismatch!");