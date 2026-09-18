#ifndef NWN2_PLUGIN_ABI_H
#define NWN2_PLUGIN_ABI_H

/* The plugin boundary, in plain C.
 *
 * MSVC and GCC lay out C++ vtables differently, so a plugin built with a different compiler than
 * the loader could load and then call the wrong method. Structs of function pointers lay out the
 * same everywhere, so that is what crosses the boundary.
 *
 * C++ plugins should use Plugin.hpp, which wraps all of this. */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#  define NWN2_EXTERN_C extern "C"
#else
#  define NWN2_EXTERN_C
#endif

/* Bump this when an existing field changes meaning, order or type. Adding a field to the end is
   handled by structSize instead, and does not need a bump. */
#define NWN2_PLUGIN_ABI_VERSION 1u

/* x64 Windows only has one calling convention, but naming it documents the intent. */
#define NWN2_CALL __cdecl

/* True if a struct from the other side of the boundary is new enough to contain this field. Both
   sides set structSize to their own sizeof, so one built against an older header is simply
   shorter. False for a null pointer. */
#define NWN2_HAS_FIELD(ptr, Type, field) \
    ((ptr) != NULL && (ptr)->structSize >= offsetof(Type, field) + sizeof((ptr)->field))

struct NWN2_Plugin;

/* ---- Constants --------------------------------------------------------- */

#define NWN2_OBJECT_INVALID     0x7F000000u

/* Chat channels. DM_FLAG is a bit the engine ORs onto TALK, SHOUT, WHISPER or TELL when the
   recipient is a DM, so DM Talk arrives as TALK | DM_FLAG. */
#define NWN2_CHAT_TALK          1
#define NWN2_CHAT_SHOUT         2
#define NWN2_CHAT_WHISPER       3
#define NWN2_CHAT_TELL          4
#define NWN2_CHAT_SERVER_TELL   5
#define NWN2_CHAT_PARTY         6
#define NWN2_CHAT_SILENT_TALK   0x0D
#define NWN2_CHAT_SILENT_SHOUT  0x0E
#define NWN2_CHAT_FACTION       0x16
#define NWN2_CHAT_DM_FLAG       0x10

/* ---- Errors ------------------------------------------------------------ */

/* Exceptions cannot cross this boundary, so failures come back as a value. */
typedef struct NWN2_Result {
    int32_t     code;      /* 0 on success */
    const char* message;   /* owned by the callee, valid until its next call */
} NWN2_Result;

enum {
    NWN2_E_UNSUPPORTED   = 1,  /* the other side does not provide this call */
    NWN2_E_NOT_FOUND     = 2,
    NWN2_E_BAD_ARGUMENT  = 3,
    NWN2_E_VERIFY_FAILED = 4
};

/* ---- Chat hook --------------------------------------------------------- */

/* Return true to swallow the message so no player sees it, and to stop the module's OnChat event
   firing for it.

   The parameter names come from the engine's own SendServerToPlayerChatMessage. Two are easy to
   misread:

     speakerId    - the engine calls this oidSpeaker, but it is 0x7FFFFFFF for player chat, so it
                    cannot be relied on to identify who spoke.
     tellPlayerId - a player (client) index, NOT an object ID, and only meaningful for TELL. It is
                    0xFFFFFFFF for talk, shout and whisper. */
typedef bool (NWN2_CALL *NWN2_ChatHookFunc)(uint8_t mode, uint32_t speakerId,
                                           const char* message, uint32_t tellPlayerId);

/* ---- The loader, called by plugins ------------------------------------- */

typedef struct NWN2_PluginHost {
    uint32_t structSize;   /* sizeof this struct, as the loader built it */
    void*    self;         /* the loader; pass it back as the first argument */

    /* Another loaded plugin by its ID, or NULL. Call it through its own function pointers - its
       self belongs to another DLL and means nothing here. */
    struct NWN2_Plugin* (NWN2_CALL *GetPlugin)(void* self, const char* id);

    /* Runs a compiled script, like NWScript's ExecuteScript(script, objectId). */
    NWN2_Result         (NWN2_CALL *RunScript)(void* self, const char* script,
                                               uint32_t objectId);

    /* Intercepts chat on its way to players. Returns the hook registered before it, which the new
       hook should call when it does not swallow a message.

       This covers player chat (talk, shout, whisper, tell, party) and server tells. It does not
       cover NWScript's SendMessageToPC, which the engine delivers as a feedback message rather
       than chat, and a few engine paths reach clients without passing through here. */
    NWN2_ChatHookFunc   (NWN2_CALL *RegisterChatHook)(void* self,
                                                      NWN2_ChatHookFunc hook);

    /* Looks up a loader service by a versioned name such as "IHookService/1", or NULL if this
       loader does not have it. New services go here rather than growing this struct. */
    void*               (NWN2_CALL *QueryService)(void* self,
                                                  const char* versionedName);
} NWN2_PluginHost;

