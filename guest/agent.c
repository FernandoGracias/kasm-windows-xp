/*
 * XP Bridge Guest Agent
 * Connects to host daemon at 10.0.2.2:9500 over TCP.
 * Syncs clipboard bidirectionally and transfers files.
 * Compiled with: i686-w64-mingw32-gcc -o xp_bridge_agent.exe agent.c -lws2_32 -luser32 -lgdi32 -mwindows
 * For console debug: remove -mwindows flag
 */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")

#define HOST_IP "10.0.2.2"
#define HOST_PORT 9500
#define CHUNK_SIZE 32768
#define RECONNECT_DELAY 5000
#define CLIPBOARD_POLL_MS 500
#define HEARTBEAT_INTERVAL_MS 10000
#define UPLOAD_DIR "C:\\Uploads"
#define DOWNLOAD_DIR "C:\\Downloads"

/* Message types */
#define MSG_CLIPBOARD_TEXT 0x01
#define MSG_FILE_START     0x02
#define MSG_FILE_CHUNK     0x03
#define MSG_FILE_END       0x04
#define MSG_PING           0x05
#define MSG_PONG           0x06
#define MSG_SET_RESOLUTION 0x07

static SOCKET g_sock = INVALID_SOCKET;
static char g_last_clipboard[65536];
static int g_last_clipboard_len = 0;
static CRITICAL_SECTION g_cs;
static volatile int g_connected = 0;
static FILE *g_logfile = NULL;

static void logmsg(const char *fmt, ...) {
    (void)fmt;
    /* Logging disabled in production build */
}

static int send_exact(SOCKET s, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

static int recv_exact(SOCKET s, char *buf, int len) {
    int got = 0;
    while (got < len) {
        int n = recv(s, buf + got, len - got, 0);
        if (n <= 0) return -1;
        got += n;
    }
    return 0;
}

static int send_msg(SOCKET s, unsigned char type, const char *payload, int payload_len) {
    unsigned char hdr[5];
    int total = payload_len + 1;
    hdr[0] = (total >> 24) & 0xFF;
    hdr[1] = (total >> 16) & 0xFF;
    hdr[2] = (total >> 8) & 0xFF;
    hdr[3] = total & 0xFF;
    hdr[4] = type;
    if (send_exact(s, (char *)hdr, 5) < 0) return -1;
    if (payload_len > 0 && send_exact(s, payload, payload_len) < 0) return -1;
    return 0;
}

static int recv_msg(SOCKET s, unsigned char *type, char *payload, int *payload_len) {
    unsigned char lenbuf[4];
    if (recv_exact(s, (char *)lenbuf, 4) < 0) return -1;
    int total = (lenbuf[0] << 24) | (lenbuf[1] << 16) | (lenbuf[2] << 8) | lenbuf[3];
    if (total < 1 || total > 1048576) return -1; /* max 1MB message */
    unsigned char typebuf;
    if (recv_exact(s, (char *)&typebuf, 1) < 0) return -1;
    *type = typebuf;
    *payload_len = total - 1;
    if (*payload_len > 0) {
        if (recv_exact(s, payload, *payload_len) < 0) return -1;
    }
    return 0;
}

static void set_local_clipboard(const char *text, int len) {
    if (!OpenClipboard(NULL)) return;
    EmptyClipboard();
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, len + 1);
    if (hg) {
        char *p = (char *)GlobalLock(hg);
        memcpy(p, text, len);
        p[len] = 0;
        GlobalUnlock(hg);
        SetClipboardData(CF_TEXT, hg);
    }
    CloseClipboard();

    /* Update our tracking so we don't echo it back */
    EnterCriticalSection(&g_cs);
    if (len < (int)sizeof(g_last_clipboard)) {
        memcpy(g_last_clipboard, text, len);
        g_last_clipboard_len = len;
    }
    LeaveCriticalSection(&g_cs);
    logmsg("Set clipboard from host: %d bytes", len);
}

