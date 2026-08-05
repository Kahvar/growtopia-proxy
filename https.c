// https.c
//
// Minimalistinen HTTPS-palvelin, joka:
//   1. Kuuntelee porttia 443 (ks. https.h)
//   2. Hyvaksyy TLS-yhteyden (OpenSSL, itse-allekirjoitettu sertti)
//   3. Lukee yhden HTTP-pyynnon
//   4. Jos polku on /growtopia/server_data.php, VALITTAA pyynnon
//      elavana oikealle Growtopia-palvelimelle (getserver.c:n
//      forward_server_data), ja korvaa vain server/port-kentat
//      paikallisiksi ennen vastaamista clientille - samalla tavalla
//      kuin GTProxy tekee. Ei mitaan staattista/arvattua vastausta.
//   5. Sulkee yhteyden (ei keep-alivea, clientti ei sita tarvitse)
//
// HUOM: Tama on tarkoituksella yksinkertainen - yksi saie per
// yhteys, ei mitaan asynkronista I/O:ta. Growtopia-client tekee
// vain yhden lyhyen POST-pyynnon kaynnistyksessa, joten tama riittaa.

#include "https.h"
#include "getserver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdbool.h>


#include <openssl/ssl.h>
#include <openssl/err.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "libssl.lib")
#pragma comment(lib, "libcrypto.lib")

static SOCKET       listen_socket = INVALID_SOCKET;
static SSL_CTX*      ssl_ctx      = NULL;
static HANDLE        accept_thread = NULL;
static volatile LONG running       = 0;

// g_target_ip/g_target_port TALLETETAAN forward_server_data():ssa
// (getserver.c) - proxy.c:n serverHost kayttaa niita ulosmenevaan
// yhteyteen oikeaan Growtopia-palvelimeen. https.c ei enaa itse
// paata niiden arvoa, se vain kutsuu forward_server_data():a.

// -----------------------------------------------------------------
// Apufunktiot
// -----------------------------------------------------------------

static int init_openssl_ctx(void) {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    const SSL_METHOD* method = TLS_server_method();
    ssl_ctx = SSL_CTX_new(method);
    if (!ssl_ctx) {
        fprintf(stderr, "[HTTPS] SSL_CTX_new Failed.\n");
        ERR_print_errors_fp(stderr);
        return 1;
    }

    if (SSL_CTX_use_certificate_file(ssl_ctx, TLS_CERT_FILE, SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "[HTTPS] Certificate loading failed (%s).\n", TLS_CERT_FILE);
        ERR_print_errors_fp(stderr);
        return 1;
    }

    if (SSL_CTX_use_PrivateKey_file(ssl_ctx, TLS_KEY_FILE, SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "[HTTPS] Private key loading failed (%s).\n", TLS_KEY_FILE);
        ERR_print_errors_fp(stderr);
        return 1;
    }

    if (!SSL_CTX_check_private_key(ssl_ctx)) {
        fprintf(stderr, "[HTTPS] Certificate and private key do not match.\n");
        return 1;
    }

    return 0;
}

// Poimii raa'an HTTP-pyynnon bodyn (tyhjan rivin jalkeinen osa).
// Palauttaa osoittimen bodyn alkuun requestin sisalla, tai NULL jos
// tyhjaa riviä ei loydy. *out_len saa bodyn pituuden.
static const char* extract_body(const char* request, int received, size_t* out_len) {
    const char* sep = strstr(request, "\r\n\r\n");
    if (!sep) {
        *out_len = 0;
        return NULL;
    }
    const char* body = sep + 4;
    *out_len = (size_t)(received - (body - request));
    return body;
}

