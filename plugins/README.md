# Ported NWNX4 plugins

NWNX4 plugins rebuilt against NWN2ServerMod's C ABI, for the 64-bit NWN2:EE server.

These are ports, not rewrites. The script-facing contract is the same as NWNX4's, so a module's
existing `NWNXGetInt`/`NWNXSetString` calls keep working. Where something had to change, it is
called out below and in a comment at the top of the file.

## Building

These build separately from the loader - the top-level `CMakeLists.txt` does not include this
directory, and `plugins/CMakeLists.txt` can be configured on its own. `NWN2Plugin` is header-only
and a plugin links no loader library, so a port needs the SDK's include directory and two support
translation units and nothing else.

From the repository root:

```bash
cmake -S plugins -B plugins/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build plugins/build
```

`LOADER_SOURCE` defaults to this repository's own `src`. Add
`-DLOADER_SOURCE="/path/to/another/NWN2ServerMod/src"` to build these against a different checkout.

Run it from an x64 Developer prompt, or the compiler will not be found. Both `C` and `CXX` must be
the same compiler — SQLite is C, and CMake refuses a build that mixes clang-cl with MSVC.

DLLs land in `plugins/build/bin`.

## Installing

Add the DLL to the `plugins` list in `nwn2mod.config`:

```yaml
plugins:
  - "C:/path/to/xp_time.dll"
  - "C:/path/to/xp_srvadmin.dll"
  - "C:/path/to/xp_sqlite.dll"
  - "C:/path/to/xp_funcs.dll"
  - "C:/path/to/xp_profiler.dll"
  - "C:/path/to/xp_pickpocket.dll"
  - "C:/path/to/xp_objectattributes.dll"
```

`xp_sqlite` and `xp_mysql` both register as `SQL`, so load one or the other - or change `class` in
one of their configs if you genuinely want both.

Each plugin writes its log next to its own DLL, and reads its config from there too. The loader
log names every plugin it loaded and the ID it registered under, which is the quickest way to
confirm a plugin is actually live.

---

## xp_time

High-resolution named timers. Registers as **`TIME`**.

```nwscript
NWNXSetString("TIME", "START", sName, 0, "");   // start or restart a timer
NWNXGetString("TIME", "STOP",  sName, 0);       // elapsed microseconds, and forget the timer
NWNXGetString("TIME", "QUERY", sName, 0);       // elapsed microseconds, timer keeps running
```

`nwnx_time.nss` appends `ObjectToString(oObject)` to the name, so timers are per-object without
the plugin knowing anything about objects.

Unchanged from NWNX4 apart from the plugin interface.

---

## xp_srvadmin

Drives the server's admin dialog by sending it window messages. Registers as **`SRVADMIN`**.

Every function is available through either `NWNXSetString` (fire and forget) or `NWNXGetInt`
(returns 1 if the action was carried out, 0 if not):

| Function | Parameter |
| --- | --- |
| `BOOTPLAYER` | player name |
| `BANPLAYERNAME` | player name |
| `BANPLAYERIP` | player name |
| `BROADCASTSERVERMESSAGE` | message |
| `SETPLAYERPASSWORD` | password |
| `SETDMPASSWORD` | password |
| `SETADMINPASSWORD` | password |
| `SETELC` | `true` / `false` |
| `SETDISABLEOVERRIDE` | `true` / `false` |
| `SETDISABLECUSTOMGUI` | `true` / `false` |
| `SHUTDOWNNWN2SERVER` | — |

### Changes from NWNX4

**`BANPLAYERCDKEY` is gone.** The 64-bit server's dialog has no Ban CD-Key button, and the control
that inherited its ID bans by IP instead. Calling it logs an explanation and returns 0 rather than
quietly banning the wrong way. Use `BANPLAYERIP` or `BANPLAYERNAME`.

**Two new functions.** `SETDISABLEOVERRIDE` and `SETDISABLECUSTOMGUI` drive checkboxes the 32-bit
server never had.

Checkboxes are set by reading the current state and clicking only if it differs — `BM_SETCHECK`
would change the box without telling the dialog, so the server would not act on it. The state is
read back after the click, so a click the dialog ignored is reported as a failure rather than as
the success it was asked for.

---

## xp_sqlite

