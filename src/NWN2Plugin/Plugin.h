#pragma once
#include <cstdint>
#include <cstddef>

class IPlugin;

/// <summary>
/// NWScript object ID constants a plugin may need when calling <see cref="IPluginHost::RunScript"/>.
/// </summary>
class NWScriptObject
{
public:
    /// <summary>Matches NWScript's own <c>OBJECT_INVALID</c>: no object / not found.</summary>
    static constexpr uint32_t OBJECT_INVALID = 0x7F000000;
};

/// <summary>
/// NWN2's own chat channel/mode byte values, passed to <see cref="ChatHookFunc"/> as <c>mode</c>.
/// </summary>
/// <remarks>
/// Reverse-engineered from <c>CNWSMessage::SendServerToPlayerChatMessage</c> and the NWScript
/// command handlers that call it. <see cref="DM_FLAG"/> is a real bit, not a separate concept per
/// channel: the engine computes a DM (green text) message by OR-ing it onto <see cref="TALK"/>,
/// <see cref="SHOUT"/>, <see cref="WHISPER"/>, or <see cref="TELL"/> whenever the recipient
/// resolves to a DM, so e.g. DM Talk arrives as <c>TALK | DM_FLAG</c>. <see cref="SERVER_TELL"/>
/// and the silent-talk/shout modes have no DM variant.
/// </remarks>
class ChatMode
{
public:
    /// <summary>Normal talk range.</summary>
    static constexpr uint8_t TALK = 1;
    /// <summary>Shout range.</summary>
    static constexpr uint8_t SHOUT = 2;
    /// <summary>Whisper range.</summary>
    static constexpr uint8_t WHISPER = 3;
    /// <summary>A tell sent to one specific player (see <c>targetId</c>).</summary>
    static constexpr uint8_t TELL = 4;
    /// <summary>A server-originated tell, matching NWScript's own <c>SendMessageToPC</c>-style server messages.</summary>
    static constexpr uint8_t SERVER_TELL = 5;
    /// <summary>Party chat.</summary>
    static constexpr uint8_t PARTY = 6;
    /// <summary>NWScript's <c>TALKVOLUME_SILENT_TALK</c>: a talk-range message with no chat-window text, only the in-world speech bubble.</summary>
    static constexpr uint8_t SILENT_TALK = 0xD;
    /// <summary>NWScript's <c>TALKVOLUME_SILENT_SHOUT</c>: a shout-range message with no chat-window text, only the in-world speech bubble.</summary>
    static constexpr uint8_t SILENT_SHOUT = 0xE;
    /// <summary>Faction/associate chat (<c>CNWSFaction::SendChatMessage</c>). Numerically the same value as <c>PARTY | DM_FLAG</c>, but reached independently of it.</summary>
    static constexpr uint8_t FACTION = 0x16;
    /// <summary>OR this onto <see cref="TALK"/>/<see cref="SHOUT"/>/<see cref="WHISPER"/>/<see cref="TELL"/> to get that channel's DM (green text) variant.</summary>
    static constexpr uint8_t DM_FLAG = 0x10;
};

/// <summary>
/// A plugin's chat interception callback - see <see cref="IPluginHost::RegisterChatHook"/>.
/// </summary>
/// <param name="mode">NWN2's own chat channel byte - see <see cref="ChatMode"/> for known values.</param>
/// <param name="senderId">The speaking object's ID, or <see cref="NWScriptObject::OBJECT_INVALID"/> for server-originated messages.</param>
/// <param name="message">The chat text.</param>
/// <param name="targetId">
/// The tell/whisper recipient's object ID. Meaningless for channels that broadcast instead of
/// targeting one object.
/// </param>
/// <returns>
/// <see langword="true"/> to suppress the message entirely - NWN2 never sends it to anyone, and
/// never runs its own chat-related scripts for it; <see langword="false"/> to let it through
/// unchanged.
/// </returns>
typedef bool (*ChatHookFunc)(uint8_t mode, uint32_t senderId, const char* message, uint32_t targetId);