// Poimii yhden headerin arvon (esim. "User-Agent: xyz\r\n" -> "xyz").
static void extract_header(const char* request, const char* header_name, char* out, size_t out_size) {
    out[0] = '\0';
    const char* pos = strstr(request, header_name);
    if (!pos) return;

    pos += strlen(header_name);
    while (*pos == ' ') pos++;

    const char* end = strstr(pos, "\r\n");
    size_t len = end ? (size_t)(end - pos) : strlen(pos);
    if (len >= out_size) len = out_size - 1;

    memcpy(out, pos, len);
    out[len] = '\0';
}

static void send_http_response(SSL* ssl, int status_code, const char* status_text,
                                const char* body) {
    char header[512];
    int body_len = (int)strlen(body);

    int header_len = snprintf(
        header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code, status_text, body_len
    );

    SSL_write(ssl, header, header_len);
    if (body_len > 0) {
        SSL_write(ssl, body, body_len);
    }
}

// Kasittelee yhden clientin: TLS-handshake, luku, vastaus, sulku.
// Vapauttaa itse clientSocket + SSL* lopuksi (siksi ei paluuarvoa).
static void handle_client(SOCKET client_socket) {
    SSL* ssl = SSL_new(ssl_ctx);
    SSL_set_fd(ssl, (int)client_socket);

    if (SSL_accept(ssl) <= 0) {
        // Handshake epaonnistui - client ei ehka luottanut serttiin,
        // tai kyseessa ei ollut TLS ollenkaan. Ei kaadeta koko palvelinta.
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        closesocket(client_socket);
        return;
    }

    char request[8192];
    int received = 0;
    int total_received = 0;
    int content_length = -1;
    const char* headers_end = NULL;

    while (true) {
        int chunk = SSL_read(ssl, request + total_received, sizeof(request) - 1 - total_received);
        if (chunk <= 0) break;
        total_received += chunk;
        request[total_received] = '\0';

        headers_end = strstr(request, "\r\n\r\n");
        if (headers_end) {
            char content_length_header[32] = "";
            extract_header(request, "Content-Length:", content_length_header, sizeof(content_length_header));
            if (content_length_header[0] != '\0') {
                content_length = atoi(content_length_header);
            }

            if (content_length < 0 ||
                total_received >= (int)((headers_end + 4) - request) + content_length) {
                break;
            }
        }

        if (total_received >= (int)(sizeof(request) - 1)) {
            break;
        }
    }

    received = total_received;
    if (received <= 0) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        closesocket(client_socket);
        return;
    }

    request[received] = '\0';

    // HUOM: yksinkertainen tarkistus - riittaa koska tiedamme etta
    // ainoa client tata palvelinta kayttava on Growtopia-peli, ei
    // tarvitse taydellista HTTP-parseria.
    printf("[HTTPS] Request received (%d tavua):\n%s\n", received, request);

    if (strncmp(request, "POST", 4) == 0 && strstr(request, SERVER_DATA_PATH)) {
        size_t body_len = 0;
        const char* body = extract_body(request, received, &body_len);

        char user_agent[256];
        extract_header(request, "User-Agent:", user_agent, sizeof(user_agent));

        /* Log the exact POST body for debugging */
        if (body && body_len > 0) {
            printf("[HTTPS] server_data POST body (len=%zu): %.*s\n", body_len, (int)body_len, body);
        } else {
            printf("[HTTPS] server_data POST body: <empty>\n");
        }

        /* Prepare a writable copy so we can optionally override the version parameter.
           Use PROXY_OVERRIDE_VERSION env var to control the override. */
        char* body_to_send = NULL;
        size_t body_to_send_len = 0;
        if (body) {
            size_t buf_sz = body_len + 256; /* extra room for modifications */
            body_to_send = (char*)malloc(buf_sz);
            if (body_to_send) {
                memcpy(body_to_send, body, body_len);
                body_to_send[body_len] = '\0';
                body_to_send_len = body_len;

                const char* override_ver = getenv("PROXY_OVERRIDE_VERSION");
                if (override_ver && override_ver[0] != '\0') {
                    char* vpos = strstr(body_to_send, "version=");
                    if (vpos) {
                        char* val_start = vpos + strlen("version=");
                        char* val_end = strchr(val_start, '&');
                        size_t prefix_len = (size_t)(vpos - body_to_send);
                        size_t suffix_len = val_end ? strlen(val_end) : 0;
                        size_t new_len = prefix_len + strlen("version=") + strlen(override_ver) + suffix_len;
                        if (new_len < buf_sz) {
                            char* newbuf = (char*)malloc(new_len + 1);
                            if (newbuf) {
                                memcpy(newbuf, body_to_send, prefix_len);
                                memcpy(newbuf + prefix_len, "version=", strlen("version="));
                                memcpy(newbuf + prefix_len + strlen("version="), override_ver, strlen(override_ver));
                                if (val_end) memcpy(newbuf + prefix_len + strlen("version=") + strlen(override_ver), val_end, suffix_len);
                                newbuf[new_len] = '\0';
                                free(body_to_send);
                                body_to_send = newbuf;
                                body_to_send_len = new_len;
                                printf("[HTTPS] Overrode version param to: %s\n", override_ver);
                            } else {
                                fprintf(stderr, "[HTTPS] Failed to allocate buffer for version override.\n");
                            }
                        } else {
                            fprintf(stderr, "[HTTPS] Cannot override version: result would exceed buffer.\n");
                        }
                    } else {
                        /* Append version parameter if it does not exist */
                        size_t add_len = 1 + strlen("version=") + strlen(override_ver); /* &version=... */
                        if (body_to_send_len + add_len < buf_sz) {
                            body_to_send[body_to_send_len] = '&';
                            memcpy(body_to_send + body_to_send_len + 1, "version=", strlen("version="));
                            memcpy(body_to_send + body_to_send_len + 1 + strlen("version="), override_ver, strlen(override_ver));
                            body_to_send_len += add_len;
                            body_to_send[body_to_send_len] = '\0';
                            printf("[HTTPS] Appended version param: %s\n", override_ver);
                        } else {
                            fprintf(stderr, "[HTTPS] Cannot append version: result would exceed buffer.\n");
                        }
                    }
                }
            }
        }

        char response_body[4096];
        int forward_ok = 1;
        if (body && (body_to_send ? forward_server_data(body_to_send, body_to_send_len, user_agent,
                                                      response_body, sizeof(response_body),
                                                      LOCAL_PROXY_PORT)
                                   : forward_server_data(body, body_len, user_agent,
                                                         response_body, sizeof(response_body),
                                                         LOCAL_PROXY_PORT)) == 0) {
            forward_ok = 0;
        }
        if (body_to_send) free(body_to_send);

        if (body && forward_ok == 0) {
            printf("[HTTPS] Valitys onnistui, vastataan clientille.\n");
            printf("[HTTPS] server_data response to client:\n%s\n", response_body);
            send_http_response(ssl, 200, "OK", response_body);
        } else if (!body) {
            fprintf(stderr, "[HTTPS] No body extracted from request.\n");
            send_http_response(ssl, 400, "Bad Request", "");
        } else {
            fprintf(stderr, "[HTTPS] Valitys oikealle palvelimelle epaonnistui.\n");
            send_http_response(ssl, 502, "Bad Gateway", "");
        }
    } else {
        send_http_response(ssl, 404, "Not Found", "");
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    closesocket(client_socket);
}

