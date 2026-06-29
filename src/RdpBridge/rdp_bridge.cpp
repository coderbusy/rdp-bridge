#include "rdp_bridge.h"

#include <atomic>
#include <cstring>
#include <iomanip>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <openssl/provider.h>
#include <openssl/err.h>

extern "C" {
#include <freerdp/freerdp.h>
#include <freerdp/version.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/input.h>
#include <freerdp/error.h>
#include <freerdp/settings.h>
#include <freerdp/settings_keys.h>
#include <winpr/winsock.h>
}

namespace
{

// ---------------------------------------------------------------------------
// Internal session state
// ---------------------------------------------------------------------------

struct RdpSession
{
    freerdp* instance = nullptr;
    std::thread worker;
    std::atomic_bool running{false};
    std::atomic_bool connected{false};
    std::mutex callback_mutex;
    std::string host;
    std::string username;
    std::string password;
    int port = 3389;
    int width = 1024;
    int height = 768;
    bool wsa_started = false;
    bool openssl_providers_loaded = false;
    RdpBridge_FrameCallback frame_callback = nullptr;
    RdpBridge_StatusCallback status_callback = nullptr;
    RdpBridge_DisconnectCallback disconnect_callback = nullptr;
    void* user_data = nullptr;
    std::string last_error;
};

struct RdpContext
{
    rdpContext context;
    RdpSession* session = nullptr;
};

struct SecurityProfile
{
    const char* name;
    bool nla;
    bool tls;
    bool rdp;
    bool negotiate;
    bool useRdpSecurityLayer;
};

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

BOOL rdp_pre_connect(freerdp* instance);
BOOL rdp_post_connect(freerdp* instance);
void rdp_post_disconnect(freerdp* instance);
BOOL rdp_authenticate(freerdp* instance, char** username, char** password, char** domain, rdp_auth_reason reason);
DWORD rdp_verify_certificate(
    freerdp* instance,
    const char* host,
    UINT16 port,
    const char* common_name,
    const char* subject,
    const char* issuer,
    const char* fingerprint,
    DWORD flags);
DWORD rdp_verify_changed_certificate(
    freerdp* instance,
    const char* host,
    UINT16 port,
    const char* common_name,
    const char* subject,
    const char* issuer,
    const char* new_fingerprint,
    const char* old_subject,
    const char* old_issuer,
    const char* old_fingerprint,
    DWORD flags);
int rdp_verify_x509_certificate(
    freerdp* instance,
    const BYTE* data,
    size_t length,
    const char* hostname,
    UINT16 port,
    DWORD flags);
int rdp_logon_error(freerdp* instance, UINT32 data, UINT32 type);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

RdpSession* get_session(rdpContext* context)
{
    if (!context)
        return nullptr;
    return reinterpret_cast<RdpContext*>(context)->session;
}

rdpSettings* get_settings(RdpSession* session)
{
    if (!session || !session->instance || !session->instance->context)
        return nullptr;
    return session->instance->context->settings;
}

bool initialize_instance(RdpSession* session)
{
    if (!session)
        return false;

    session->instance = freerdp_new();
    if (!session->instance)
        return false;

    session->instance->PreConnect = rdp_pre_connect;
    session->instance->PostConnect = rdp_post_connect;
    session->instance->PostDisconnect = rdp_post_disconnect;
    session->instance->AuthenticateEx = rdp_authenticate;
    session->instance->VerifyCertificateEx = rdp_verify_certificate;
    session->instance->VerifyChangedCertificateEx = rdp_verify_changed_certificate;
    session->instance->VerifyX509Certificate = rdp_verify_x509_certificate;
    session->instance->LogonErrorInfo = rdp_logon_error;
    session->instance->ContextSize = sizeof(RdpContext);

    if (!freerdp_context_new(session->instance))
    {
        freerdp_free(session->instance);
        session->instance = nullptr;
        return false;
    }

    reinterpret_cast<RdpContext*>(session->instance->context)->session = session;
    return true;
}

void free_instance(RdpSession* session)
{
    if (!session || !session->instance)
        return;

    freerdp_context_free(session->instance);
    freerdp_free(session->instance);
    session->instance = nullptr;
}

void set_error(RdpSession* session, const std::string& message)
{
    if (session)
        session->last_error = message;
}

void notify_status(RdpSession* session, const char* message)
{
    if (!session)
        return;

    std::lock_guard<std::mutex> lock(session->callback_mutex);
    if (session->status_callback)
        session->status_callback(session->user_data, message);
}

std::string get_library_directory()
{
#if defined(_WIN32)
    HMODULE module = nullptr;
    char path[MAX_PATH]{};
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&get_library_directory),
            &module))
        return {};

    const DWORD length = GetModuleFileNameA(module, path, static_cast<DWORD>(sizeof(path)));
    if (length == 0 || length >= sizeof(path))
        return {};

    std::string value(path, length);
    const auto index = value.find_last_of("\\/");
    return index == std::string::npos ? std::string{} : value.substr(0, index);
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&get_library_directory), &info) == 0 || !info.dli_fname)
        return {};

    std::string value(info.dli_fname);
    const auto index = value.find_last_of("\\/");
    return index == std::string::npos ? std::string{} : value.substr(0, index);