SQL from NWScript, backed by SQLite. Registers as **`SQL`** by default.

Use `nwnx_sql.nss` from this directory rather than NWNX4's — see the compatibility note below.

### Configuration

Optional. Without `xp_sqlite.yml` next to the DLL, the defaults apply. See the shipped
`xp_sqlite.yml` for the full annotated reference; briefly:

| Key | Default | Notes |
| --- | --- | --- |
| `class` | `SQL` | the name scripts call this plugin by |
| `file` | `sqlite.db` | relative paths resolve against the DLL's directory |
| `journal_mode` | unset | SQLite's default (`delete`); `wal` is faster and more crash-resistant |
| `synchronous` | unset | SQLite's default (`full`), the most durable setting |
| `wrap_transaction` | `false` | see the warning below |

The database defaults to sitting beside the DLL rather than in the game's install directory,
where the engine keeps its own campaign databases. Nothing good comes of mixing the two.

`journal_mode` and `synchronous` are left unset deliberately. SQLite's own defaults are the most
durable combination it offers, and this plugin does not quietly trade that for speed.

### wrap_transaction

> **Do not turn this on for a database holding player data.**
>
> It wraps the server's entire run in one transaction: `BEGIN` at startup, `COMMIT` at shutdown.
> Nothing is committed in between, so a crash, a power loss, or killing the process loses **every
> write since the server started** — not the last few seconds, the whole session.

### Changes from NWNX4

**Stock SQLite 3.53.4 instead of a patched 3.3.17.** NWNX4 shipped SQLite from 2007 with a local
patch to `sqlite3.c` that invented an extended error code, `SQLITE_ERROR_OPENSTMT`, so a refused
`COMMIT` would arrive as an `SQLITE_ERROR` variant rather than `SQLITE_BUSY`. This port uses stock
SQLite, so the amalgamation can be updated by dropping in a newer one with no patch to reapply.

**A failed statement now reports failure.** NWNX4 handled only `SQLITE_ERROR` and fell out of its
switch to "success" for anything else — including `SQLITE_BUSY`, which is what a refused `COMMIT`
actually returns. A commit that did not happen was reported to the script as though it had.
Anything that is not `SQLITE_DONE` or `SQLITE_ROW` is now a failure.

**`Fetch` checks for a statement first.** The original called `sqlite3_step` unconditionally, and
`sqlite3_step(NULL)` crashes.

**`GETDATA` uses the column's byte count** rather than relying on null termination, so a `TEXT`
value containing an embedded null survives. The original's fixed buffer could not carry one.

---

## nwnx_sql.nss: the one compatibility break

`plugins/common/nwnx_sql.nss` is NWNX4's file with two changes, so it diffs cleanly against upstream.

The one that matters:

```nwscript
// NWNX4
void SQLStoreObject(object oObject) { StoreCampaignObject("NWNX", "-", oObject); }

// here
void SQLStoreObject(object oObject) { StoreCampaignObject(SQL_PLUGIN, "-", oObject); }
```

NWNX4 treated `"NWNX"` as a magic campaign name routed to whichever plugin handled SCORCO — a
second name, separate from the function class. NWN2ServerMod routes SCORCO by plugin ID, so a
plugin answers to exactly one name.

**If you port a module from NWNX4 and objects silently stop persisting, this is why.** Scalar
persistence is unaffected; it never went through SCORCO.

The other change is cosmetic: the plugin name is the constant `SQL_PLUGIN` rather than `"SQL"`
repeated eleven times, so changing `class` in the config means editing one line.

---

## xp_funcs

Reads fields out of serialized game objects. Registers as **`FUNCS`**.

Use `nwnx_funcs.nss` from this directory.

```nwscript
int GetCreatureSoundSet(object oCreature);                                  // as NWNX4
int GetCreatureFieldInt(object oCreature, string sLabel, int nDefault = 0); // new
```

`GetCreatureSoundSet` has NWNX4's signature and meaning. `GetCreatureFieldInt` returns any integer
field by its GFF label — `Appearance_Type`, `Gender`, `FactionID`, and so on. The labels are the
ones in the `.UTC` blueprint, which the toolset or any GFF editor will show.

### Changes from NWNX4