static int get_local_clipboard(char *buf, int bufsize) {
    if (!OpenClipboard(NULL)) return 0;
    HANDLE h = GetClipboardData(CF_TEXT);
    if (!h) { CloseClipboard(); return 0; }
    char *p = (char *)GlobalLock(h);
    if (!p) { CloseClipboard(); return 0; }
    int len = (int)strlen(p);
    if (len >= bufsize) len = bufsize - 1;
    memcpy(buf, p, len);
    buf[len] = 0;
    GlobalUnlock(h);
    CloseClipboard();
    return len;
}

static SOCKET connect_to_host(void) {
    struct sockaddr_in addr;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    addr.sin_family = AF_INET;
    addr.sin_port = htons(HOST_PORT);
    addr.sin_addr.s_addr = inet_addr(HOST_IP);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

/* Receive thread: handles incoming messages */
static DWORD WINAPI recv_thread(LPVOID param) {
    char *payload = (char *)malloc(1048576);
    if (!payload) return 1;
    FILE *recv_file = NULL;

    while (g_connected) {
        unsigned char type;
        int payload_len = 0;
        if (recv_msg(g_sock, &type, payload, &payload_len) < 0) {
            g_connected = 0;
            break;
        }

        switch (type) {
        case MSG_CLIPBOARD_TEXT:
            set_local_clipboard(payload, payload_len);
            break;

        case MSG_FILE_START: {
            /* Parse: 2-byte name len + name + 8-byte size */
            if (payload_len < 10) break;
            int name_len = (unsigned char)payload[0] << 8 | (unsigned char)payload[1];
            if (name_len + 10 > payload_len) break;
            char filename[260];
            if (name_len >= (int)sizeof(filename)) name_len = sizeof(filename) - 1;
            memcpy(filename, payload + 2, name_len);
            filename[name_len] = 0;

            /* Create upload dir if needed */
            CreateDirectoryA(UPLOAD_DIR, NULL);

            char fullpath[512];
            _snprintf(fullpath, sizeof(fullpath), "%s\\%s", UPLOAD_DIR, filename);
            recv_file = fopen(fullpath, "wb");
            logmsg("Receiving file: %s", fullpath);
            break;
        }

        case MSG_FILE_CHUNK:
            if (recv_file) fwrite(payload, 1, payload_len, recv_file);
            break;

        case MSG_FILE_END:
            if (recv_file) { fclose(recv_file); recv_file = NULL; }
            logmsg("File receive complete");
            break;

        case MSG_PING:
            send_msg(g_sock, MSG_PONG, NULL, 0);
            break;

        case MSG_PONG:
            break;

        case MSG_SET_RESOLUTION: {
            /* Payload: 4 bytes width (big-endian) + 4 bytes height (big-endian) */
            if (payload_len >= 8) {
                int width = ((unsigned char)payload[0] << 24) | ((unsigned char)payload[1] << 16) |
                            ((unsigned char)payload[2] << 8) | (unsigned char)payload[3];
                int height = ((unsigned char)payload[4] << 24) | ((unsigned char)payload[5] << 16) |
                             ((unsigned char)payload[6] << 8) | (unsigned char)payload[7];
                DEVMODEA dm;
                memset(&dm, 0, sizeof(dm));
                dm.dmSize = sizeof(dm);
                dm.dmPelsWidth = width;
                dm.dmPelsHeight = height;
                dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
                LONG result = ChangeDisplaySettingsA(&dm, CDS_UPDATEREGISTRY);
                logmsg("Set resolution %dx%d: result=%ld", width, height, result);
            }
            break;
        }
        }
    }

    if (recv_file) fclose(recv_file);
    free(payload);
    return 0;
}

/* Scan download dir for new files to send to host */
static void check_downloads(void) {
    static char seen_files[64][260];
    static int seen_count = 0;
    WIN32_FIND_DATAA fd;
    char pattern[512];
    _snprintf(pattern, sizeof(pattern), "%s\\*", DOWNLOAD_DIR);

    HANDLE hFind = FindFirstFileA(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        /* Check if already seen */
        int found = 0;
        int i;
        for (i = 0; i < seen_count; i++) {
            if (strcmp(seen_files[i], fd.cFileName) == 0) { found = 1; break; }
        }
        if (found) continue;

        /* New file - send it */
        if (seen_count < 64) {
            strncpy(seen_files[seen_count], fd.cFileName, 259);
            seen_count++;
        }

        char fullpath[512];
        _snprintf(fullpath, sizeof(fullpath), "%s\\%s", DOWNLOAD_DIR, fd.cFileName);
        FILE *f = fopen(fullpath, "rb");
        if (!f) continue;

        /* Get file size */
        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);

        /* Send FILE_START */
        int name_len = (int)strlen(fd.cFileName);
        char header[512];
        header[0] = (name_len >> 8) & 0xFF;
        header[1] = name_len & 0xFF;
        memcpy(header + 2, fd.cFileName, name_len);
        /* 8-byte big-endian file size */
        header[2 + name_len] = 0;
        header[3 + name_len] = 0;
        header[4 + name_len] = 0;
        header[5 + name_len] = 0;
        header[6 + name_len] = (file_size >> 24) & 0xFF;
        header[7 + name_len] = (file_size >> 16) & 0xFF;
        header[8 + name_len] = (file_size >> 8) & 0xFF;
        header[9 + name_len] = file_size & 0xFF;

        if (send_msg(g_sock, MSG_FILE_START, header, 10 + name_len) < 0) {
            fclose(f);
            g_connected = 0;
            break;
        }

        /* Send chunks */
        char chunk[CHUNK_SIZE];
        int n;
        while ((n = (int)fread(chunk, 1, CHUNK_SIZE, f)) > 0) {
            if (send_msg(g_sock, MSG_FILE_CHUNK, chunk, n) < 0) {
                g_connected = 0;
                break;
            }
        }
        fclose(f);

        if (g_connected)
            send_msg(g_sock, MSG_FILE_END, NULL, 0);

        logmsg("Sent file to host: %s (%ld bytes)", fd.cFileName, file_size);
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    InitializeCriticalSection(&g_cs);

    CreateDirectoryA(UPLOAD_DIR, NULL);
    CreateDirectoryA(DOWNLOAD_DIR, NULL);

    logmsg("XP Bridge Agent starting");

    while (1) {
        logmsg("Connecting to %s:%d...", HOST_IP, HOST_PORT);
        g_sock = connect_to_host();
        if (g_sock == INVALID_SOCKET) {
            logmsg("Connection failed, retrying in %dms", RECONNECT_DELAY);
            Sleep(RECONNECT_DELAY);
            continue;
        }

        g_connected = 1;
        logmsg("Connected!");

        /* Start receive thread */
        HANDLE hThread = CreateThread(NULL, 0, recv_thread, NULL, 0, NULL);

        /* Main loop: poll clipboard + check downloads */
        DWORD last_heartbeat = GetTickCount();
        char clipbuf[65536];

        while (g_connected) {
            /* Clipboard check */
            int len = get_local_clipboard(clipbuf, sizeof(clipbuf));
            if (len > 0) {
                int changed = 0;
                EnterCriticalSection(&g_cs);
                if (len != g_last_clipboard_len || memcmp(clipbuf, g_last_clipboard, len) != 0) {
                    memcpy(g_last_clipboard, clipbuf, len);
                    g_last_clipboard_len = len;
                    changed = 1;
                }
                LeaveCriticalSection(&g_cs);

                if (changed) {
                    if (send_msg(g_sock, MSG_CLIPBOARD_TEXT, clipbuf, len) < 0) {
                        g_connected = 0;
                        break;
                    }
                    logmsg("Sent clipboard to host: %d bytes", len);
                }
            }

            /* File download check */
            check_downloads();

            /* Heartbeat */
            DWORD now = GetTickCount();
            if (now - last_heartbeat > HEARTBEAT_INTERVAL_MS) {
                if (send_msg(g_sock, MSG_PING, NULL, 0) < 0) {
                    g_connected = 0;
                    break;
                }
                last_heartbeat = now;
            }

            Sleep(CLIPBOARD_POLL_MS);
        }

        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        WaitForSingleObject(hThread, 2000);
        CloseHandle(hThread);
        logmsg("Disconnected, reconnecting...");
        Sleep(RECONNECT_DELAY);
    }

    WSACleanup();
    return 0;
}