/// <summary>
/// The host API a plugin is given access to, e.g. to look up other loaded plugins.
/// </summary>
/// <remarks>
/// This is a vtable-based ABI, exactly like <see cref="IBinaryDataResult"/>/<see cref="IStringResult"/>:
/// a plugin calls through this interface without ever needing to know how the real host class
/// (NWN2ModLoader's internal <c>NWN2Mod</c>) is laid out, what it links against, or how its other
/// members are declared. Treat it as a stable contract and only ever append new methods at the end.
/// </remarks>
class IPluginHost
{
public:
    virtual ~IPluginHost() = default;

    /// <summary>
    /// Looks up another currently loaded plugin by its <see cref="IPlugin::GetPluginId"/>.
    /// </summary>
    /// <param name="id">The plugin ID to look up.</param>
    /// <returns>The matching plugin instance, or <see langword="nullptr"/> if none is loaded with that ID.</returns>
    virtual IPlugin* GetPlugin(const char* id) const = 0;

    /// <summary>
    /// Runs a compiled script immediately, exactly as NWScript's own <c>ExecuteScript(sScript, oTarget)</c> would.
    /// </summary>
    /// <param name="script">The script's resref (its filename without the <c>.ncs</c> extension).</param>
    /// <param name="objectId">
    /// The NWN object ID the script runs against - this is what <c>OBJECT_SELF</c> resolves to
    /// inside it. Use <see cref="NWScriptObject::OBJECT_INVALID"/> for a script that doesn't need
    /// a valid target.
    /// </param>
    /// <returns>
    /// <see langword="true"/> if the script ran; <see langword="false"/> if it could not be found/compiled,
    /// or if the engine's script virtual machine is not ready yet.
    /// </returns>
    /// <remarks>
    /// The target script must be a bare <c>void main()</c> - passing extra script parameters (as
    /// NWScript's <c>ExecuteScriptEx</c> allows) is not supported.
    /// </remarks>
    virtual bool RunScript(const char* script, uint32_t objectId) const = 0;

    /// <summary>
    /// Registers <paramref name="hook"/> as the chat interceptor, called for every chat message
    /// before NWN2 sends it anywhere.
    /// </summary>
    /// <param name="hook">
    /// The new hook, or <see langword="nullptr"/> to stop intercepting chat and let it behave
    /// exactly as if no plugin had ever registered one.
    /// </param>
    /// <returns>
    /// Whatever hook was previously registered, or <see langword="nullptr"/> if none was.
    /// </returns>
    /// <remarks>
    /// Only one hook is ever active - registering a new one replaces the last outright. A plugin
    /// that wants to add to an existing hook rather than silently drop it should hold onto the
    /// returned value and call it itself (typically when it decides not to suppress a message on
    /// its own), the same way <c>DestroyPlugin</c> chains, and unregister its own hook by
    /// re-registering whatever it was given back, so this always stays a well-formed chain
    /// regardless of load/unload order.
    /// </remarks>
    virtual ChatHookFunc RegisterChatHook(ChatHookFunc hook) = 0;
};

/// <summary>
/// Produces the writable buffer a plugin fills in from <see cref="IPlugin::OnGetBinaryData"/>.
/// </summary>
/// <remarks>
/// The host owns the buffer this interface hands out; a plugin never allocates or frees it
/// directly. Leaving a result untouched (never calling <see cref="Allocate"/>) reports "no data"
/// for the requested variable, exactly as if the call had never been claimed.
/// </remarks>
class IBinaryDataResult
{
public:
    virtual ~IBinaryDataResult() = default;

    /// <summary>
    /// Allocates a writable buffer of the given size and marks this result as handled.
    /// </summary>
    /// <param name="size">The number of bytes to allocate.</param>
    /// <returns>A pointer to a writable buffer of exactly <paramref name="size"/> bytes.</returns>
    /// <remarks>
    /// Calling this more than once for the same call replaces the previously allocated buffer.
    /// </remarks>
    virtual uint8_t* Allocate(size_t size) = 0;

