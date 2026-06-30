#pragma once

#include <stdint.h>

#if defined(_WIN32)
#if defined(RDP_BRIDGE_EXPORTS)
#define RDP_BRIDGE_API __declspec(dllexport)
#else
#define RDP_BRIDGE_API __declspec(dllimport)
#endif
#else
#define RDP_BRIDGE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Session state enumeration delivered by RdpBridge_StateCallback.
 */
typedef enum RdpBridge_State
{
    RdpBridge_State_Connecting   = 0,
    RdpBridge_State_Connected    = 1,
    RdpBridge_State_Disconnected = 2,
    RdpBridge_State_Failed       = 3
} RdpBridge_State;

/**
 * State callback: called each time the session transitions to a new state.
 *
 * @param user_data Opaque pointer supplied via RdpBridge_set_state_callback.
 * @param state     New session state.
 */
typedef void (*RdpBridge_StateCallback)(void* user_data, RdpBridge_State state);

/**
 * Clipboard callback: delivers remote clipboard text to the caller.
 *
 * @param user_data  Opaque pointer supplied via RdpBridge_set_clipboard_callback.
 * @param utf8_text  Null-terminated UTF-8 string, valid only during the callback.
 */
typedef void (*RdpBridge_ClipboardCallback)(void* user_data, const char* utf8_text);

/** Experience flag: disable desktop wallpaper. */
#define RDPBRIDGE_EXPERIENCE_DISABLE_WALLPAPER        0x01u
/** Experience flag: disable full-window drag. */
#define RDPBRIDGE_EXPERIENCE_DISABLE_FULL_WINDOW_DRAG 0x02u
/** Experience flag: disable menu animations. */
#define RDPBRIDGE_EXPERIENCE_DISABLE_MENU_ANIMS       0x04u
/** Experience flag: disable desktop themes. */
#define RDPBRIDGE_EXPERIENCE_DISABLE_THEMES           0x08u
/** Experience flag: enable font smoothing (ClearType). */
#define RDPBRIDGE_EXPERIENCE_ENABLE_FONT_SMOOTHING    0x10u

/**
 * Framebuffer callback: called whenever a new frame is available.
 *
 * @param user_data   Opaque pointer supplied via RdpBridge_set_callbacks.
 * @param width       Frame width in pixels.
 * @param height      Frame height in pixels.
 * @param stride      Row stride in bytes (= width * 4 for BGRA32).
 * @param bgra_pixels Pointer to the raw BGRA32 pixel buffer (valid only
 *                    during the callback).
 */
typedef void (*RdpBridge_FrameCallback)(
    void* user_data,
    int width,
    int height,
    int stride,
    const uint8_t* bgra_pixels);

/**
 * Status callback: called for log / state-change messages.
 *
 * @param user_data Opaque pointer supplied via RdpBridge_set_callbacks.
 * @param message   Null-terminated UTF-8 status string.
 */
typedef void (*RdpBridge_StatusCallback)(void* user_data, const char* message);

/**
 * Disconnect callback: called when the remote session is closed.
 *
 * @param user_data Opaque pointer supplied via RdpBridge_set_callbacks.
 */
typedef void (*RdpBridge_DisconnectCallback)(void* user_data);

/**
 * Create a new RDP bridge session.
 *
 * @return Opaque session handle, or NULL on failure.
 */
RDP_BRIDGE_API void* RdpBridge_create(void);

/**
 * Destroy a session previously created with RdpBridge_create.
 * Calls RdpBridge_disconnect internally if the session is still active.
 *
 * @param handle Session handle returned by RdpBridge_create.
 */
RDP_BRIDGE_API void RdpBridge_destroy(void* handle);

/**
 * Register event callbacks for a session.
 * May be called before or after RdpBridge_connect.
 *
 * @param handle              Session handle.
 * @param frame_callback      Called on every rendered frame (may be NULL).
 * @param status_callback     Called for status / log messages (may be NULL).
 * @param disconnect_callback Called when the session disconnects (may be NULL).
 * @param user_data           Passed unchanged to every callback.
 */
RDP_BRIDGE_API void RdpBridge_set_callbacks(
    void* handle,
    RdpBridge_FrameCallback frame_callback,
    RdpBridge_StatusCallback status_callback,
    RdpBridge_DisconnectCallback disconnect_callback,
    void* user_data);

/**
 * Connect to an RDP server.
 * Attempts NLA, TLS, RDP-legacy, and negotiate security profiles in order.
 * The connection runs on a dedicated background thread.
 *
 * @param handle   Session handle.
 * @param host     Server hostname or IP address (UTF-8, must not be NULL).
 * @param port     TCP port (use 3389 for the default).
 * @param username Login username (UTF-8, must not be NULL).
 * @param password Login password (UTF-8, must not be NULL).
 * @param width    Initial desktop width in pixels.
 * @param height   Initial desktop height in pixels.
 * @return  0  – connection thread started successfully.
 *         -1  – invalid handle.
 *         -2  – Winsock initialisation failed (Windows only).
 */
