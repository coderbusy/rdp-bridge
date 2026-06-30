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
#include <iconv.h>
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
#include <freerdp/channels/channels.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/channels/cliprdr.h>
#include <freerdp/client/disp.h>
#include <freerdp/channels/disp.h>
#include <winpr/winsock.h>
}

namespace
{

// Bytes per pixel for the BGRA32 pixel format used by this SDK.
static constexpr int BGRA32_BYTES_PER_PIXEL = 4;

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
    std::string domain;
    int port = 3389;
    int width = 1024;
    int height = 768;
    int color_depth = 32;
    uint32_t experience_flags = 0;
    bool wsa_started = false;
    bool openssl_providers_loaded = false;
    bool want_disp_channel = false;

    struct DriveEntry { std::string name, path; };
    std::vector<DriveEntry> drives;

    RdpBridge_FrameCallback frame_callback = nullptr;
    RdpBridge_StatusCallback status_callback = nullptr;
    RdpBridge_DisconnectCallback disconnect_callback = nullptr;
    RdpBridge_StateCallback state_callback = nullptr;
    RdpBridge_ClipboardCallback clipboard_callback = nullptr;
    void* user_data = nullptr;

    CliprdrClientContext* cliprdr = nullptr;
    std::string pending_local_text;

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
// Clipboard (cliprdr) forward declarations
// ---------------------------------------------------------------------------

static void cliprdr_announce_formats(CliprdrClientContext* context);
static UINT cliprdr_server_capabilities(CliprdrClientContext* context, const CLIPRDR_CAPABILITIES* capabilities);
static UINT cliprdr_server_format_list(CliprdrClientContext* context, const CLIPRDR_FORMAT_LIST* formatList);
static UINT cliprdr_server_format_list_response(CliprdrClientContext* context, const CLIPRDR_FORMAT_LIST_RESPONSE* response);
static UINT cliprdr_server_format_data_request(CliprdrClientContext* context, const CLIPRDR_FORMAT_DATA_REQUEST* request);
static UINT cliprdr_server_format_data_response(CliprdrClientContext* context, const CLIPRDR_FORMAT_DATA_RESPONSE* response);

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

void notify_state(RdpSession* session, RdpBridge_State state)
{
    if (!session)
        return;

    std::lock_guard<std::mutex> lock(session->callback_mutex);
    if (session->state_callback)
        session->state_callback(session->user_data, state);
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
        *domain = duplicate_string(session->domain);
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

// ---------------------------------------------------------------------------
// UTF-16LE ↔ UTF-8 conversion helpers
// ---------------------------------------------------------------------------

// Convert UTF-8 to UTF-16LE. Returns byte vector including null terminator.
static std::vector<uint8_t> utf8_to_utf16le(const std::string& utf8)
{
    std::vector<uint8_t> result;
    if (utf8.empty())
    {
        result.push_back(0);
        result.push_back(0);
        return result;
    }
#if defined(_WIN32)
    const int wlen = MultiByteToWideChar(
        CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0)
    {
        result.push_back(0);
        result.push_back(0);
        return result;
    }
    result.resize(static_cast<size_t>(wlen) * 2);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1,
        reinterpret_cast<LPWSTR>(result.data()), wlen);
#else
    iconv_t cd = iconv_open("UTF-16LE", "UTF-8");
    if (cd == reinterpret_cast<iconv_t>(-1))
    {
        result.push_back(0);
        result.push_back(0);
        return result;
    }
    // Allocate worst-case: 4 bytes per UTF-8 byte + 2 for null terminator
    result.resize(utf8.size() * 4 + 2);
    char* inbuf  = const_cast<char*>(utf8.c_str());
    size_t inleft  = utf8.size();
    char* outbuf   = reinterpret_cast<char*>(result.data());
    size_t outleft = result.size() - 2; // reserve 2 for null terminator
    iconv(cd, &inbuf, &inleft, &outbuf, &outleft);
    iconv_close(cd);
    const size_t written = result.size() - 2 - outleft;
    result.resize(written + 2);
    result[written]     = 0;
    result[written + 1] = 0;
#endif
    return result;
}

// Convert UTF-16LE byte buffer to UTF-8 string. dataLen is in bytes.
static std::string utf16le_to_utf8(const uint8_t* data, size_t dataLen)
{
    if (!data || dataLen < 2)
        return {};
#if defined(_WIN32)
    const int chars = WideCharToMultiByte(
        CP_UTF8, 0,
        reinterpret_cast<LPCWCH>(data),
        static_cast<int>(dataLen / 2),
        nullptr, 0, nullptr, nullptr);
    if (chars <= 0)
        return {};
    std::string result(static_cast<size_t>(chars), '\0');
    WideCharToMultiByte(CP_UTF8, 0,
        reinterpret_cast<LPCWCH>(data),
        static_cast<int>(dataLen / 2),
        result.data(), chars, nullptr, nullptr);
    while (!result.empty() && result.back() == '\0')
        result.pop_back();
    return result;
#else
    iconv_t cd = iconv_open("UTF-8", "UTF-16LE");
    if (cd == reinterpret_cast<iconv_t>(-1))
        return {};
    std::string result(dataLen * 2, '\0');
    char* inbuf    = const_cast<char*>(reinterpret_cast<const char*>(data));
    size_t inleft  = dataLen;
    char* outbuf   = result.data();
    size_t outleft = result.size();
    iconv(cd, &inbuf, &inleft, &outbuf, &outleft);
    iconv_close(cd);
    result.resize(result.size() - outleft);
    while (!result.empty() && result.back() == '\0')
        result.pop_back();
    return result;
#endif
}

// ---------------------------------------------------------------------------
// Clipboard (cliprdr) callbacks
// ---------------------------------------------------------------------------

static RdpSession* cliprdr_get_session(CliprdrClientContext* context)
{
    return context ? static_cast<RdpSession*>(context->custom) : nullptr;
}

static void cliprdr_announce_formats(CliprdrClientContext* context)
{
    CLIPRDR_FORMAT fmt{};
    fmt.formatId   = CF_UNICODETEXT;
    fmt.formatName = nullptr;

    CLIPRDR_FORMAT_LIST list{};
    list.common.msgType  = CB_FORMAT_LIST;
    list.common.msgFlags = 0;
    list.numFormats      = 1;
    list.formats         = &fmt;

    context->ClientFormatList(context, &list);
}

static UINT cliprdr_server_capabilities(
    CliprdrClientContext* context,
    const CLIPRDR_CAPABILITIES* /*capabilities*/)
{
    // Reply with our own capabilities
    CLIPRDR_GENERAL_CAPABILITY_SET generalCap{};
    generalCap.capabilitySetType   = CB_CAPSTYPE_GENERAL;
    generalCap.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
    generalCap.version             = CB_CAPS_VERSION_2;
    generalCap.generalFlags        = CB_USE_LONG_FORMAT_NAMES;

    CLIPRDR_CAPABILITIES clientCaps{};
    clientCaps.cCapabilitiesSets = 1;
    clientCaps.capabilitySets    =
        reinterpret_cast<CLIPRDR_CAPABILITY_SET*>(&generalCap);

    context->ClientCapabilities(context, &clientCaps);

    // Announce that we have clipboard data (text)
    cliprdr_announce_formats(context);

    return CHANNEL_RC_OK;
}

static UINT cliprdr_server_format_list(
    CliprdrClientContext* context,
    const CLIPRDR_FORMAT_LIST* formatList)
{
    // Acknowledge
    CLIPRDR_FORMAT_LIST_RESPONSE resp{};
    resp.common.msgType  = CB_FORMAT_LIST_RESPONSE;
    resp.common.msgFlags = CB_RESPONSE_OK;
    context->ClientFormatListResponse(context, &resp);

    // If the remote offers CF_UNICODETEXT, request it
    for (UINT32 i = 0; i < formatList->numFormats; ++i)
    {
        if (formatList->formats[i].formatId == CF_UNICODETEXT)
        {
            CLIPRDR_FORMAT_DATA_REQUEST req{};
            req.common.msgType       = CB_FORMAT_DATA_REQUEST;
            req.common.msgFlags      = 0;
            req.requestedFormatId    = CF_UNICODETEXT;
            context->ClientFormatDataRequest(context, &req);
            break;
        }
    }

    return CHANNEL_RC_OK;
}

static UINT cliprdr_server_format_list_response(
    CliprdrClientContext* /*context*/,
    const CLIPRDR_FORMAT_LIST_RESPONSE* /*response*/)
{
    return CHANNEL_RC_OK;
}

static UINT cliprdr_server_format_data_request(
    CliprdrClientContext* context,
    const CLIPRDR_FORMAT_DATA_REQUEST* request)
{
    auto* session = cliprdr_get_session(context);

    CLIPRDR_FORMAT_DATA_RESPONSE resp{};
    resp.common.msgType = CB_FORMAT_DATA_RESPONSE;

    if (!session || request->requestedFormatId != CF_UNICODETEXT)
    {
        resp.common.msgFlags     = CB_RESPONSE_FAIL;
        resp.common.dataLen      = 0;
        resp.requestedFormatData = nullptr;
        context->ClientFormatDataResponse(context, &resp);
        return CHANNEL_RC_OK;
    }

    std::string utf8Text;
    {
        std::lock_guard<std::mutex> lock(session->callback_mutex);
        utf8Text = session->pending_local_text;
    }

    const auto utf16 = utf8_to_utf16le(utf8Text);

    resp.common.msgFlags     = CB_RESPONSE_OK;
    resp.common.dataLen      = static_cast<UINT32>(utf16.size());
    resp.requestedFormatData = utf16.data();
    context->ClientFormatDataResponse(context, &resp);

    return CHANNEL_RC_OK;
}

static UINT cliprdr_server_format_data_response(
    CliprdrClientContext* context,
    const CLIPRDR_FORMAT_DATA_RESPONSE* response)
{
    auto* session = cliprdr_get_session(context);
    if (!session)
        return CHANNEL_RC_OK;

    if (response->common.msgFlags != CB_RESPONSE_OK ||
        !response->requestedFormatData || response->common.dataLen < 2)
        return CHANNEL_RC_OK;

    const std::string utf8 = utf16le_to_utf8(
        response->requestedFormatData,
        static_cast<size_t>(response->common.dataLen));

    if (!utf8.empty())
    {
        std::lock_guard<std::mutex> lock(session->callback_mutex);
        if (session->clipboard_callback)
            session->clipboard_callback(session->user_data, utf8.c_str());
    }

    return CHANNEL_RC_OK;
}

// ---------------------------------------------------------------------------
// Paint callbacks
// ---------------------------------------------------------------------------

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
    const int stride = width * BGRA32_BYTES_PER_PIXEL;
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