#endif
}

void ensure_openssl_providers(RdpSession* session)
{
    if (!session || session->openssl_providers_loaded)
        return;

    const auto libDirectory = get_library_directory();
    std::string providerDirectory = libDirectory;

#if defined(_WIN32)
    if (!libDirectory.empty())
    {
        SetDllDirectoryA(libDirectory.c_str());
        const auto moduleDirectory = libDirectory + "\\ossl-modules";
        const DWORD attributes = GetFileAttributesA(moduleDirectory.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
            providerDirectory = moduleDirectory;
    }
#endif

    if (!providerDirectory.empty())
        OSSL_PROVIDER_set_default_search_path(nullptr, providerDirectory.c_str());

#if defined(_WIN32)
    std::string nativeLoadStatus;
    if (!providerDirectory.empty())
    {
        const auto providerPath = providerDirectory + "\\legacy.dll";
        SetLastError(0);
        HMODULE providerModule = LoadLibraryA(providerPath.c_str());
        if (providerModule)
        {
            nativeLoadStatus = " nativeLoad=ok";
        }
        else
        {
            std::ostringstream nativeError;
            nativeError << " nativeLoad=failed(" << GetLastError() << ")";
            nativeLoadStatus = nativeError.str();
        }
    }
#endif

    ERR_clear_error();
    auto* defaultProvider = OSSL_PROVIDER_load(nullptr, "default");
    std::vector<std::string> opensslErrors;
    auto* legacyProvider = OSSL_PROVIDER_load(nullptr, "legacy");
    unsigned long errorCode = 0;
    while ((errorCode = ERR_get_error()) != 0)
    {
        char buffer[256]{};
        ERR_error_string_n(errorCode, buffer, sizeof(buffer));
        opensslErrors.emplace_back(buffer);
    }

    std::ostringstream message;
    message << "OpenSSL providers default="
            << (defaultProvider ? "loaded" : "failed")
            << " legacy="
            << (legacyProvider ? "loaded" : "failed")
            << " path="
            << (providerDirectory.empty() ? "<empty>" : providerDirectory);
#if defined(_WIN32)
    message << nativeLoadStatus;
#endif
    if (!opensslErrors.empty())
    {
        message << " errors=";
        for (size_t i = 0; i < opensslErrors.size(); ++i)
        {
            if (i > 0)
                message << " | ";
            message << opensslErrors[i];
        }
    }

    notify_status(session, message.str().c_str());
    session->openssl_providers_loaded = true;
}

char* duplicate_string(const std::string& value)
{
    const auto length = value.size() + 1;
    auto* copy = static_cast<char*>(std::malloc(length));
    if (!copy)
        return nullptr;
    std::memcpy(copy, value.c_str(), length);
    return copy;
}

const char* auth_reason_name(rdp_auth_reason reason)
{
    switch (reason)
    {
    case AUTH_NLA:            return "NLA";
    case AUTH_TLS:            return "TLS";
    case AUTH_RDP:            return "RDP";
    case GW_AUTH_HTTP:        return "GatewayHttp";
    case GW_AUTH_RDG:         return "GatewayRdg";
    case GW_AUTH_RPC:         return "GatewayRpc";
    case AUTH_SMARTCARD_PIN:  return "SmartcardPin";
#if FREERDP_VERSION_MAJOR > 3 || (FREERDP_VERSION_MAJOR == 3 && FREERDP_VERSION_MINOR >= 26)
    case AUTH_RDSTLS:         return "Rdstls";
    case AUTH_FIDO_PIN:       return "FidoPin";
#endif
    default:                  return "Unknown";
    }
}

std::string describe_last_error(RdpSession* session)
{
    if (!session || !session->instance || !session->instance->context)
        return "FreeRDP connection failed.";

    const UINT32 code = freerdp_get_last_error(session->instance->context);
    const char* name     = freerdp_get_last_error_name(code);
    const char* text     = freerdp_get_last_error_string(code);
    const char* category = freerdp_get_last_error_category(code);

    std::ostringstream stream;
    stream << "FreeRDP connection failed.";
    stream << " code=0x" << std::hex << std::setw(8) << std::setfill('0') << code;
    if (name && name[0] != '\0')
        stream << " name=" << name;
    if (category && category[0] != '\0')
        stream << " category=" << category;
    if (text && text[0] != '\0')
        stream << " message=" << text;

    return stream.str();
}

// ---------------------------------------------------------------------------
// FreeRDP callbacks
// ---------------------------------------------------------------------------

BOOL rdp_authenticate(
    freerdp* instance,
    char** username,
    char** password,
    char** domain,
    rdp_auth_reason reason)
{
    if (!instance || !instance->context)
        return FALSE;

    auto* session = get_session(instance->context);
    if (!session)
        return FALSE;

    std::ostringstream message;
    message << "RDP authenticate callback reason=" << auth_reason_name(reason)
            << " usernameLen=" << session->username.size()
            << " passwordLen=" << session->password.size();
    notify_status(session, message.str().c_str());

    if (username)
    {
        std::free(*username);
        *username = duplicate_string(session->username);
    }
    if (password)
    {
        std::free(*password);
        *password = duplicate_string(session->password);
    }
    if (domain)
    {
        std::free(*domain);
        *domain = duplicate_string("");
    }

    return TRUE;
}

DWORD rdp_verify_certificate(
    freerdp* instance,
    const char* host,
    UINT16 port,
    const char* common_name,
    const char* /*subject*/,
    const char* /*issuer*/,
    const char* fingerprint,
    DWORD flags)
{
    if (instance && instance->context)
    {
        auto* session = get_session(instance->context);
        std::ostringstream message;
        message << "RDP certificate accepted host=" << (host ? host : "<null>")
                << " port=" << port
                << " commonName=" << (common_name ? common_name : "<null>")
                << " fingerprint=" << (fingerprint ? fingerprint : "<null>")
                << " flags=0x" << std::hex << flags;
        notify_status(session, message.str().c_str());
    }
    return 2;
}

DWORD rdp_verify_changed_certificate(
    freerdp* instance,
    const char* host,
    UINT16 port,
    const char* common_name,
    const char* subject,
    const char* issuer,
    const char* new_fingerprint,
    const char* /*old_subject*/,
    const char* /*old_issuer*/,
    const char* /*old_fingerprint*/,
    DWORD flags)
{
    return rdp_verify_certificate(
        instance, host, port, common_name, subject, issuer, new_fingerprint, flags);
}

int rdp_verify_x509_certificate(
    freerdp* instance,
    const BYTE* /*data*/,
    size_t length,
    const char* hostname,
    UINT16 port,
    DWORD flags)
{
    if (instance && instance->context)
    {
        auto* session = get_session(instance->context);
        std::ostringstream message;
        message << "RDP X509 certificate accepted host=" << (hostname ? hostname : "<null>")
                << " port=" << port
                << " length=" << length
                << " flags=0x" << std::hex << flags;
        notify_status(session, message.str().c_str());
    }
    return 2;
}

int rdp_logon_error(freerdp* instance, UINT32 data, UINT32 type)
{
    if (instance && instance->context)
    {
        auto* session = get_session(instance->context);
        std::ostringstream message;
        message << "RDP logon error data=0x" << std::hex << data << " type=0x" << type;
        notify_status(session, message.str().c_str());
    }
    return 1;
}

BOOL rdp_begin_paint(rdpContext* context)
{
    if (!context || !context->gdi)
        return FALSE;

    context->gdi->primary->hdc->hwnd->invalid->null = TRUE;
    return TRUE;
}

BOOL rdp_end_paint(rdpContext* context)
{
    if (!context || !context->gdi || !context->instance)
        return FALSE;

    auto* session = get_session(context);
    if (!session)
        return TRUE;

    auto* gdi = context->gdi;
    if (!gdi->primary_buffer || gdi->width <= 0 || gdi->height <= 0)
        return TRUE;

    const int width  = static_cast<int>(gdi->width);
    const int height = static_cast<int>(gdi->height);
    const int stride = width * 4;
    const auto* pixels = static_cast<const uint8_t*>(gdi->primary_buffer);
    if (!pixels)
        return TRUE;

    std::lock_guard<std::mutex> lock(session->callback_mutex);
    if (session->frame_callback)
        session->frame_callback(session->user_data, width, height, stride, pixels);

    return TRUE;
}

BOOL rdp_pre_connect(freerdp* instance)
{
    if (!instance || !instance->context || !instance->context->settings)
        return FALSE;

    freerdp_settings_set_uint32(
        instance->context->settings, FreeRDP_OsMajorType, OSMAJORTYPE_WINDOWS);
    freerdp_settings_set_uint32(
        instance->context->settings, FreeRDP_OsMinorType, OSMINORTYPE_WINDOWS_NT);
    return TRUE;
}

BOOL rdp_post_connect(freerdp* instance)
{
    if (!instance || !instance->context)
        return FALSE;

    auto* session = get_session(instance->context);
    if (!session)
        return FALSE;

    if (!gdi_init(instance, PIXEL_FORMAT_BGRA32))
    {
        set_error(session, "FreeRDP GDI initialization failed.");
        return FALSE;
    }

    instance->context->update->BeginPaint = rdp_begin_paint;
    instance->context->update->EndPaint   = rdp_end_paint;
    notify_status(session, "RDP connected.");
    return TRUE;
}

void rdp_post_disconnect(freerdp* instance)
{
    if (!instance || !instance->context)
        return;

    auto* session = get_session(instance->context);
    gdi_free(instance);

    if (session)
    {
        const bool wasConnected = session->connected.exchange(false);
        notify_status(session, "RDP disconnected.");
        std::lock_guard<std::mutex> lock(session->callback_mutex);
        if (wasConnected && session->disconnect_callback)
            session->disconnect_callback(session->user_data);
    }
}

// ---------------------------------------------------------------------------
// Connection worker thread
// ---------------------------------------------------------------------------

void connection_thread(RdpSession* session)
{
    notify_status(session, "Connecting RDP...");

    {
        std::ostringstream target;
        target << "RDP target host=" << session->host
               << " port=" << session->port
               << " user=" << session->username
               << " size=" << session->width << "x" << session->height;
        notify_status(session, target.str().c_str());
    }

    const SecurityProfile profiles[] = {
        { "nla",       true,  false, false, false, false },
        { "tls",       false, true,  false, false, false },
        { "rdp",       false, false, true,  false, true  },
        { "negotiate", true,  true,  true,  true,  false }
    };

    std::string lastError;
    for (const auto& profile : profiles)
    {
        if (!session->running)
            break;

        if (!initialize_instance(session))
        {
            lastError = "FreeRDP instance initialization failed.";
            set_error(session, lastError);
            notify_status(session, lastError.c_str());
            break;
        }

        auto* settings = get_settings(session);
        if (!settings)
        {
            lastError = "FreeRDP settings unavailable.";
            set_error(session, lastError);
            notify_status(session, lastError.c_str());
            free_instance(session);
            break;
        }

        freerdp_settings_set_string(settings, FreeRDP_ServerHostname, session->host.c_str());
        freerdp_settings_set_string(settings, FreeRDP_UserSpecifiedServerName, session->host.c_str());
        freerdp_settings_set_uint32(settings, FreeRDP_ServerPort,
            static_cast<uint32_t>(session->port > 0 ? session->port : 3389));
        freerdp_settings_set_string(settings, FreeRDP_Username, session->username.c_str());
        freerdp_settings_set_string(settings, FreeRDP_Password, session->password.c_str());
        freerdp_settings_set_string(settings, FreeRDP_Domain, "");
        freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth,
            static_cast<uint32_t>(session->width > 0 ? session->width : 1024));
        freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight,
            static_cast<uint32_t>(session->height > 0 ? session->height : 768));
        freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);
        freerdp_settings_set_bool(settings, FreeRDP_IgnoreCertificate, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_AutoAcceptCertificate, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_Authentication, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_AutoLogonEnabled, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_LogonNotify, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_LogonErrors, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_LongCredentialsSupported, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_SoftwareGdi, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, FALSE);
        freerdp_settings_set_bool(settings, FreeRDP_GfxThinClient, FALSE);
        freerdp_settings_set_bool(settings, FreeRDP_NegotiateSecurityLayer,
            profile.negotiate ? TRUE : FALSE);
        freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity,
            profile.nla ? TRUE : FALSE);
        freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity,
            profile.tls ? TRUE : FALSE);
        freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity,
            profile.rdp ? TRUE : FALSE);
        freerdp_settings_set_bool(settings, FreeRDP_UseRdpSecurityLayer,
            profile.useRdpSecurityLayer ? TRUE : FALSE);

        {
            const char* parsedServerName =
                freerdp_settings_get_server_name(settings);
            const char* parsedHostName =
                freerdp_settings_get_string(settings, FreeRDP_ServerHostname);
            const char* parsedUserSpecifiedName =
                freerdp_settings_get_string(settings, FreeRDP_UserSpecifiedServerName);
            const UINT32 parsedPort =
                freerdp_settings_get_uint32(settings, FreeRDP_ServerPort);
            std::ostringstream parsedTarget;
            parsedTarget << "RDP parsed target server="
                         << (parsedServerName ? parsedServerName : "<null>")
                         << " host="
                         << (parsedHostName ? parsedHostName : "<null>")
                         << " userSpecified="
                         << (parsedUserSpecifiedName ? parsedUserSpecifiedName : "<null>")
                         << " port=" << parsedPort;
            notify_status(session, parsedTarget.str().c_str());
        }

        {
            std::ostringstream security;
            security << "RDP security attempt mode=" << profile.name
                     << " nla=" << (freerdp_settings_get_bool(settings, FreeRDP_NlaSecurity) ? "true" : "false")
                     << " tls=" << (freerdp_settings_get_bool(settings, FreeRDP_TlsSecurity) ? "true" : "false")
                     << " rdp=" << (freerdp_settings_get_bool(settings, FreeRDP_RdpSecurity) ? "true" : "false")
                     << " negotiate=" << (freerdp_settings_get_bool(settings, FreeRDP_NegotiateSecurityLayer) ? "true" : "false");
            notify_status(session, security.str().c_str());
        }

        if (!freerdp_connect(session->instance))
        {
            lastError = describe_last_error(session);
            std::ostringstream attemptError;
            attemptError << lastError << " mode=" << profile.name;
            set_error(session, attemptError.str());
            notify_status(session, attemptError.str().c_str());
            free_instance(session);
            continue;
        }

        session->connected = true;
        break;
    }

    if (!session->connected)
    {
        session->running = false;
        if (!lastError.empty())
            set_error(session, lastError);
        return;
    }

    while (session->running)
    {
        if (freerdp_check_fds(session->instance) != TRUE)
        {
            set_error(session, "FreeRDP transport closed.");
            break;
        }
    }

    freerdp_disconnect(session->instance);
    free_instance(session);
    session->running = false;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public C ABI