RDP_BRIDGE_API int RdpBridge_connect(
    void* handle,
    const char* host,
    int port,
    const char* username,
    const char* password,
    int width,
    int height);

/**
 * Disconnect an active session and wait for the worker thread to exit.
 *
 * @param handle Session handle.
 */
RDP_BRIDGE_API void RdpBridge_disconnect(void* handle);

/**
 * Send a mouse event to the remote session.
 *
 * @param handle Session handle.
 * @param flags  PTR_FLAGS_* bitmask (FreeRDP / MS-RDPBCGR §2.2.8.1.1.3.1.1).
 * @param x      Pointer X coordinate in desktop pixels.
 * @param y      Pointer Y coordinate in desktop pixels.
 */
RDP_BRIDGE_API void RdpBridge_send_pointer(void* handle, uint16_t flags, uint16_t x, uint16_t y);

/**
 * Send a keyboard scancode event to the remote session.
 *
 * @param handle Session handle.
 * @param key    RDP scancode / virtual key (depends on FreeRDP API layer).
 * @param down   Non-zero for key-press, zero for key-release.
 */
RDP_BRIDGE_API void RdpBridge_send_key(void* handle, uint32_t key, int down);

/**
 * Send a Unicode character event to the remote session.
 *
 * @param handle Session handle.
 * @param code   UTF-16 code unit to inject.
 * @param down   Non-zero for key-press, zero for key-release.
 */
RDP_BRIDGE_API void RdpBridge_send_unicode_key(void* handle, uint16_t code, int down);

/**
 * Return the last error message for this session.
 *
 * @param handle Session handle.
 * @return Null-terminated UTF-8 string.  Returns "" when handle is NULL.
 *         The pointer is valid until the next API call on the same handle.
 */
RDP_BRIDGE_API const char* RdpBridge_get_last_error(void* handle);

/**
 * Set optional connection parameters.  Call before RdpBridge_connect.
 * Parameters not supplied (NULL / 0) keep their defaults.
 *
 * @param handle       Session handle.
 * @param domain       Windows domain (UTF-8).  NULL or "" = no domain.
 * @param color_depth  16, 24, or 32.  0 = default (32).
 * @param experience   Bitmask of RDPBRIDGE_EXPERIENCE_* flags.  0 = defaults.
 */
RDP_BRIDGE_API void RdpBridge_set_options(
    void* handle,
    const char* domain,
    int color_depth,
    uint32_t experience);

/**
 * Register a local directory to expose as a network drive on the remote.
 * Call before RdpBridge_connect; may be called multiple times.
 *
 * @param handle      Session handle.
 * @param name        Share name visible on the remote (e.g. "LocalDisk").
 * @param local_path  Local filesystem path (UTF-8).
 */
RDP_BRIDGE_API void RdpBridge_add_drive(
    void* handle,
    const char* name,
    const char* local_path);

/**
 * Register a structured state callback.
 * Independent of RdpBridge_set_callbacks; either or both may be used.
 *
 * @param handle         Session handle.
 * @param state_callback Called on every state transition.  NULL = unregister.
 * @param user_data      Passed unchanged to every state_callback invocation.
 */
RDP_BRIDGE_API void RdpBridge_set_state_callback(
    void* handle,
    RdpBridge_StateCallback state_callback,
    void* user_data);

/**
 * Register a callback that receives remote clipboard text.
 * Setting a non-NULL callback also enables the CLIPRDR virtual channel
 * for the next RdpBridge_connect call on this handle.
 *
 * @param handle             Session handle.
 * @param clipboard_callback Called when the remote clipboard changes.
 *                           NULL = unregister.
 * @param user_data          Passed unchanged to every clipboard_callback invocation.
 */
RDP_BRIDGE_API void RdpBridge_set_clipboard_callback(
    void* handle,
    RdpBridge_ClipboardCallback clipboard_callback,
    void* user_data);

/**
 * Push local text to the remote clipboard.
 * Safe to call from any thread while the session is connected.
 *
 * @param handle     Session handle.
 * @param utf8_text  Null-terminated UTF-8 string.
 * @return  0  – announced successfully.
 *         -1  – not connected or CLIPRDR channel not available.
 */
RDP_BRIDGE_API int RdpBridge_clipboard_set_text(
    void* handle,
    const char* utf8_text);

/**
 * Request a dynamic desktop resize.
 * Requires the DISP virtual channel, which is loaded automatically when
 * RdpBridge_resize has been called before RdpBridge_connect on the same handle.
 * Also safe to call before connecting to update the initial resolution.
 *
 * @param handle  Session handle.
 * @param width   New desktop width in pixels.
 * @param height  New desktop height in pixels.
 * @return  0  – resize request sent.
 *         -1  – not connected or DISP channel not available.
 */
RDP_BRIDGE_API int RdpBridge_resize(
    void* handle,
    int width,
    int height);

#ifdef __cplusplus
}
#endif