    /// <summary>
    /// Discards any buffer obtained from <see cref="Allocate"/> and marks this result "empty"
    /// again, as if <see cref="Allocate"/> had never been called.
    /// </summary>
    virtual void Clear() = 0;
};

/// <summary>
/// Produces the string value a plugin sets from <see cref="IPlugin::OnNWNXGetString"/>.
/// </summary>
/// <remarks>
/// The host owns the storage behind this interface; a plugin never allocates, frees, or
/// null-terminates it directly. Leaving a result untouched reports "no value".
/// </remarks>
class IStringResult
{
public:
    virtual ~IStringResult() = default;

    /// <summary>
    /// Allocates a writable buffer of the given length and marks this result as handled.
    /// </summary>
    /// <param name="length">The number of characters to allocate, not including the null terminator.</param>
    /// <returns>
    /// A pointer to a writable buffer of exactly <paramref name="length"/> characters. The host
    /// guarantees a null terminator immediately follows the buffer; the caller never writes it.
    /// </returns>
    /// <remarks>
    /// Calling this more than once for the same call replaces the previously allocated buffer.
    /// </remarks>
    virtual char* Allocate(size_t length) = 0;

    /// <summary>
    /// Sets this result to a copy of the given null-terminated string and marks it as handled.
    /// </summary>
    /// <param name="value">The null-terminated string to copy.</param>
    virtual void Set(const char* value) = 0;

    /// <summary>
    /// Discards any value obtained from <see cref="Allocate"/> or <see cref="Set"/> and marks
    /// this result "empty" again, as if neither had been called.
    /// </summary>
    virtual void Clear() = 0;
};

/// <summary>
/// The interface every NWN2ServerMod plugin implements.
/// </summary>
/// <remarks>
/// <para>
/// A plugin DLL exports two functions with C linkage:
/// <code>
/// extern "C" __declspec(dllexport) IPlugin* CreatePlugin(IPluginHost* host);
/// extern "C" __declspec(dllexport) void DestroyPlugin(IPlugin* plugin);
/// </code>
/// The exported <c>CreatePlugin</c> function (see <see cref="CreatePluginFunc"/>) is called once
/// at load time with an <see cref="IPluginHost"/>, so the plugin can use its API (e.g. looking up
/// other plugins). The exported <c>DestroyPlugin</c> function (see
/// <see cref="DestroyPluginFunc"/>) is called on shutdown so the plugin's own module frees the
/// object it allocated.
/// </para>
/// <para>
/// Only raw pointers, <c>size_t</c>, primitives, and small virtual interfaces (such as
/// <see cref="IBinaryDataResult"/> and <see cref="IStringResult"/>) cross this boundary - never
/// <c>std::string</c>, <c>std::vector</c>, <c>std::function</c>, or a C-style callback/context
/// pair. Those STL types are not ABI-stable across differing compiler/CRT/iterator-debug-level
/// settings, so a plugin built with mismatched settings could silently corrupt memory instead of
/// failing to link. A plugin must be built with the same compiler toolset as NWN2ModLoader.dll so
/// this class's vtable layout and calling convention match; nothing beyond that is required.
/// </para>
/// <para>
/// This is a vtable-based ABI: treat it as a stable contract and only ever append new methods at
/// the end. Every method besides <see cref="GetPluginId"/> has a no-op default implementation, so
/// a plugin only needs to override what it actually uses.
/// </para>
/// </remarks>
class IPlugin
{
public:
    virtual ~IPlugin() = default;

    /// <summary>
    /// Gets a short, stable identifier for this plugin (e.g. "MyPlugin").
    /// </summary>
    /// <returns>
    /// A pointer to a null-terminated string that remains valid for the plugin's whole lifetime.
    /// </returns>
    /// <remarks>
    /// This is used to route calls to the plugin: <c>NWNX*</c> script calls carry it explicitly
    /// as their "plugin" argument, and <c>StoreCampaignObject</c>/<c>RetrieveCampaignObject</c>
    /// calls carry it as their campaign name argument (see <see cref="OnSetBinaryData"/> and
    /// <see cref="OnGetBinaryData"/>). It must be unique among all plugins loaded at once.
    /// </remarks>
    virtual const char* GetPluginId() const = 0;

