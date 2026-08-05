// getserver.c
// Growtopia server_data.php forwarding with maintenance-line stripping.

#include "getserver.h"
#include <windows.h>
#include <wininet.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "wininet.lib")

char g_target_ip[64] = "";
int g_target_port = 0;

#define REQUEST_TIMEOUT_MS 5000
#define TARGET_DOMAIN "www.growtopia2.com"

static void trim_newline(char* str) {
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == '\r' || str[len - 1] == '\n')) {
        str[len - 1] = '\0';
        len--;
    }
}

int resolve_via_doh(const char* domain, char* out_ip, size_t out_size) {
    int result = 1;

    HINTERNET hInternet = InternetOpenA(
        "GTProxy-DoH-Resolver/1.0",
        INTERNET_OPEN_TYPE_DIRECT,
        NULL, NULL, 0
    );
    if (!hInternet) return 1;

    DWORD timeout = REQUEST_TIMEOUT_MS;
    InternetSetOptionA(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionA(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    HINTERNET hConnect = InternetConnectA(
        hInternet, "cloudflare-dns.com", INTERNET_DEFAULT_HTTPS_PORT,
        NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0
    );
    if (!hConnect) {
        InternetCloseHandle(hInternet);
        return 1;
    }

    char path[256];
    snprintf(path, sizeof(path), "/dns-query?name=%s&type=A", domain);

    const char* acceptTypes[] = { "application/dns-json", NULL };
    HINTERNET hRequest = HttpOpenRequestA(
        hConnect, "GET", path, NULL, NULL, acceptTypes,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD, 0
    );
    if (!hRequest) {
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInternet);
        return 1;
    }

    const char* headers = "Accept: application/dns-json\r\n";
    BOOL sent = HttpSendRequestA(hRequest, headers, (DWORD)strlen(headers), NULL, 0);

    if (sent) {
        char full_buffer[4096];
        DWORD bytesRead = 0;
        if (InternetReadFile(hRequest, full_buffer, sizeof(full_buffer) - 1, &bytesRead) && bytesRead > 0) {
            full_buffer[bytesRead] = '\0';
            const char* needle = "\"data\":\"";
            const char* last_match = strstr(full_buffer, needle);
            if (last_match) {
                const char* value_start = last_match + strlen(needle);
                const char* value_end = strchr(value_start, '"');
                if (value_end && (size_t)(value_end - value_start) < out_size) {
                    size_t len = (size_t)(value_end - value_start);
                    memcpy(out_ip, value_start, len);
                    out_ip[len] = '\0';
                    printf("[DoH] %s -> %s\n", domain, out_ip);
                    result = 0;
                }
            }
        }
    }

    InternetCloseHandle(hRequest);
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);
    return result;
}

