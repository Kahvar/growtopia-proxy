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
#include "proxy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

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
        fprintf(stderr, "[HTTPS] SSL_CTX_new epaonnistui.\n");
        ERR_print_errors_fp(stderr);
        return 1;
    }

    if (SSL_CTX_use_certificate_file(ssl_ctx, TLS_CERT_FILE, SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "[HTTPS] Sertifikaatin lataus epaonnistui (%s).\n", TLS_CERT_FILE);
        ERR_print_errors_fp(stderr);
        return 1;
    }

    if (SSL_CTX_use_PrivateKey_file(ssl_ctx, TLS_KEY_FILE, SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "[HTTPS] Yksityisavaimen lataus epaonnistui (%s).\n", TLS_KEY_FILE);
        ERR_print_errors_fp(stderr);
        return 1;
    }

    if (!SSL_CTX_check_private_key(ssl_ctx)) {
        fprintf(stderr, "[HTTPS] Sertifikaatti ja avain eivat vastaa toisiaan.\n");
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

    char request[4096];
    int received = SSL_read(ssl, request, sizeof(request) - 1);
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
    printf("[HTTPS] Pyynto vastaanotettu (%d tavua):\n%s\n", received, request);

    if (strncmp(request, "POST", 4) == 0 && strstr(request, SERVER_DATA_PATH)) {
        size_t body_len = 0;
        const char* body = extract_body(request, received, &body_len);

        char user_agent[256];
        extract_header(request, "User-Agent:", user_agent, sizeof(user_agent));

        char response_body[4096];
        if (body && forward_server_data(body, body_len, user_agent,
                                         response_body, sizeof(response_body),
                                         LOCAL_PROXY_PORT) == 0) {
            printf("[HTTPS] Valitys onnistui, tarkistetaan upstream-yhteys ennen vastausta.\n");

            // Try to ensure upstream ENet is reachable before replying to client.
            if (g_target_ip[0] != '\0' && g_target_port != 0) {
                if (attempt_upstream_connect(g_target_ip, g_target_port, 3000) == 0) {
                    printf("[HTTPS] Upstream %s:%d reachable - replying to client.\n", g_target_ip, g_target_port);
                    send_http_response(ssl, 200, "OK", response_body);
                } else {
                    fprintf(stderr, "[HTTPS] Upstream %s:%d not reachable within timeout - returning 502.\n", g_target_ip, g_target_port);
                    send_http_response(ssl, 502, "Bad Gateway", "");
                }
            } else {
                // Should not happen - forward_server_data should populate g_target_ip/g_target_port
                fprintf(stderr, "[HTTPS] No upstream target set after forward_server_data - returning 502.\n");
                send_http_response(ssl, 502, "Bad Gateway", "");
            }
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

    printf("[HTTPS] Kuuntelee porttia %d.\n", HTTPS_PORT);
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