**No engine addresses.** The original found the creature through a hardcoded `CAppManager`
pointer and `CServerExoApp::GetCreatureByGameObjectID`, then called `CNWSCreature::GetSoundSet`.
That middle function cannot be resolved by signature on x64 — it is one of 36 byte-identical
siblings generated from the same template, differing only in a RIP-relative displacement.

None of it is necessary. `SoundSetFile` is a field in the creature's own serialized form, so this
port asks the engine to serialize the creature through SCORCO and reads the field out of the GFF.
That works identically on the GOG and Steam builds and cannot be moved by a future patch, because
GFF is the module file format.

It is also closer to the truth than the original: SCORCO serializes live state rather than the
blueprint, so anything that changed a creature's soundset at runtime is reflected.

Verified on both an NPC and a player character — NWN2 will serialize a PC through SCORCO, which
NWN1 refused to do.

### Cost

A member read became a serialization of the whole creature — tens of KB per call, measured at 35 KB
for an NPC and 41 KB for a player. That is irrelevant for the occasional soundset lookup and would
hurt badly in a loop over every creature in an area. **Cache the result** if you need it repeatedly
for the same creature.

### Limits

Only types stored inline in the GFF field entry can be read: the integer types up to 32 bits.
Strings, floats and nested structs keep an *offset* in that slot instead, so returning it as a
value would be nonsense — those log the type and return nothing rather than guessing.

---

## xp_mysql

SQL from NWScript, backed by MySQL or MariaDB. Registers as **`SQL`** by default, so it and
`xp_sqlite` cannot both be loaded unless one has its `class` changed.

**Not yet run against MySQL itself.** The C API is the same and the schema restored identically,
but that validation is still outstanding.

### Building

This is the only port with a prerequisite, and it is skipped rather than fatal when absent, so the
other seven still build with no setup:

```bash
vcpkg install libmariadb:x64-windows-static
cmake -S plugins -B plugins/build -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
```

Or point `-DMARIADB_CONNECTOR_ROOT=<prefix>` at any existing connector. NWNX4 uses the same
library via vcpkg, so an NWNX4 build tree already has one.

Building the connector from source was tried and abandoned: its bundled zlib predates CMake 4, and
several sources include `windows.h` ahead of `winsock2.h`, which clang-cl rejects. vcpkg carries
patches for both.

### Configuration

See the shipped `xp_mysql.yml`. `server` defaults to `localhost`, matching NWNX4.

### Changes from NWNX4

**Objects are bound as parameters rather than escaped into the statement.** NWNX4 built the SCO
statement with `sprintf(pSQL, scorcoSQL.c_str(), Data)`, which meant the script's statement was
used as a printf format string, the buffer could overflow, and the object went in as a quoted
literal escaped against the connection charset. The last of those is what ties modules using
SCORCO to a permissive charset such as latin1. This port uses `MYSQL_TYPE_LONG_BLOB`, so none of
it applies, and the script's `%s` is rewritten to a parameter marker so no module script changes.

**`FETCHMODE` no longer frees the result set it is reading.** The original freed it and left the
member dangling for the next `Fetch` or `GetData`.

**`Disconnect` clears the connection pointer,** so the "not connected" guards actually guard.

**Reconnect also triggers on `CR_SERVER_LOST` (2013), not only `CR_SERVER_GONE_ERROR` (2006).**
2006 is the connection found dead *between* queries; 2013 is it dying *during* one, which is what
a `KILL`, a network blip or a server restart produces. NWNX4 retried only the first, so the second
left the plugin permanently disconnected until the game server was restarted.

Verified by having the connection kill itself and checking the connection id changed:

```
Executing: KILL 94
SQL error: Lost connection to server during query (2013)
reconnecting...
```

**`CLIENT_MULTI_STATEMENTS` is opt-in.** NWNX4 had it on always, which turns one injected
semicolon into a second statement the server will run.

### Silent-corruption reporting

Stored objects that go wrong tend to fail quietly: in game an item simply does not come back. This
port logs the byte count of every store and retrieve, warns when an object exceeds 65535 bytes
(a `BLOB` column truncates past that outside strict `sql_mode`), reports server warnings raised by
a store, and checks that retrieved data is structurally valid GFF.

---

## xp_profiler