// Saikeen paafunktio: accept()-looppi. Yksi saie per client,
// koska client-maara on aina hyvin pieni (kaytannossa 1 kerrallaan).
static DWORD WINAPI accept_loop(LPVOID param) {
    (void)param;

    while (InterlockedCompareExchange(&running, 0, 0)) {
        struct sockaddr_in client_addr;
        int addr_len = sizeof(client_addr);

        SOCKET client_socket = accept(listen_socket, (struct sockaddr*)&client_addr, &addr_len);
        if (client_socket == INVALID_SOCKET) {
            // Jos palvelin on pysaytetty, accept() palauttaa virheen
            // koska listen_socket suljettiin https_stop:ssa - se on ok.
            if (InterlockedCompareExchange(&running, 0, 0) == 0) {
                break;
            }
            continue;
        }

        // Kasitellaan client omassa saikeessaan ettei yksi hidas/jumissa
        // oleva yhteys tukkisi muita (vaikka kaytannossa niita on 0-1).
        HANDLE h = CreateThread(NULL, 0,
            (LPTHREAD_START_ROUTINE)handle_client,
            (LPVOID)(ULONG_PTR)client_socket, 0, NULL);
        if (h) {
            CloseHandle(h); // ei odoteta valmistumista, thread hoitaa itse sulkemisen
        } else {
            closesocket(client_socket);
        }
    }

    return 0;
}

