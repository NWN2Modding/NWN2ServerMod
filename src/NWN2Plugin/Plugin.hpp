#pragma once
// The C++ side of the plugin SDK.
//
// Derive from nwn2::PluginBase, override what you need, and export the class with
// NWN2_EXPORT_PLUGIN. The loader only ever sees the C struct in PluginAbi.h; the thunks in
// nwn2::detail bridge the two and stop exceptions at the DLL boundary.
#include "PluginAbi.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <type_traits>

namespace nwn2
{
    /// NWScript object IDs a plugin might need.
    struct NWScriptObject
    {
        /// NWScript's OBJECT_INVALID: no object, or not found.
        static constexpr uint32_t OBJECT_INVALID = NWN2_OBJECT_INVALID;
    };

    /// Chat channels, as passed to a chat hook. DM_FLAG is a bit the engine ORs onto TALK, SHOUT,
    /// WHISPER or TELL when the recipient is a DM.
    struct ChatMode
    {
        static constexpr uint8_t TALK         = NWN2_CHAT_TALK;
        static constexpr uint8_t SHOUT        = NWN2_CHAT_SHOUT;
        static constexpr uint8_t WHISPER      = NWN2_CHAT_WHISPER;
        static constexpr uint8_t TELL         = NWN2_CHAT_TELL;
        static constexpr uint8_t SERVER_TELL  = NWN2_CHAT_SERVER_TELL;
        static constexpr uint8_t PARTY        = NWN2_CHAT_PARTY;
        static constexpr uint8_t SILENT_TALK  = NWN2_CHAT_SILENT_TALK;
        static constexpr uint8_t SILENT_SHOUT = NWN2_CHAT_SILENT_SHOUT;
        static constexpr uint8_t FACTION      = NWN2_CHAT_FACTION;
        static constexpr uint8_t DM_FLAG      = NWN2_CHAT_DM_FLAG;
    };

    /// Success.
    constexpr NWN2_Result Ok() noexcept { return { 0, nullptr }; }

    /// Failure. The message must outlive the call - a literal, or storage the callee owns.
    constexpr NWN2_Result Fail(int32_t code, const char* message = nullptr) noexcept { return { code, message }; }

    /// True if the result reports success.
    constexpr bool Succeeded(const NWN2_Result& result) noexcept { return result.code == 0; }

    /// The loader, as seen by a plugin. Copyable, and owns nothing.
    ///
    /// Every call checks structSize first, so a plugin built against a newer header than the
    /// loader gets a clean failure instead of reading past the end of the struct.
    class PluginHost
    {
    public:
        PluginHost() noexcept = default;
        explicit PluginHost(const NWN2_PluginHost* host) noexcept : _host(host) {}

        /// True if this wraps a loader at all.
        explicit operator bool() const noexcept { return _host != nullptr; }

        /// The underlying struct, for anything this wrapper does not cover.
        const NWN2_PluginHost* Raw() const noexcept { return _host; }

        /// Another loaded plugin by its ID, or null. For this plugin it returns &Abi().
        ///
        /// Call the result through its own function pointers. Never cast its self to PluginBase:
        /// it belongs to another DLL, possibly built by another compiler, with its own heap.
        NWN2_Plugin* GetPlugin(const char* id) const noexcept
        {
            return NWN2_HAS_FIELD(_host, NWN2_PluginHost, GetPlugin) && _host->GetPlugin
                ? _host->GetPlugin(_host->self, id)
                : nullptr;
        }

        /// Runs a compiled script, like NWScript's ExecuteScript(script, objectId). objectId is
        /// what OBJECT_SELF resolves to inside it.
        NWN2_Result RunScript(const char* script, uint32_t objectId) const noexcept
        {
            return NWN2_HAS_FIELD(_host, NWN2_PluginHost, RunScript) && _host->RunScript
                ? _host->RunScript(_host->self, script, objectId)
                : Fail(NWN2_E_UNSUPPORTED, "RunScript is not provided by this loader");
        }

        /// Intercepts every chat message before NWN2 sends it. Returning true from the hook
        /// swallows the message.
        ///
        /// Only one hook is active at a time. Keep the hook returned here and call it when you do
        /// not swallow a message, so plugins that registered earlier still see it.
        NWN2_ChatHookFunc RegisterChatHook(NWN2_ChatHookFunc hook) const noexcept
        {
            return NWN2_HAS_FIELD(_host, NWN2_PluginHost, RegisterChatHook) && _host->RegisterChatHook
                ? _host->RegisterChatHook(_host->self, hook)
                : nullptr;
        }