int forward_server_data(
    const char* client_body, size_t client_body_len,
    const char* user_agent,
    char* out_response_body, size_t out_response_size,
    int local_proxy_port
) {
    char resolved_ip[64];
    if (resolve_via_doh(TARGET_DOMAIN, resolved_ip, sizeof(resolved_ip)) != 0) return 1;

    HINTERNET hInternet = InternetOpenA(
        user_agent && user_agent[0] ? user_agent : "UbiApp/2.0 (Windows; Growtopia)",
        INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0
    );
    if (!hInternet) return 1;

    DWORD timeout = REQUEST_TIMEOUT_MS;
    InternetSetOptionA(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionA(hInternet, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionA(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    HINTERNET hConnect = InternetConnectA(
        hInternet, resolved_ip, INTERNET_DEFAULT_HTTPS_PORT,
        NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0
    );
    if (!hConnect) {
        InternetCloseHandle(hInternet);
        return 1;
    }

    const char* acceptTypes[] = { "*/*", NULL };
    HINTERNET hRequest = HttpOpenRequestA(
        hConnect, "POST", "/growtopia/server_data.php",
        NULL, NULL, acceptTypes,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD, 0
    );
    if (!hRequest) {
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInternet);
        return 1;
    }

    {
        DWORD security_flags = 0;
        DWORD flags_size = sizeof(security_flags);
        InternetQueryOptionA(hRequest, INTERNET_OPTION_SECURITY_FLAGS, &security_flags, &flags_size);
        security_flags |= SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
        InternetSetOptionA(hRequest, INTERNET_OPTION_SECURITY_FLAGS, &security_flags, sizeof(security_flags));
    }

    char headers[256];
    snprintf(headers, sizeof(headers),
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "Host: %s\r\n", TARGET_DOMAIN);

    BOOL sent = HttpSendRequestA(
        hRequest, headers, (DWORD)strlen(headers),
        (LPVOID)client_body, (DWORD)client_body_len
    );
    if (!sent) {
        InternetCloseHandle(hRequest);
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInternet);
        return 1;
    }

    char full_buffer[4096];
    size_t total_read = 0;
    char chunk[2048];
    DWORD bytesRead = 0;

    while (InternetReadFile(hRequest, chunk, sizeof(chunk) - 1, &bytesRead) && bytesRead > 0) {
        if (total_read + bytesRead >= sizeof(full_buffer) - 1) break;
        memcpy(full_buffer + total_read, chunk, bytesRead);
        total_read += bytesRead;
    }
    full_buffer[total_read] = '\0';

    InternetCloseHandle(hRequest);
    InternetCloseHandle(hConnect);
    InternetCloseHandle(hInternet);

    if (total_read == 0) return 1;

    printf("[FORWARD] Real server response (%zu bytes):\n%s\n", total_read, full_buffer);

    // Extract original server/port into globals for proxy.c
    const char* server_line = strstr(full_buffer, "server|");
    const char* port_line = strstr(full_buffer, "port|");

    if (server_line) {
        char temp_ip[64];
        if (sscanf(server_line, "server|%63[^\n]", temp_ip) == 1) {
            trim_newline(temp_ip);
            strncpy(g_target_ip, temp_ip, sizeof(g_target_ip) - 1);
            g_target_ip[sizeof(g_target_ip) - 1] = '\0';
        }
    }
    if (port_line) {
        int temp_port = 0;
        if (sscanf(port_line, "port|%d", &temp_port) == 1) {
            g_target_port = temp_port;
        }
    }

    if (g_target_ip[0] == '\0' || g_target_port == 0) return 1;

    printf("[FORWARD] Real server: %s:%d (client redirected to 127.0.0.1:%d)\n",
           g_target_ip, g_target_port, local_proxy_port);

    // Rewrite response: replace server/port, skip lines starting with '#'
    char rewritten[4096];
    size_t out_len = 0;
    char* line_start = full_buffer;

    while (line_start && *line_start && out_len < sizeof(rewritten) - 128) {
        char* line_end = strchr(line_start, '\n');
        size_t line_len = line_end ? (size_t)(line_end - line_start + 1) : strlen(line_start);

        if (strncmp(line_start, "server|", 7) == 0) {
            out_len += snprintf(rewritten + out_len, sizeof(rewritten) - out_len, "server|127.0.0.1\n");
        } else if (strncmp(line_start, "port|", 5) == 0) {
            out_len += snprintf(rewritten + out_len, sizeof(rewritten) - out_len, "port|%d\n", local_proxy_port);
        } else if (*line_start == '#') {
            // Skip maintenance/comment lines
        } else {
            if (out_len + line_len < sizeof(rewritten)) {
                memcpy(rewritten + out_len, line_start, line_len);
                out_len += line_len;
            }
        }

        line_start = line_end ? line_end + 1 : NULL;
    }
    rewritten[out_len] = '\0';

    strncpy(out_response_body, rewritten, out_response_size - 1);
    out_response_body[out_response_size - 1] = '\0';

    printf("[FORWARD] Rewritten response sent to client:\n%s\n", out_response_body);
    return 0;
}