/* ---- Where a plugin writes its results ---------------------------------- */

/* The loader owns the buffer. Not calling Allocate means "no data". */
typedef struct NWN2_BinarySink {
    uint32_t structSize;
    void*    self;
    uint8_t* (NWN2_CALL *Allocate)(void* self, size_t size);
    void     (NWN2_CALL *Clear)(void* self);
} NWN2_BinarySink;

/* The loader owns the storage and adds the terminator, so Allocate's length excludes it.
   Touching neither Allocate nor Set means "no value". */
typedef struct NWN2_StringSink {
    uint32_t structSize;
    void*    self;
    char* (NWN2_CALL *Allocate)(void* self, size_t length);
    void  (NWN2_CALL *Set)(void* self, const char* value);
    void  (NWN2_CALL *Clear)(void* self);
} NWN2_StringSink;

/* ---- The plugin, called by the loader ----------------------------------- */

/* Any entry may be NULL, which means the plugin does not handle that call. */
typedef struct NWN2_Plugin {
    uint32_t structSize;   /* sizeof this struct, as the plugin built it */
    void*    self;         /* the plugin; pass it back as the first argument */

    /* Short, stable, and unique among loaded plugins. Scripts use it to pick this plugin. */
    const char* (NWN2_CALL *GetPluginId)(void* self);

    /* StoreCampaignObject / RetrieveCampaignObject with this plugin's ID as the campaign name.
       The loader never touches the engine's own campaign database for these. */
    bool (NWN2_CALL *OnSetBinaryData)(void* self, const char* varName,
                                      const char* player,
                                      const uint8_t* data, size_t size);
    void (NWN2_CALL *OnGetBinaryData)(void* self, const char* varName,
                                      const char* player,
                                      NWN2_BinarySink* result);

    /* NWNXSetString / NWNXSetInt / NWNXSetFloat with this plugin's ID. */
    void (NWN2_CALL *OnNWNXSetString)(void* self, const char* function,
                                      const char* param1, int32_t param2,
                                      const char* value);
    void (NWN2_CALL *OnNWNXSetInt)   (void* self, const char* function,
                                      const char* param1, int32_t param2,
                                      int32_t value);
    void (NWN2_CALL *OnNWNXSetFloat) (void* self, const char* function,
                                      const char* param1, int32_t param2,
                                      float value);

    /* NWNXGetString / NWNXGetInt / NWNXGetFloat. The Get calls return false when they have no
       value, and the script then sees 0 or an empty string. */
    void (NWN2_CALL *OnNWNXGetString)(void* self, const char* function,
                                      const char* param1, int32_t param2,
                                      NWN2_StringSink* result);
    bool (NWN2_CALL *OnNWNXGetInt)   (void* self, const char* function,
                                      const char* param1, int32_t param2,
                                      int32_t* outValue);
    bool (NWN2_CALL *OnNWNXGetFloat) (void* self, const char* function,
                                      const char* param1, int32_t param2,
                                      float* outValue);

    /* Called once, after every plugin has loaded, so looking up other plugins here is safe. */
    void (NWN2_CALL *OnInitialize)(void* self, const NWN2_PluginHost* host);
} NWN2_Plugin;

/* ---- What a plugin DLL exports ------------------------------------------ */

typedef uint32_t     (NWN2_CALL *NWN2_GetPluginAbiVersionFunc)(void);
typedef NWN2_Plugin* (NWN2_CALL *NWN2_CreatePluginFunc)(const NWN2_PluginHost* host);
typedef void         (NWN2_CALL *NWN2_DestroyPluginFunc)(NWN2_Plugin* plugin);

/* Exports the version, so the loader can reject a plugin built against a different ABI. */
#define NWN2_DECLARE_PLUGIN_ABI()                                        \
    NWN2_EXTERN_C __declspec(dllexport)                                  \
    uint32_t NWN2_CALL GetPluginAbiVersion(void)                         \
    { return NWN2_PLUGIN_ABI_VERSION; }

#ifdef __cplusplus
}
#endif
#endif /* NWN2_PLUGIN_ABI_H */
