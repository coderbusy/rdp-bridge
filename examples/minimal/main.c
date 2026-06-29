/**
 * Minimal RdpBridge usage example.
 *
 * This program does NOT actually connect to a real RDP server; it merely
 * demonstrates how to initialise a session, register callbacks, and tear
 * everything down cleanly.  The connection attempt will fail (no real server
 * is present) but all lifecycle calls are exercised.
 *
 * Build:
 *   cmake -S . -B build && cmake --build build
 *
 * Usage:
 *   rdp_example [host [port [username [password]]]]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rdp_bridge.h"

/* -------------------------------------------------------------------------
 * Callback implementations
 * ---------------------------------------------------------------------- */

static void on_frame(
    void* user_data,
    int width,
    int height,
    int stride,
    const uint8_t* bgra_pixels)
{
    (void)user_data;
    (void)bgra_pixels;
    printf("[frame] %dx%d  stride=%d\n", width, height, stride);
}

static void on_status(void* user_data, const char* message)
{
    (void)user_data;
    printf("[status] %s\n", message);
}

static void on_disconnect(void* user_data)
{
    (void)user_data;
    printf("[disconnect] session closed\n");
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

int main(int argc, char* argv[])
{
    const char* host     = argc > 1 ? argv[1] : "127.0.0.1";
    int         port     = argc > 2 ? atoi(argv[2]) : 3389;
    const char* username = argc > 3 ? argv[3] : "Administrator";
    const char* password = argc > 4 ? argv[4] : "";

    printf("RdpBridge minimal example\n");
    printf("  host=%s  port=%d  user=%s\n\n", host, port, username);

    /* 1. Create session -------------------------------------------------- */
    void* session = RdpBridge_create();
    if (!session)
    {
        fprintf(stderr, "RdpBridge_create() failed\n");
        return 1;
    }

    /* 2. Register callbacks ---------------------------------------------- */
    RdpBridge_set_callbacks(session, on_frame, on_status, on_disconnect, NULL);

    /* 3. Connect (non-blocking: starts a worker thread) ------------------ */
    int rc = RdpBridge_connect(session, host, port, username, password, 1024, 768);
    if (rc != 0)
    {
        fprintf(stderr, "RdpBridge_connect() returned %d: %s\n",
                rc, RdpBridge_get_last_error(session));
        RdpBridge_destroy(session);
        return 1;
    }

    /* 4. In a real application the event loop runs here.
     *    For this demo we just wait a moment then disconnect. */
#if defined(_WIN32)
    Sleep(3000);
#else
    {
        struct timespec ts = { 3, 0 };
        nanosleep(&ts, NULL);
    }
#endif

    /* 5. Disconnect & destroy -------------------------------------------- */
    printf("\nDisconnecting...\n");
    RdpBridge_disconnect(session);

    const char* lastErr = RdpBridge_get_last_error(session);
    if (lastErr && lastErr[0] != '\0')
        printf("Last error: %s\n", lastErr);

    RdpBridge_destroy(session);
    printf("Done.\n");
    return 0;
}