Times every script the server runs and periodically logs per-script totals. Registers as
**`PROFILER`**. It has no script-facing functions beyond the generic queries - it watches, and
writes to its own log.

```
script statistics, last 10489 ms
-------------------------------------------------------
nw_c2_default9          257 us        1 calls       257 us/call *
nw_c2_default1          157 us        2 calls        78 us/call *
x2_mod_def_aqu           74 us        5 calls        14 us/call *
-------------------------------------------------------
```

Busiest first, with a `*` marking anything that ran since the previous report. See the shipped
`xp_profiler.yml`: `log_level` 0 disables the hook entirely, 1 gives the periodic table, 2 adds a
line per script call - useful for finding what a specific event runs, not for leaving on.

### Changes from NWNX4

**No assembly.** The original needed a `__declspec(naked)` trampoline because 32-bit `thiscall` put
`this` in `ECX` with arguments on the stack, which C could not express. On x64 `__fastcall` *is* the
convention, so the hook is an ordinary function and the return value of a `StartingConditional`
script is preserved by the language rather than by hand.

**No memory scan.** NWNX4 searched `0x400000`-`0x800000` for a hardcoded byte sequence. This uses a
byte pattern through the loader's `IAddressService`, verified unique in two different Patch 3
server builds at different addresses in each.

**`std::unordered_map` instead of a hand-rolled hash table**, removing 328 lines.

**The script name is validated before use.** It is read from `CVirtualMachine::m_sLastScriptRun` at
a fixed offset, which is only known to be correct for the builds this was developed against. On a
build where that offset is wrong, reading it blindly would hand back garbage or fault the server,
so the plugin checks the memory is committed and readable, that the length is plausible, and that
the text is printable. If any of that fails it disables itself with an explanation and leaves the
server running.

---

## xp_pickpocket

Lets a module script allow or deny a pickpocket attempt. Registers as **`PICK`**.

Set `script` in `xp_pickpocket.yml` to a module script. It runs on every attempt with `OBJECT_SELF`
as the creature doing the pickpocketing, and uses `nwnx_pickpocket.nss`:

```nwscript
#include "nwnx_pickpocket"

void main()
{
    object oTarget = PickpocketGetTarget();
    if (GetIsPC(oTarget)) { PickpocketCancel(); }
}
```

Nothing is hooked until `script` is set, and the log says so rather than failing quietly.


### Changes from NWNX4

Three hardcoded 32-bit addresses became one byte pattern - the loader already provides RunScript,
so only the pickpocket action itself has to be found. The target is read from the engine's actual
structure layout rather than from a hand-reconstructed struct, and every read is bounds-checked, so a
wrong offset on an unverified build fails rather than faulting the server.

---

## xp_objectattributes

Changes a creature's appearance at runtime. Registers as **`OBJECTATTRIBUTES`**.

Use `nwnx_objectattributes.nss`. The object id goes in the fourth argument and the value as the
string:

```nwscript
NWNXSetString("OBJECTATTRIBUTES", "SetHeadVariation", "", ObjectToInt(oCreature), "3");
NWNXSetString("OBJECTATTRIBUTES", "SetBodyTint", "", ObjectToInt(oCreature),
              "NWN2_TintSet[0xRRGGBBAA, 0xRRGGBBAA, 0xRRGGBBAA]");
```

| Function | Value |
| --- | --- |
| `SetHeadVariation`, `SetHairVariation`, `SetTailVariation`, `SetWingVariation`, `SetFacialHairVariation` | a number |
| `SetBodyTint`, `SetHeadTint`, `SetHairTint` | `NWN2_TintSet[0xRRGGBBAA, 0xRRGGBBAA, 0xRRGGBBAA]` |
| `SetRace` | a racial type |

### Changes from NWNX4

The original reached creature fields through a hand-reconstructed 32-bit struct. Every offset here is 
named in the source so it can be checked. Each setter writes the creature's own copy and the stats copy, 
as the original did.

Finding a creature from an object id used to need a hardcoded app-manager pointer. It now resolves
the engine's own accessor, which is in `common/EngineAccess.h` and reusable by other plugins.

Every write is bounds-checked first and reports failure rather than guessing: the offsets are only
known to hold for the builds this was developed against.