    /// <summary>
    /// Called for a <c>StoreCampaignObject(sCampaignName, sVarName, oObject, oPlayer)</c> write
    /// whose campaign name matches this plugin's ID.
    /// </summary>
    /// <param name="varName">The variable name the script passed, unmodified.</param>
    /// <param name="player">
    /// Not the raw <c>oPlayer</c> object - the engine resolves it to a 0-32 character identity
    /// string before calling this, built as the concatenation, with no separator, of:
    /// <list type="number">
    /// <item>up to the first 16 characters of the connected player's custom name if they set one
    /// in-game, otherwise their raw platform (Steam/GOG) display name;</item>
    /// <item>up to the first 16 characters of the full name of the creature <c>oPlayer</c> is
    /// currently controlling.</item>
    /// </list>
    /// Each piece is truncated, never padded, so either or both may be shorter than 16 characters.
    /// This is an empty string if <c>oPlayer</c> does not resolve to a currently connected client
    /// controlling a valid creature.
    /// </param>
    /// <param name="data">A pointer to <paramref name="size"/> bytes of serialized object data.</param>
    /// <param name="size">The number of bytes available at <paramref name="data"/>.</param>
    /// <returns><see langword="true"/> if the data was stored; otherwise <see langword="false"/>.</returns>
    /// <remarks>
    /// NWN2ServerMod never calls the engine's own <c>SetBinaryData</c>; store the bytes however
    /// you like (your own database, a file, memory). The return value becomes
    /// <c>StoreCampaignObject</c>'s own return value as seen by the calling script.
    /// </remarks>
    virtual bool OnSetBinaryData(const char* varName, const char* player,
        const uint8_t* data, size_t size)
    {
        return false;
    }

    /// <summary>
    /// Called for a <c>RetrieveCampaignObject(sCampaignName, sVarName, ...)</c> read whose
    /// campaign name matches this plugin's ID.
    /// </summary>
    /// <param name="varName">The variable name the script passed, unmodified.</param>
    /// <param name="player">
    /// Not the raw <c>oPlayer</c> object - the engine resolves it to a 0-32 character identity
    /// string before calling this, built as the concatenation, with no separator, of:
    /// <list type="number">
    /// <item>up to the first 16 characters of the connected player's custom name if they set one
    /// in-game, otherwise their raw platform (Steam/GOG) display name;</item>
    /// <item>up to the first 16 characters of the full name of the creature <c>oPlayer</c> is
    /// currently controlling.</item>
    /// </list>
    /// Each piece is truncated, never padded, so either or both may be shorter than 16 characters.
    /// This is an empty string if <c>oPlayer</c> does not resolve to a currently connected client
    /// controlling a valid creature.
    /// </param>
    /// <param name="result">
    /// Filled in with the requested data, if any is available. Left untouched to report "no data"
    /// for this variable.
    /// </param>
    /// <remarks>
    /// NWN2ServerMod never calls the engine's own <c>GetBinaryData</c>. "No data" is reported to
    /// the engine as a null <c>shared_ptr</c>, matching the value the real engine uses for
    /// "variable not found", which the engine already handles correctly on its own.
    /// </remarks>
    virtual void OnGetBinaryData(const char* varName, const char* player,
        IBinaryDataResult& result)
    {
    }

    /// <summary>
    /// Called when a script calls <c>NWNXSetString</c> with this plugin's ID.
    /// </summary>
    /// <param name="function">The function name the script passed.</param>
    /// <param name="param1">The first parameter the script passed.</param>
    /// <param name="param2">The second parameter the script passed.</param>
    /// <param name="value">The string value the script passed.</param>
    virtual void OnNWNXSetString(const char* function, const char* param1, int param2, const char* value) {}

