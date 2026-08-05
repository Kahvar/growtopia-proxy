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

// Helper to detect maintenance messages in server_data responses.
static int is_maintenance(const char *body) {
    if (!body) return 0;
    if (strstr(body, "#maint|") != NULL) return 1;
    if (strstr(body, "Server is under maintenance") != NULL) return 1;
    return 0;
}

// Try to extract an alternate server and port from the response body.
// We look for beta2_server/beta2_port, beta3_server/beta3_port, beta_server/beta_port,
// then fallback to any "server|..." / "port|..." pair.
// Returns 0 on success and fills out_host/out_port; non-zero if none found.
static int choose_alternate_server(const char *body, char *out_host, size_t out_host_size, int *out_port) {
    if (!body || !out_host || !out_port) return 1;
    const char *keys[] = { "beta2_server|", "beta2_port|", "beta3_server|", "beta3_port|", "beta_server|", "beta_port|" };
    for (int i = 0; i < (int)(sizeof(keys)/sizeof(keys[0])); i += 2) {
        const char *skey = keys[i];
        const char *pkey = keys[i+1];
        const char *spos = strstr(body, skey);
        const char *ppos = strstr(body, pkey);
        if (spos && ppos) {
            char hostbuf[256] = {0};
            int port = 0;
            if (sscanf(spos, "%255[^\r\n]", hostbuf) == 1) {
                const char *pipe = strchr(hostbuf, '|');
                if (pipe && pipe[1]) {
                    strncpy(out_host, pipe + 1, out_host_size - 1);
                    out_host[out_host_size - 1] = '\0';
                    // parse port line
                    char portbuf[64] = {0};
                    if (sscanf(ppos, "%63[^\r\n]", portbuf) == 1) {
                        const char *ppipe = strchr(portbuf, '|');
                        if (ppipe && ppipe[1]) {
                            port = atoi(ppipe + 1);
                            if (port > 0 && port <= 65535) {
                                *out_port = port;
                                // trim newline from out_host
                                char *nl = strpbrk(out_host, "\r\n");
                                if (nl) *nl = '\0';
                                return 0;
                            }
                        }
                    }
                }
            }
        }
    }

    // Last resort: look for any server|... and port|... pair
    const char *s1 = strstr(body, "server|");
    const char *p1 = strstr(body, "port|");
    if (s1 && p1) {
        char hostbuf[256] = {0};
        int port = 0;
        if (sscanf(s1, "server|%255[^\r\n]", hostbuf) == 1) {
            if (sscanf(p1, "port|%d", &port) == 1 && port > 0 && port <= 65535) {
                strncpy(out_host, hostbuf, out_host_size - 1);
                out_host[out_host_size - 1] = '\0';
                *out_port = port;
                return 0;
            }
        }
    }

    return 1; // none found
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

    // If the response indicates maintenance, try to pick an alternate server
    if (is_maintenance(full_buffer)) {
        printf("[FORWARD] Primary returned maintenance, searching for alternate in response...\n");
        char alt_host[256] = {0};
        int alt_port = 0;
        if (choose_alternate_server(full_buffer, alt_host, sizeof(alt_host), &alt_port) == 0) {
            printf("[FORWARD] Found alternate server in payload: %s:%d\n", alt_host, alt_port);
            char resolved_alt[64] = {0};
            if (resolve_via_doh(alt_host, resolved_alt, sizeof(resolved_alt)) == 0) {
                strncpy(g_target_ip, resolved_alt, sizeof(g_target_ip) - 1);
                g_target_ip[sizeof(g_target_ip) - 1] = '\0';
                printf("[DoH] Resolved alternate %s -> %s\n", alt_host, g_target_ip);
            } else {
                // Use hostname directly if DoH resolution failed
                strncpy(g_target_ip, alt_host, sizeof(g_target_ip) - 1);
                g_target_ip[sizeof(g_target_ip) - 1] = '\0';
                printf("[FORWARD] Could not DoH-resolve %s, will use hostname directly\n", alt_host);
            }
            g_target_port = alt_port;
        } else {
            // Fallback: try the secondary main endpoint (www.growtopia2.com)
            const char* secondary = "www.growtopia2.com";
            char resolved2[64] = {0};
            if (resolve_via_doh(secondary, resolved2, sizeof(resolved2)) == 0) {
                printf("[FORWARD] Trying secondary domain %s -> %s\n", secondary, resolved2);

                HINTERNET hInternet2 = InternetOpenA(
                    user_agent && user_agent[0] ? user_agent : "UbiApp/2.0 (Windows; Growtopia)",
                    INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0
                );
                if (hInternet2) {
                    DWORD timeout2 = REQUEST_TIMEOUT_MS;
                    InternetSetOptionA(hInternet2, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout2, sizeof(timeout2));
                    InternetSetOptionA(hInternet2, INTERNET_OPTION_SEND_TIMEOUT, &timeout2, sizeof(timeout2));
                    InternetSetOptionA(hInternet2, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout2, sizeof(timeout2));

                    HINTERNET hConnect2 = InternetConnectA(
                        hInternet2, resolved2, INTERNET_DEFAULT_HTTPS_PORT,
                        NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0
                    );
                    if (hConnect2) {
                        const char* acceptTypes2[] = { "*/*", NULL };
                        HINTERNET hRequest2 = HttpOpenRequestA(
                            hConnect2, "POST", "/growtopia/server_data.php",
                            NULL, NULL, acceptTypes2,
                            INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD, 0
                        );
                        if (hRequest2) {
                            DWORD security_flags = 0;
                            DWORD flags_size = sizeof(security_flags);
                            InternetQueryOptionA(hRequest2, INTERNET_OPTION_SECURITY_FLAGS, &security_flags, &flags_size);
                            security_flags |= SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
                            InternetSetOptionA(hRequest2, INTERNET_OPTION_SECURITY_FLAGS, &security_flags, sizeof(security_flags));

                            char headers2[256];
                            snprintf(headers2, sizeof(headers2), "Content-Type: application/x-www-form-urlencoded\r\nHost: %s\r\n", secondary);

                            BOOL sent2 = HttpSendRequestA(hRequest2, headers2, (DWORD)strlen(headers2), (LPVOID)client_body, (DWORD)client_body_len);
                            if (sent2) {
                                char buf2[4096];
                                size_t tot2 = 0;
                                char chunk2[2048];
                                DWORD br2 = 0;
                                while (InternetReadFile(hRequest2, chunk2, sizeof(chunk2) - 1, &br2) && br2 > 0) {
                                    if (tot2 + br2 >= sizeof(buf2) - 1) break;
                                    memcpy(buf2 + tot2, chunk2, br2);
                                    tot2 += br2;
                                }
                                buf2[tot2] = '\0';
                                printf("[FORWARD] Secondary response (%zu bytes):\n%s\n", tot2, buf2);

                                if (!is_maintenance(buf2)) {
                                    // parse server/port from secondary response
                                    const char* s_line = strstr(buf2, "server|");
                                    const char* p_line = strstr(buf2, "port|");
                                    if (s_line && p_line) {
                                        char temp_ip2[64] = {0};
                                        int temp_port2 = 0;
                                        if (sscanf(s_line, "server|%63[^\n]", temp_ip2) == 1) {
                                            trim_newline(temp_ip2);
                                            strncpy(g_target_ip, temp_ip2, sizeof(g_target_ip) - 1);
                                            g_target_ip[sizeof(g_target_ip) - 1] = '\0';
                                        }
                                        if (sscanf(p_line, "port|%d", &temp_port2) == 1) {
                                            g_target_port = temp_port2;
                                        }
                                        if (g_target_ip[0] != '\0' && g_target_port != 0) {
                                            printf("[FORWARD] Using server from secondary: %s:%d\n", g_target_ip, g_target_port);
                                        }
                                    }
                                } else {
                                    // try to pull alternate from secondary payload
                                    char alt2[256] = {0}; int alt2port = 0;
                                    if (choose_alternate_server(buf2, alt2, sizeof(alt2), &alt2port) == 0) {
                                        printf("[FORWARD] Found alternate in secondary payload: %s:%d\n", alt2, alt2port);
                                        char resolved_alt2[64] = {0};
                                        if (resolve_via_doh(alt2, resolved_alt2, sizeof(resolved_alt2)) == 0) {
                                            strncpy(g_target_ip, resolved_alt2, sizeof(g_target_ip) - 1);
                                            g_target_ip[sizeof(g_target_ip) - 1] = '\0';
                                            printf("[DoH] Resolved alternate %s -> %s\n", alt2, g_target_ip);
                                        } else {
                                            strncpy(g_target_ip, alt2, sizeof(g_target_ip) - 1);
                                            g_target_ip[sizeof(g_target_ip) - 1] = '\0';
                                            printf("[FORWARD] Could not DoH-resolve %s, will use hostname directly\n", alt2);
                                        }
                                        g_target_port = alt2port;
                                    }
                                }
                            } else {
                                fprintf(stderr, "[FORWARD] HttpSendRequest to secondary failed.\n");
                            }

                            InternetCloseHandle(hRequest2);
                        }
                        InternetCloseHandle(hConnect2);
                    }
                    InternetCloseHandle(hInternet2);
                }
            } else {
                fprintf(stderr, "[DoH] Could not resolve secondary domain %s\n", secondary);
            }
        }
    } else {
        // Not maintenance: parse server/port from the response body as before
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
    }

    if (g_target_ip[0] == '\0' || g_target_port == 0) {
        fprintf(stderr, "[FORWARD] No usable target server/port was found or resolved. Forwarding original maintenance response to client (no upstream connection set).\n");
        // Still rewrite the response the same way so client points to local proxy, but proxy won't have a valid upstream to connect.
    } else {
        printf("[FORWARD] Real server to connect: %s:%d (client will be redirected to 127.0.0.1:%d)\n", g_target_ip, g_target_port, local_proxy_port);
    }

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
        } else if (strncmp(line_start, "#maint|", 7) == 0) {
            // Skip maintenance marker lines entirely so client doesn't show maintenance UI
            // Do nothing (do not copy this line)
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