// ---------------------------------------------------------------------------

extern "C"
{

RDP_BRIDGE_API void* RdpBridge_create(void)
{
    return new RdpSession();
}

RDP_BRIDGE_API void RdpBridge_destroy(void* handle)
{
    auto* session = static_cast<RdpSession*>(handle);
    if (!session)
        return;

    RdpBridge_disconnect(handle);
    free_instance(session);
    delete session;
}

RDP_BRIDGE_API void RdpBridge_set_callbacks(
    void* handle,
    RdpBridge_FrameCallback frame_callback,
    RdpBridge_StatusCallback status_callback,
    RdpBridge_DisconnectCallback disconnect_callback,
    void* user_data)
{
    auto* session = static_cast<RdpSession*>(handle);
    if (!session)
        return;

    std::lock_guard<std::mutex> lock(session->callback_mutex);
    session->frame_callback      = frame_callback;
    session->status_callback     = status_callback;
    session->disconnect_callback = disconnect_callback;
    session->user_data           = user_data;
}

RDP_BRIDGE_API int RdpBridge_connect(
    void* handle,
    const char* host,
    int port,
    const char* username,
    const char* password,
    int width,
    int height)
{
    auto* session = static_cast<RdpSession*>(handle);
    if (!session)
        return -1;

    if (session->running)
        return 0;

#if defined(_WIN32)
    if (!session->wsa_started)
    {
        WSADATA wsaData{};
        const int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (wsaResult != 0)
        {
            std::ostringstream error;
            error << "Winsock initialization failed. code=" << wsaResult;
            set_error(session, error.str());
            notify_status(session, error.str().c_str());
            return -2;
        }
        session->wsa_started = true;
    }
#endif

    session->host     = host     ? host     : "";
    session->port     = port     > 0 ? port : 3389;
    session->username = username ? username : "";
    session->password = password ? password : "";
    session->width    = width    > 0 ? width  : 1024;
    session->height   = height   > 0 ? height : 768;

    ensure_openssl_providers(session);

    session->running = true;
    session->worker  = std::thread(connection_thread, session);
    return 0;
}

RDP_BRIDGE_API void RdpBridge_disconnect(void* handle)
{
    auto* session = static_cast<RdpSession*>(handle);
    if (!session)
        return;

    session->running = false;
    if (session->instance)
        freerdp_abort_connect(session->instance);

    if (session->worker.joinable())
        session->worker.join();

#if defined(_WIN32)
    if (session->wsa_started)
    {
        WSACleanup();
        session->wsa_started = false;
    }
#endif
}

RDP_BRIDGE_API void RdpBridge_send_pointer(void* handle, uint16_t flags, uint16_t x, uint16_t y)
{
    auto* session = static_cast<RdpSession*>(handle);
    if (!session || !session->connected)
        return;

    auto* input = (session->instance && session->instance->context)
        ? session->instance->context->input
        : nullptr;
    if (!input)
        return;

    freerdp_input_send_mouse_event(input, flags, x, y);
}

RDP_BRIDGE_API void RdpBridge_send_key(void* handle, uint32_t key, int down)
{
    auto* session = static_cast<RdpSession*>(handle);
    if (!session || !session->connected)
        return;

    auto* input = (session->instance && session->instance->context)
        ? session->instance->context->input
        : nullptr;
    if (!input)
        return;

    freerdp_input_send_keyboard_event_ex(input, down ? TRUE : FALSE, FALSE, key);
}

RDP_BRIDGE_API void RdpBridge_send_unicode_key(void* handle, uint16_t code, int down)
{
    auto* session = static_cast<RdpSession*>(handle);
    if (!session || !session->connected || code == 0)
        return;

    auto* input = (session->instance && session->instance->context)
        ? session->instance->context->input
        : nullptr;
    if (!input)
        return;

    freerdp_input_send_unicode_keyboard_event(input, down ? 0 : KBD_FLAGS_RELEASE, code);
}

RDP_BRIDGE_API const char* RdpBridge_get_last_error(void* handle)
{
    auto* session = static_cast<RdpSession*>(handle);
    if (!session)
        return "";
    return session->last_error.c_str();
}

} // extern "C"