    /// <summary>
    /// Called when a script calls <c>NWNXSetInt</c> with this plugin's ID.
    /// </summary>
    /// <param name="function">The function name the script passed.</param>
    /// <param name="param1">The first parameter the script passed.</param>
    /// <param name="param2">The second parameter the script passed.</param>
    /// <param name="value">The integer value the script passed.</param>
    virtual void OnNWNXSetInt(const char* function, const char* param1, int param2, int value) {}

    /// <summary>
    /// Called when a script calls <c>NWNXSetFloat</c> with this plugin's ID.
    /// </summary>
    /// <param name="function">The function name the script passed.</param>
    /// <param name="param1">The first parameter the script passed.</param>
    /// <param name="param2">The second parameter the script passed.</param>
    /// <param name="value">The floating-point value the script passed.</param>
    virtual void OnNWNXSetFloat(const char* function, const char* param1, int param2, float value) {}

    /// <summary>
    /// Called when a script calls <c>NWNXGetString</c> with this plugin's ID.
    /// </summary>
    /// <param name="function">The function name the script passed.</param>
    /// <param name="param1">The first parameter the script passed.</param>
    /// <param name="param2">The second parameter the script passed.</param>
    /// <param name="result">
    /// Filled in with the requested value, if any is available. Left untouched to report "no
    /// value".
    /// </param>
    virtual void OnNWNXGetString(const char* function, const char* param1, int param2, IStringResult& result) {}

    /// <summary>
    /// Called when a script calls <c>NWNXGetInt</c> with this plugin's ID.
    /// </summary>
    /// <param name="function">The function name the script passed.</param>
    /// <param name="param1">The first parameter the script passed.</param>
    /// <param name="param2">The second parameter the script passed.</param>
    /// <param name="outValue">Set to the requested value if this plugin has one.</param>
    /// <returns>
    /// <see langword="true"/> if <paramref name="outValue"/> was set; otherwise <see langword="false"/>.
    /// </returns>
    virtual bool OnNWNXGetInt(const char* function, const char* param1, int param2, int& outValue) { return false; }

    /// <summary>
    /// Called when a script calls <c>NWNXGetFloat</c> with this plugin's ID.
    /// </summary>
    /// <param name="function">The function name the script passed.</param>
    /// <param name="param1">The first parameter the script passed.</param>
    /// <param name="param2">The second parameter the script passed.</param>
    /// <param name="outValue">Set to the requested value if this plugin has one.</param>
    /// <returns>
    /// <see langword="true"/> if <paramref name="outValue"/> was set; otherwise <see langword="false"/>.
    /// </returns>
    virtual bool OnNWNXGetFloat(const char* function, const char* param1, int param2, float& outValue) { return false; }

    /// <summary>
    /// Called once per plugin after every plugin listed in the config has finished loading (each
    /// one's <c>CreatePlugin</c> has already returned successfully).
    /// </summary>
    /// <param name="host">The same <see cref="IPluginHost"/> instance passed to <c>CreatePlugin</c>.</param>
    /// <remarks>
    /// Unlike inside <c>CreatePlugin</c> itself - where a plugin listed later in the config
    /// wouldn't have loaded yet - every plugin is guaranteed to exist by the time any plugin's
    /// <c>OnInitialize</c> runs. This makes it the right place to look up other plugins with
    /// <see cref="IPluginHost::GetPlugin"/> and interact with them, if needed.
    /// </remarks>
    virtual void OnInitialize(IPluginHost* host) {}
};

/// <summary>
/// The signature a plugin DLL's exported <c>CreatePlugin</c> function must have.
/// </summary>
/// <param name="host">The host API the plugin can use, e.g. to look up other plugins.</param>
/// <returns>A new plugin instance, or <see langword="nullptr"/> on failure.</returns>
typedef IPlugin* (__cdecl* CreatePluginFunc)(IPluginHost* host);

/// <summary>
/// The signature a plugin DLL's exported <c>DestroyPlugin</c> function must have.
/// </summary>
/// <param name="plugin">The plugin instance to destroy, previously returned by <c>CreatePlugin</c>.</param>
typedef void (__cdecl* DestroyPluginFunc)(IPlugin* plugin);