        /// A loader service, or null if this loader does not have it. T needs a
        /// static constexpr const char* kName, such as "IHookService/1".
        template <typename T>
        T* QueryService() const noexcept
        {
            return NWN2_HAS_FIELD(_host, NWN2_PluginHost, QueryService) && _host->QueryService
                ? static_cast<T*>(_host->QueryService(_host->self, T::kName))
                : nullptr;
        }

    private:
        const NWN2_PluginHost* _host = nullptr;
    };

    /// Where OnGetBinaryData writes its data. The loader owns the buffer, and leaving this
    /// untouched reports "no data".
    class BinaryDataResult
    {
    public:
        explicit BinaryDataResult(NWN2_BinarySink* sink) noexcept : _sink(sink) {}

        /// A writable buffer of exactly size bytes, or null if it could not be allocated.
        /// Calling this again replaces the previous buffer.
        uint8_t* Allocate(size_t size) const noexcept
        {
            return NWN2_HAS_FIELD(_sink, NWN2_BinarySink, Allocate) && _sink->Allocate
                ? _sink->Allocate(_sink->self, size)
                : nullptr;
        }

        /// Throws away anything written so far, back to "no data".
        void Clear() const noexcept
        {
            if (NWN2_HAS_FIELD(_sink, NWN2_BinarySink, Clear) && _sink->Clear)
            {
                _sink->Clear(_sink->self);
            }
        }

    private:
        NWN2_BinarySink* _sink;
    };

    /// Where OnNWNXGetString writes its value. The loader owns the storage, and leaving this
    /// untouched reports "no value".
    class StringResult
    {
    public:
        explicit StringResult(NWN2_StringSink* sink) noexcept : _sink(sink) {}

        /// A writable buffer of exactly length characters, or null if it could not be allocated.
        /// The loader adds the terminator, so length excludes it.
        char* Allocate(size_t length) const noexcept
        {
            return NWN2_HAS_FIELD(_sink, NWN2_StringSink, Allocate) && _sink->Allocate
                ? _sink->Allocate(_sink->self, length)
                : nullptr;
        }

        /// Copies a null-terminated string in. Easier than Allocate when you already have one.
        void Set(const char* value) const noexcept
        {
            if (NWN2_HAS_FIELD(_sink, NWN2_StringSink, Set) && _sink->Set)
            {
                _sink->Set(_sink->self, value);
            }
        }

        /// Throws away anything written so far, back to "no value".
        void Clear() const noexcept
        {
            if (NWN2_HAS_FIELD(_sink, NWN2_StringSink, Clear) && _sink->Clear)
            {
                _sink->Clear(_sink->self);
            }
        }

    private:
        NWN2_StringSink* _sink;
    };

    /// Base class for a C++ plugin. Override GetPluginId and whatever else you use; the rest have
    /// no-op defaults. Export the class once, at file scope:
    ///
    ///     NWN2_EXPORT_PLUGIN(MyPlugin)
    ///
    /// Callbacks may throw. The exception is caught before it reaches the loader, reported to
    /// OnUnhandledException, and the call is treated as unhandled.
    class PluginBase
    {
    public:
        PluginBase() noexcept;
        virtual ~PluginBase() = default;

        // The C struct points at this object, so it must not be copied or moved.
        PluginBase(const PluginBase&) = delete;
        PluginBase& operator=(const PluginBase&) = delete;

        /// Short, stable, and unique among loaded plugins. Scripts use it to pick this plugin, and
        /// it must stay valid for the plugin's lifetime.
        virtual const char* GetPluginId() const = 0;

        /// Called once, after every plugin has loaded, so looking up other plugins here is safe.
        virtual void OnInitialize(PluginHost) {}

        /// A script's StoreCampaignObject with this plugin's ID as the campaign name. Return true
        /// if the data was stored.
        virtual bool OnSetBinaryData(const char* /*varName*/, const char* /*player*/,
                                     const uint8_t* /*data*/, size_t /*size*/) { return false; }

        /// A script's RetrieveCampaignObject. Leave result untouched for "no data".
        virtual void OnGetBinaryData(const char* /*varName*/, const char* /*player*/,
                                     BinaryDataResult& /*result*/) {}

        /// A script's NWNXSetString with this plugin's ID.
        virtual void OnNWNXSetString(const char* /*function*/, const char* /*param1*/,
                                     int32_t /*param2*/, const char* /*value*/) {}

        /// A script's NWNXSetInt with this plugin's ID.
        virtual void OnNWNXSetInt(const char* /*function*/, const char* /*param1*/,
                                  int32_t /*param2*/, int32_t /*value*/) {}