    auto* session = get_session(instance->context);

    // Drive redirection
    if (session && !session->drives.empty())
    {
        freerdp_settings_set_bool(
            instance->context->settings, FreeRDP_DeviceRedirection, TRUE);
        for (const auto& d : session->drives)
        {
            auto* drive = static_cast<RDPDR_DRIVE*>(
                std::calloc(1, sizeof(RDPDR_DRIVE)));
            if (!drive)
                continue;
            drive->device.Type = RDPDR_DTYP_FILESYSTEM;
            drive->device.Name = _strdup(d.name.c_str());
            drive->Path        = _strdup(d.path.c_str());
            drive->automount   = FALSE;
            freerdp_device_collection_add(
                instance->context->settings,
                reinterpret_cast<RDPDR_DEVICE*>(drive));
        }
    }

    // Load all enabled virtual channel addins (cliprdr, disp, drive, …)
    if (freerdp_client_load_addins(
            instance->context->channels,
            instance->context->settings) < 0)
    {
        if (session)
            notify_status(session, "Warning: some channel addins failed to load.");
    }

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

    // Wire up clipboard virtual channel if enabled
    if (freerdp_settings_get_bool(instance->context->settings, FreeRDP_RedirectClipboard))
    {
        auto* cliprdr = static_cast<CliprdrClientContext*>(
            freerdp_channels_get_static_channel_interface(
                instance->context->channels, CLIPRDR_SVC_CHANNEL_NAME));
        if (cliprdr)
        {
            cliprdr->custom                     = session;
            cliprdr->ServerCapabilities         = cliprdr_server_capabilities;
            cliprdr->ServerFormatList           = cliprdr_server_format_list;
            cliprdr->ServerFormatListResponse   = cliprdr_server_format_list_response;
            cliprdr->ServerFormatDataRequest    = cliprdr_server_format_data_request;
            cliprdr->ServerFormatDataResponse   = cliprdr_server_format_data_response;
            std::lock_guard<std::mutex> lock(session->callback_mutex);
            session->cliprdr = cliprdr;
        }
    }

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
        {
            std::lock_guard<std::mutex> lock(session->callback_mutex);
            session->cliprdr = nullptr;
        }
        const bool wasConnected = session->connected.exchange(false);
        notify_status(session, "RDP disconnected.");
        notify_state(session, RdpBridge_State_Disconnected);
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
    notify_state(session, RdpBridge_State_Connecting);

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
        freerdp_settings_set_string(settings, FreeRDP_Domain,
            session->domain.empty() ? "" : session->domain.c_str());
        freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth,
            static_cast<uint32_t>(session->width > 0 ? session->width : 1024));
        freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight,
            static_cast<uint32_t>(session->height > 0 ? session->height : 768));
        {
            const uint32_t depth = (session->color_depth == 16 || session->color_depth == 24)
                ? static_cast<uint32_t>(session->color_depth) : 32u;
            freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, depth);
        }
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
        // Experience flags
        freerdp_settings_set_bool(settings, FreeRDP_DisableWallpaper,
            (session->experience_flags & RDPBRIDGE_EXPERIENCE_DISABLE_WALLPAPER) ? TRUE : FALSE);
        freerdp_settings_set_bool(settings, FreeRDP_DisableFullWindowDrag,
            (session->experience_flags & RDPBRIDGE_EXPERIENCE_DISABLE_FULL_WINDOW_DRAG) ? TRUE : FALSE);
        freerdp_settings_set_bool(settings, FreeRDP_DisableMenuAnims,
            (session->experience_flags & RDPBRIDGE_EXPERIENCE_DISABLE_MENU_ANIMS) ? TRUE : FALSE);
        freerdp_settings_set_bool(settings, FreeRDP_DisableThemes,
            (session->experience_flags & RDPBRIDGE_EXPERIENCE_DISABLE_THEMES) ? TRUE : FALSE);
        freerdp_settings_set_bool(settings, FreeRDP_AllowFontSmoothing,
            (session->experience_flags & RDPBRIDGE_EXPERIENCE_ENABLE_FONT_SMOOTHING) ? TRUE : FALSE);
        // Optional channels
        if (session->clipboard_callback)
            freerdp_settings_set_bool(settings, FreeRDP_RedirectClipboard, TRUE);
        if (session->want_disp_channel)
            freerdp_settings_set_bool(settings, FreeRDP_SupportDisplayControl, TRUE);
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
        notify_state(session, RdpBridge_State_Connected);
        break;
    }

    if (!session->connected)
    {
        session->running = false;
        if (!lastError.empty())
            set_error(session, lastError);
        notify_state(session, RdpBridge_State_Failed);
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

    // Guard against self-join: a callback fired from connection_thread must not
    // call Disconnect() synchronously (it would join the calling thread).
    if (session->worker.joinable() &&
        session->worker.get_id() != std::this_thread::get_id())
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

RDP_BRIDGE_API void RdpBridge_set_options(
    void* handle,
    const char* domain,
    int color_depth,
    uint32_t experience)
{
    auto* session = static_cast<RdpSession*>(handle);
    if (!session)
        return;
    session->domain           = domain ? domain : "";
    session->color_depth      = (color_depth == 16 || color_depth == 24) ? color_depth : 32;
    session->experience_flags = experience;
}

RDP_BRIDGE_API void RdpBridge_add_drive(
    void* handle,
    const char* name,
    const char* local_path)
{
    auto* session = static_cast<RdpSession*>(handle);
    if (!session || !name || !local_path)
        return;
    session->drives.push_back({name, local_path});
}

RDP_BRIDGE_API void RdpBridge_set_state_callback(
    void* handle,
    RdpBridge_StateCallback state_callback)
{
    auto* session = static_cast<RdpSession*>(handle);
    if (!session)
        return;
    std::lock_guard<std::mutex> lock(session->callback_mutex);
    session->state_callback = state_callback;
}

RDP_BRIDGE_API void RdpBridge_set_clipboard_callback(
    void* handle,
    RdpBridge_ClipboardCallback clipboard_callback)
{
    auto* session = static_cast<RdpSession*>(handle);
    if (!session)
        return;
    std::lock_guard<std::mutex> lock(session->callback_mutex);
    session->clipboard_callback = clipboard_callback;
}

RDP_BRIDGE_API int RdpBridge_clipboard_set_text(
    void* handle,
    const char* utf8_text)
{
    auto* session = static_cast<RdpSession*>(handle);
    if (!session || !session->connected)
        return -1;

    CliprdrClientContext* cliprdr = nullptr;
    {
        std::lock_guard<std::mutex> lock(session->callback_mutex);
        session->pending_local_text = utf8_text ? utf8_text : "";
        cliprdr = session->cliprdr;
    }

    if (cliprdr)
    {
        cliprdr_announce_formats(cliprdr);
        return 0;
    }

    return -1;
}

RDP_BRIDGE_API int RdpBridge_resize(
    void* handle,
    int width,
    int height)
{
    auto* session = static_cast<RdpSession*>(handle);
    if (!session)
        return -1;

    // Always update stored dimensions and mark DISP channel as wanted
    if (width > 0)  session->width  = width;
    if (height > 0) session->height = height;
    session->want_disp_channel = true;

    if (!session->connected || !session->instance || !session->instance->context)
        return -1;

    auto* disp = static_cast<DispClientContext*>(
        freerdp_channels_get_static_channel_interface(
            session->instance->context->channels, DISP_CHANNEL_NAME));
    if (!disp || !disp->SendMonitorLayout)
        return -1;

    DISPLAY_CONTROL_MONITOR_LAYOUT monitor{};
    monitor.Flags              = DISPLAY_CONTROL_MONITOR_PRIMARY;
    monitor.Left               = 0;
    monitor.Top                = 0;
    monitor.Width              = static_cast<UINT32>(width > 0 ? width : session->width);
    monitor.Height             = static_cast<UINT32>(height > 0 ? height : session->height);
    monitor.PhysicalWidth      = 0;
    monitor.PhysicalHeight     = 0;
    monitor.Orientation        = ORIENTATION_LANDSCAPE;
    monitor.DesktopScaleFactor = 100;
    monitor.DeviceScaleFactor  = 100;

    disp->SendMonitorLayout(disp, 1, &monitor);
    return 0;
}

} // extern "C"
