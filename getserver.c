// getserver.c
//
// GTProxy-tyylinen elava server_data.php-valitys. EI ole enaa mitaan
// erillista kaynnistysaikaista "arvaushakua" - kaikki tapahtuu vasta
// kun oikea Growtopia-client oikeasti tekee pyynnon https.c:lle.
//
// Kulku (sama kuin GTProxyn src/core/web_server.cpp):
//   1. Client -> https.c: POST /growtopia/server_data.php
//   2. https.c -> forward_server_data(): resolvoi TARGET_DOMAIN:in oikean
//      IP:n DoH:lla, yhdistaa sinne HTTPS:lla, valittaa saman pyynnon
//   3. Oikea palvelin vastaa oikealla server|/port|-datalla
//   4. Talletetaan g_target_ip/g_target_port (proxy.c kayttaa naita
//      ulosmenevaan ENet-yhteyteen), korvataan server/port paikallisiksi
//   5. Muokattu vastaus https.c:lle -> takaisin clientille

#include "getserver.h"
#include <windows.h>
#include <wininet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "wininet.lib")

char g_target_ip[64] = "";
int g_target_port = 0;

#define REQUEST_TIMEOUT_MS 5000
#define TARGET_DOMAIN "www.growtopia1.com"

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
    if (!hInternet) {
        fprintf(stderr, "[DoH] InternetOpen epaonnistui. Virhekoodi: %lu\n", GetLastError());
        return 1;
    }

    DWORD timeout = REQUEST_TIMEOUT_MS;
    InternetSetOptionA(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionA(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    HINTERNET hConnect = InternetConnectA(
        hInternet, "cloudflare-dns.com", INTERNET_DEFAULT_HTTPS_PORT,
        NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0
    );
    if (!hConnect) {
        fprintf(stderr, "[DoH] InternetConnect epaonnistui. Virhekoodi: %lu\n", GetLastError());
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
        fprintf(stderr, "[DoH] HttpOpenRequest epaonnistui. Virhekoodi: %lu\n", GetLastError());
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInternet);
        return 1;
    }

    const char* headers = "Accept: application/dns-json\r\n";
    BOOL sent = HttpSendRequestA(hRequest, headers, (DWORD)strlen(headers), NULL, 0);

    if (sent) {
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

        if (total_read > 0) {
            const char* needle = "\"data\":\"";
            const char* search_pos = full_buffer;
            const char* last_match = NULL;

            while ((search_pos = strstr(search_pos, needle)) != NULL) {
                last_match = search_pos;
                search_pos += strlen(needle);
            }

            if (last_match) {
                const char* value_start = last_match + strlen(needle);
                const char* value_end = strchr(value_start, '"');
                if (value_end && (size_t)(value_end - value_start) < out_size) {
                    size_t len = (size_t)(value_end - value_start);
                    memcpy(out_ip, value_start, len);
                    out_ip[len] = '\0';
                    printf("[DoH] %s -> %s\n", domain, out_ip);
                    result = 0;
                } else {
                    fprintf(stderr, "[DoH] IP-osoite liian pitka tai muotoiltu vaarin.\n");
                }
            } else {
                fprintf(stderr, "[DoH] Ei loydetty 'data'-kenttaa vastauksesta:\n%s\n", full_buffer);
            }
        } else {
            fprintf(stderr, "[DoH] Tyhja vastaus Cloudflarelta.\n");
        }
    } else {
        fprintf(stderr, "[DoH] HttpSendRequest epaonnistui. Virhekoodi: %lu\n", GetLastError());
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
    if (resolve_via_doh(TARGET_DOMAIN, resolved_ip, sizeof(resolved_ip)) != 0) {
        fprintf(stderr, "[FORWARD] DoH-resoluutio epaonnistui, ei voida valittaa pyyntoa.\n");
        return 1;
    }

    int result = 1;

    HINTERNET hInternet = InternetOpenA(
        user_agent && user_agent[0] ? user_agent : "UbiApp/2.0 (Windows; Growtopia)",
        INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0
    );
    if (!hInternet) {
        fprintf(stderr, "[FORWARD] InternetOpen epaonnistui. Virhekoodi: %lu\n", GetLastError());
        return 1;
    }

    DWORD timeout = REQUEST_TIMEOUT_MS;
    InternetSetOptionA(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionA(hInternet, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionA(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    HINTERNET hConnect = InternetConnectA(
        hInternet, resolved_ip, INTERNET_DEFAULT_HTTPS_PORT,
        NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0
    );
    if (!hConnect) {
        fprintf(stderr, "[FORWARD] InternetConnect epaonnistui. Virhekoodi: %lu\n", GetLastError());
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
        fprintf(stderr, "[FORWARD] HttpOpenRequest epaonnistui. Virhekoodi: %lu\n", GetLastError());
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInternet);
        return 1;
    }

    // Ohitetaan sertifikaatin nimiristiriita - yhdistamme IP:hen mutta
    // sertti on TARGET_DOMAIN:ille, Host-header kertoo Akamaille kumpaa
    // domainia pyynto oikeasti koskee.
    {
        DWORD security_flags = 0;
        DWORD flags_size = sizeof(security_flags);
        InternetQueryOptionA(hRequest, INTERNET_OPTION_SECURITY_FLAGS, &security_flags, &flags_size);
        security_flags |= SECURITY_FLAG_IGNORE_CERT_CN_INVALID
                         | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
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
        fprintf(stderr, "[FORWARD] HttpSendRequest epaonnistui. Virhekoodi: %lu\n", GetLastError());
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

    if (total_read == 0) {
        fprintf(stderr, "[FORWARD] Tyhja vastaus oikealta palvelimelta.\n");
        return 1;
    }

    printf("[FORWARD] Oikean palvelimen vastaus (%zu tavua):\n%s\n", total_read, full_buffer);

    // --- Poimitaan oikea server/port talteen g_target_ip/g_target_port:iin
    //     (proxy.c:n serverHost tarvitsee naita ulosmenevaan yhteyteen) ---
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

    if (g_target_ip[0] == '\0' || g_target_port == 0) {
        fprintf(stderr, "[FORWARD] Vastauksesta ei loytynyt kelvollista server/port-kenttaa.\n");
        return 1;
    }

    printf("[FORWARD] Oikea pelipalvelin: %s:%d (client ohjataan 127.0.0.1:%d)\n",
           g_target_ip, g_target_port, local_proxy_port);

    // --- Rakennetaan clientille lahetettava vastaus: kaikki muu
    //     sailytetaan, server/port korvataan paikallisiksi. ---
    char rewritten[4096];
    size_t out_len = 0;
    const char* line_start = full_buffer;

    while (*line_start && out_len < sizeof(rewritten) - 128) {
        const char* line_end = strchr(line_start, '\n');
        size_t line_len = line_end ? (size_t)(line_end - line_start + 1) : strlen(line_start);

        if (strncmp(line_start, "server|", 7) == 0) {
            out_len += snprintf(rewritten + out_len, sizeof(rewritten) - out_len, "server|127.0.0.1\n");
        } else if (strncmp(line_start, "port|", 5) == 0) {
            out_len += snprintf(rewritten + out_len, sizeof(rewritten) - out_len, "port|%d\n", local_proxy_port);
        } else {
            if (out_len + line_len < sizeof(rewritten)) {
                memcpy(rewritten + out_len, line_start, line_len);
                out_len += line_len;
            }
        }

        if (!line_end) break;
        line_start = line_end + 1;
    }
    rewritten[out_len] = '\0';

    strncpy(out_response_body, rewritten, out_response_size - 1);
    out_response_body[out_response_size - 1] = '\0';

    result = 0;
    return result;
}