// -----------------------------------------------------------------
// Julkinen rajapinta
// -----------------------------------------------------------------

int https_start(void) {
    printf("[HTTPS] Starting HTTPS server...\n");

    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "[HTTPS] WSAStartup epaonnistui.\n");
        return 1;
    }

    if (init_openssl_ctx() != 0) {
        WSACleanup();
        return 1;
    }

    listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == INVALID_SOCKET) {
        fprintf(stderr, "[HTTPS] socket() epaonnistui: %d\n", WSAGetLastError());
        SSL_CTX_free(ssl_ctx);
        WSACleanup();
        return 1;
    }

    // Sallitaan porttia uudelleenkayttaa jos prosessi kaatuu ja
    // kaynnistetaan heti uudelleen (muuten bind voi epaonnistua hetken).
    BOOL opt = TRUE;
    setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(HTTPS_PORT);

    if (bind(listen_socket, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[HTTPS] bind() epaonnistui porttiin %d: %d\n",
                HTTPS_PORT, WSAGetLastError());
        fprintf(stderr, "[HTTPS] Aja proxy admin-oikeuksilla (portti < 1024).\n");
        closesocket(listen_socket);
        SSL_CTX_free(ssl_ctx);
        WSACleanup();
        return 1;
    }

    if (listen(listen_socket, SOMAXCONN) == SOCKET_ERROR) {
        fprintf(stderr, "[HTTPS] listen() epaonnistui: %d\n", WSAGetLastError());
        closesocket(listen_socket);
        SSL_CTX_free(ssl_ctx);
        WSACleanup();
        return 1;
    }

    InterlockedExchange(&running, 1);

    accept_thread = CreateThread(NULL, 0, accept_loop, NULL, 0, NULL);
    if (!accept_thread) {
        fprintf(stderr, "[HTTPS] accept-saikeen luonti epaonnistui.\n");
        InterlockedExchange(&running, 0);
        closesocket(listen_socket);
        SSL_CTX_free(ssl_ctx);
        WSACleanup();
        return 1;
    }

    printf("[HTTPS] Listens port %d.\n", HTTPS_PORT);
    return 0;
}

void https_poll(void) {
    // Ei tarvita - accept_loop hoitaa yhteydet omassa saikeessaan.
    // Jatetty tanne jos joskus halutaan vaihtaa ei-threadattuun malliin.
}

void https_stop(void) {
    if (!InterlockedCompareExchange(&running, 0, 0))
        return;

    printf("[HTTPS] Stopping HTTPS server...\n");

    InterlockedExchange(&running, 0);

    // Suljetaan listen-socket jotta accept() jumittunut kutsu herää
    // virheeseen ja saie paasee poistumaan loopista.
    if (listen_socket != INVALID_SOCKET) {
        closesocket(listen_socket);
        listen_socket = INVALID_SOCKET;
    }

    if (accept_thread) {
        WaitForSingleObject(accept_thread, 2000);
        CloseHandle(accept_thread);
        accept_thread = NULL;
    }

    if (ssl_ctx) {
        SSL_CTX_free(ssl_ctx);
        ssl_ctx = NULL;
    }

    WSACleanup();
}