        /// A script's NWNXSetFloat with this plugin's ID.
        virtual void OnNWNXSetFloat(const char* /*function*/, const char* /*param1*/,
                                    int32_t /*param2*/, float /*value*/) {}

        /// A script's NWNXGetString. Leave result untouched for "no value".
        virtual void OnNWNXGetString(const char* /*function*/, const char* /*param1*/,
                                     int32_t /*param2*/, StringResult& /*result*/) {}

        /// A script's NWNXGetInt. Return true if outValue was set; the script cannot tell a false
        /// from a genuine 0.
        virtual bool OnNWNXGetInt(const char* /*function*/, const char* /*param1*/,
                                  int32_t /*param2*/, int32_t& /*outValue*/) { return false; }

        /// A script's NWNXGetFloat. Return true if outValue was set.
        virtual bool OnNWNXGetFloat(const char* /*function*/, const char* /*param1*/,
                                    int32_t /*param2*/, float& /*outValue*/) { return false; }

        /// Called when one of the callbacks above throws, instead of the exception reaching the
        /// loader. Override it to log - silently swallowed errors hide bugs.
        virtual void OnUnhandledException(const char* /*callback*/, const char* /*what*/) noexcept {}

        /// This plugin's C struct: what the loader holds, and what GetPlugin returns for it.
        NWN2_Plugin& Abi() noexcept { return _abi; }

        /// This plugin's C struct: what the loader holds, and what GetPlugin returns for it.
        const NWN2_Plugin& Abi() const noexcept { return _abi; }

    private:
        NWN2_Plugin _abi;
    };

    // The C entry points behind PluginBase::Abi(). Each recovers the plugin from self, forwards to
    // the virtual, and absorbs anything thrown - which is what makes a C++ plugin safe for the
    // loader to call.
    namespace detail
    {
        inline PluginBase& Self(void* self) noexcept
        {
            return *static_cast<PluginBase*>(self);
        }

        // Only valid inside a catch block.
        inline void ReportCurrentException(PluginBase& plugin, const char* callback) noexcept
        {
            try
            {
                throw;
            }
            catch (const std::exception& e)
            {
                plugin.OnUnhandledException(callback, e.what());
            }
            catch (...)
            {
                plugin.OnUnhandledException(callback, "unknown exception");
            }
        }

        inline const char* NWN2_CALL GetPluginId(void* self) noexcept
        {
            try
            {
                // The loader copies this into a std::string, so it must never be null.
                const char* id = Self(self).GetPluginId();
                return id ? id : "";
            }
            catch (...)
            {
                ReportCurrentException(Self(self), "GetPluginId");
                return "";
            }
        }

        inline void NWN2_CALL OnInitialize(void* self, const NWN2_PluginHost* host) noexcept
        {
            try
            {
                Self(self).OnInitialize(PluginHost(host));
            }
            catch (...)
            {
                ReportCurrentException(Self(self), "OnInitialize");
            }
        }

        inline bool NWN2_CALL OnSetBinaryData(void* self, const char* varName, const char* player,
                                              const uint8_t* data, size_t size) noexcept
        {
            try
            {
                return Self(self).OnSetBinaryData(varName, player, data, size);
            }
            catch (...)
            {
                ReportCurrentException(Self(self), "OnSetBinaryData");
                return false;
            }
        }

        inline void NWN2_CALL OnGetBinaryData(void* self, const char* varName, const char* player,
                                              NWN2_BinarySink* result) noexcept
        {
            // Without a sink there is nowhere to put data, so there is nothing to do.
            if (result == nullptr)
            {
                return;
            }
            try
            {
                BinaryDataResult view(result);
                Self(self).OnGetBinaryData(varName, player, view);
            }
            catch (...)
            {
                ReportCurrentException(Self(self), "OnGetBinaryData");
            }
        }

        inline void NWN2_CALL OnNWNXSetString(void* self, const char* function, const char* param1,
                                              int32_t param2, const char* value) noexcept
        {
            try
            {
                Self(self).OnNWNXSetString(function, param1, param2, value);
            }
            catch (...)
            {
                ReportCurrentException(Self(self), "OnNWNXSetString");
            }
        }

        inline void NWN2_CALL OnNWNXSetInt(void* self, const char* function, const char* param1,
                                           int32_t param2, int32_t value) noexcept
        {
            try
            {
                Self(self).OnNWNXSetInt(function, param1, param2, value);
            }
            catch (...)
            {
                ReportCurrentException(Self(self), "OnNWNXSetInt");
            }
        }

        inline void NWN2_CALL OnNWNXSetFloat(void* self, const char* function, const char* param1,
                                             int32_t param2, float value) noexcept
        {
            try
            {
                Self(self).OnNWNXSetFloat(function, param1, param2, value);
            }
            catch (...)
            {
                ReportCurrentException(Self(self), "OnNWNXSetFloat");
            }
        }

        inline void NWN2_CALL OnNWNXGetString(void* self, const char* function, const char* param1,
                                              int32_t param2, NWN2_StringSink* result) noexcept
        {
            if (result == nullptr)
            {
                return;
            }
            try
            {
                StringResult view(result);
                Self(self).OnNWNXGetString(function, param1, param2, view);
            }
            catch (...)
            {
                ReportCurrentException(Self(self), "OnNWNXGetString");
            }
        }

        inline bool NWN2_CALL OnNWNXGetInt(void* self, const char* function, const char* param1,
                                           int32_t param2, int32_t* outValue) noexcept
        {
            if (outValue == nullptr)
            {
                return false;
            }
            try
            {
                return Self(self).OnNWNXGetInt(function, param1, param2, *outValue);
            }
            catch (...)
            {
                ReportCurrentException(Self(self), "OnNWNXGetInt");
                return false;
            }
        }

        inline bool NWN2_CALL OnNWNXGetFloat(void* self, const char* function, const char* param1,
                                             int32_t param2, float* outValue) noexcept
        {
            if (outValue == nullptr)
            {
                return false;
            }
            try
            {
                return Self(self).OnNWNXGetFloat(function, param1, param2, *outValue);
            }
            catch (...)
            {
                ReportCurrentException(Self(self), "OnNWNXGetFloat");
                return false;
            }
        }

        // Passes the host to the plugin's constructor if it takes one.
        template <typename T>
        PluginBase* Construct([[maybe_unused]] const NWN2_PluginHost* host)
        {
            static_assert(std::is_base_of_v<PluginBase, T>,
                          "NWN2_EXPORT_PLUGIN: the plugin type must derive from nwn2::PluginBase");

            if constexpr (std::is_constructible_v<T, PluginHost>)
            {
                return new T(PluginHost(host));
            }
            else
            {
                return new T();
            }
        }
    }

    inline PluginBase::PluginBase() noexcept
        : _abi{}
    {
        _abi.structSize      = sizeof(NWN2_Plugin);
        _abi.self            = this;
        _abi.GetPluginId     = &detail::GetPluginId;
        _abi.OnSetBinaryData = &detail::OnSetBinaryData;
        _abi.OnGetBinaryData = &detail::OnGetBinaryData;
        _abi.OnNWNXSetString = &detail::OnNWNXSetString;
        _abi.OnNWNXSetInt    = &detail::OnNWNXSetInt;
        _abi.OnNWNXSetFloat  = &detail::OnNWNXSetFloat;
        _abi.OnNWNXGetString = &detail::OnNWNXGetString;
        _abi.OnNWNXGetInt    = &detail::OnNWNXGetInt;
        _abi.OnNWNXGetFloat  = &detail::OnNWNXGetFloat;
        _abi.OnInitialize    = &detail::OnInitialize;
    }
}

/// Exports a plugin class as this DLL's plugin: emits GetPluginAbiVersion, CreatePlugin and
/// DestroyPlugin. Use it once, at file scope.
///
/// The type must derive from nwn2::PluginBase and be constructible either from a nwn2::PluginHost
/// or with no arguments. If its constructor throws, CreatePlugin returns null and the loader skips
/// the plugin.
#define NWN2_EXPORT_PLUGIN(TYPE)                                                        \
    NWN2_DECLARE_PLUGIN_ABI()                                                           \
                                                                                        \
    NWN2_EXTERN_C __declspec(dllexport)                                                 \
    NWN2_Plugin* NWN2_CALL CreatePlugin(const NWN2_PluginHost* host) noexcept           \
    {                                                                                   \
        try                                                                             \
        {                                                                               \
            return &::nwn2::detail::Construct<TYPE>(host)->Abi();                       \
        }                                                                               \
        catch (...)                                                                     \
        {                                                                               \
            return nullptr;                                                             \
        }                                                                               \
    }                                                                                   \
                                                                                        \
    NWN2_EXTERN_C __declspec(dllexport)                                                 \
    void NWN2_CALL DestroyPlugin(NWN2_Plugin* plugin) noexcept                          \
    {                                                                                   \
        if (plugin != nullptr && plugin->self != nullptr)                               \
        {                                                                               \
            delete static_cast<::nwn2::PluginBase*>(plugin->self);                      \
        }                                                                               \
    }
