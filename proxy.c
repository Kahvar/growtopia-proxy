#include "enet/enet.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "hosts.h"
#include "getserver.h"
#include "https.h"
#include "packet.h"
#include "proxy.h"
#include <signal.h>
#include <windows.h>
#include <stdlib.h>


static volatile sig_atomic_t running = 1;

void handle_sigint(int sig) {
    (void)sig;
    running = 0;
}

// Tulostaa paketin sisallon hex-muodossa - Growtopian paketit ovat binaarista
// dataa, joten %s/%.*s tulostaisi vain sotkua
static void print_packet_hex(const ENetPacket* packet) {
    printf("Packet:\n  size: %u bytes\n  ", (unsigned int)packet->dataLength);

    const unsigned char* data = (const unsigned char*)packet->data;
    for (size_t i = 0; i < packet->dataLength; i++) {
        printf("%02X ", data[i]);
        if ((i + 1) % 16 == 0 && i + 1 < packet->dataLength) {
            printf("\n  ");
        }
    }
    printf("\n");
}

// Connection mapping list - per-client upstream peers
typedef struct conn_map {
    ENetPeer* client;
    ENetPeer* upstream;
    int upstream_connected;
    struct conn_map* next;
} conn_map;

static conn_map* mappings = NULL;

static conn_map* add_mapping(ENetPeer* client, ENetPeer* upstream) {
    conn_map* m = (conn_map*)malloc(sizeof(conn_map));
    if (!m) return NULL;
    m->client = client;
    m->upstream = upstream;
    m->upstream_connected = 0;
    m->next = mappings;
    mappings = m;
    if (client) client->data = m;
    if (upstream) upstream->data = m;
    return m;
}

static void remove_mapping(conn_map* m) {
    if (!m) return;
    conn_map** cur = &mappings;
    while (*cur) {
        if (*cur == m) {
            *cur = m->next;
            free(m);
            return;
        }
        cur = &(*cur)->next;
    }
}

static conn_map* find_mapping_by_peer(ENetPeer* peer) {
    if (!peer) return NULL;
    conn_map* cur = mappings;
    while (cur) {
        if (cur->client == peer || cur->upstream == peer) return cur;
        cur = cur->next;
    }
    return NULL;
}

// Attempt a short ENet connection to verify upstream is reachable.
// Returns 0 on successful connect handshake within timeout_ms, non-zero otherwise.
int attempt_upstream_connect(const char* ip, int port, int timeout_ms) {
    if (!ip || port <= 0) return 1;

    ENetHost* tmpHost = enet_host_create(NULL, 1, 2, 0, 0);
    if (!tmpHost) return 1;

    ENetAddress addr;
    enet_address_set_host(&addr, ip);
    addr.port = (enet_uint16)port;

    ENetPeer* peer = enet_host_connect(tmpHost, &addr, 2, 0);
    if (!peer) {
        enet_host_destroy(tmpHost);
        return 1;
    }

    ENetEvent ev;
    int waited = 0;
    const int step = 50;
    while (waited < timeout_ms) {
        while (enet_host_service(tmpHost, &ev, step) > 0) {
            if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                // Connected successfully
                // Tear down the test connection gracefully
                enet_peer_disconnect(peer, 0);
                // Allow disconnect event to be processed
                enet_host_service(tmpHost, &ev, 100);
                enet_host_destroy(tmpHost);
                return 0;
            } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                enet_host_destroy(tmpHost);
                return 1;
            }
        }
        waited += step;
    }

    // timeout
    enet_peer_reset(peer);
    enet_host_destroy(tmpHost);
    return 1;
}

int main(void) {
    signal(SIGINT, handle_sigint);

    int exit_code = 0;

    // --- Vaihe 1: hosts-tiedoston muokkaus ---
    if (add_hosts() != 0) {
        fprintf(stderr, "add hosts failed.\n");
        return 1;
    }

    // --- Vaihe 2: HTTPS-palvelin kayntiin ---
    // Initialize ENet early so https.c can perform quick upstream checks
    // before replying to clients. ENet initialization is lightweight.
    if (enet_initialize() != 0) {
        fprintf(stderr, "Failed to initialize ENet.\n");
        exit_code = 1;
        goto cleanup_hosts;
    }

    if (https_start() != 0) {
        fprintf(stderr, "HTTPS server failed to start.\n");
        exit_code = 1;
        goto cleanup_enet;
    }

    // --- ENet already initialized above ---

    // --- Vaihe 4: clientHost - kuuntelee oikeaa Growtopia-clientia ---
    // Kiintea paikallinen portti (https.h:n LOCAL_PROXY_PORT) - EI
    // g_target_port:ia, koska sita ei viela tiedeta tassa vaiheessa
    // (se opitaan vasta live server_data.php-valityksesta).
    ENetAddress listenAddress;
    listenAddress.host = ENET_HOST_ANY;
    listenAddress.port = (enet_uint16)LOCAL_PROXY_PORT;

    ENetHost* clientHost = enet_host_create(&listenAddress, 32, 2, 0, 0);
    if (clientHost == NULL) {
        fprintf(stderr, "Failed to create client host.\n");
        exit_code = 1;
        goto cleanup_enet;
    }
    printf("Proxy kuuntelee portissa %d (odottaa Growtopia clientia)...\n", listenAddress.port);

    // --- Vaihe 5: serverHost - kayttaa proxy -> oikea server -yhteyteen ---
    // Tama luodaan heti, mutta yhteytta EI oteta viela - se tapahtuu
    // vasta kun clientHost saa CONNECT-eventin
    ENetHost* serverHost = enet_host_create(NULL, 32, 2, 0, 0);
    if (serverHost == NULL) {
        fprintf(stderr, "Failed to create server host.\n");
        exit_code = 1;
        goto cleanup_client_host;
    }

    // --- Paasilmukka ---
    ENetEvent event;
    while (running) {
        bool activity = false;

        // 1. Kasitellaan clientHostin eventit (Growtopia client <-> proxy)
        if (enet_host_service(clientHost, &event, 0) > 0) {
            activity = true;

            switch (event.type) {
                case ENET_EVENT_TYPE_CONNECT: {
                    char ip[64];
                    enet_address_get_host_ip(&event.peer->address, ip, sizeof(ip));
                    ENetPeer* client = event.peer;
                    printf("Client yhdisti proxyyn (%s:%u)\n", ip, event.peer->address.port);

                    if (g_target_ip[0] == '\0' || g_target_port == 0) {
                        const int max_wait_ms = 5000;
                        int waited = 0;
                        while ((g_target_ip[0] == '\0' || g_target_port == 0) && waited < max_wait_ms) {
                            Sleep(50);
                            waited += 50;
                        }

                        if (g_target_ip[0] == '\0' || g_target_port == 0) {
                            fprintf(stderr, "Oikean serverin osoitetta ei viela tiedossa - katkaistaan.\n");
                            enet_peer_disconnect(client, 0);
                            break;
                        }
                    }

                    // Nyt yhdistetaan oikeaan serveriin
                    ENetAddress serverAddress;
                    enet_address_set_host(&serverAddress, g_target_ip);
                    serverAddress.port = (enet_uint16)g_target_port;

                    ENetPeer* upstream = enet_host_connect(serverHost, &serverAddress, 2, 0);
                    if (upstream == NULL) {
                        fprintf(stderr, "Yhteyden avaaminen serveriin epaonnistui.\n");
                        enet_peer_disconnect(client, 0);
                    } else {
                        add_mapping(client, upstream);
                        printf("Yhdistetaan serveriin %s:%d...\n", g_target_ip, g_target_port);
                    }
                    break;
                }

                case ENET_EVENT_TYPE_DISCONNECT: {
                    printf("Client katkaisi yhteyden.\n");
                    conn_map* m = find_mapping_by_peer(event.peer);
                    if (m) {
                        if (m->upstream) {
                            enet_peer_disconnect(m->upstream, 0);
                        }
                        remove_mapping(m);
                    }
                    break;
                }

                case ENET_EVENT_TYPE_RECEIVE: {
                    printf("[client -> server] ");
                    print_packet_hex(event.packet);

                    GameUpdatePacket* tank = packet_as_game_packet(
                        event.packet->data, event.packet->dataLength);
                    if (tank != NULL) {
                        printf("  [tank] type=%u net_id=%d coords=(%d,%d) pos=(%.1f, %.1f)\n",
                               tank->type, tank->net_id, tank->int_x, tank->int_y,
                               tank->pos_x, tank->pos_y);
                    }

                    conn_map* m = find_mapping_by_peer(event.peer);
                    if (m && m->upstream) {
                        ENetPacket* forward = enet_packet_create(
                            event.packet->data,
                            event.packet->dataLength,
                            event.packet->flags
                        );
                        enet_peer_send(m->upstream, 0, forward);
                    }
                    enet_packet_destroy(event.packet);
                    break;
                }
            }
        }

        // 2. Kasitellaan serverHostin eventit (proxy <-> oikea server)
        if (enet_host_service(serverHost, &event, 0) > 0) {
            activity = true;

            switch (event.type) {
                case ENET_EVENT_TYPE_CONNECT: {
                    char ip[64];
                    enet_address_get_host_ip(&event.peer->address, ip, sizeof(ip));
                    conn_map* m = find_mapping_by_peer(event.peer);
                    if (m) {
                        m->upstream_connected = 1;
                        printf("Yhteys serveriin muodostettu (%s:%u) - mapped to client peer\n", ip, event.peer->address.port);
                    } else {
                        printf("Yhteys serveriin muodostettu (%s:%u)\n", ip, event.peer->address.port);
                    }
                    break;
                }

                case ENET_EVENT_TYPE_DISCONNECT: {
                    printf("Server katkaisi yhteyden.\n");
                    conn_map* m2 = find_mapping_by_peer(event.peer);
                    if (m2) {
                        if (m2->client) {
                            enet_peer_disconnect(m2->client, 0);
                        }
                        // remove mapping
                        remove_mapping(m2);
                    }
                    break;
                }

                case ENET_EVENT_TYPE_RECEIVE: {
                    printf("[server -> client] ");
                    print_packet_hex(event.packet);

                    conn_map* m3 = find_mapping_by_peer(event.peer);
                    if (m3 && m3->client) {
                        ENetPacket* forward = enet_packet_create(
                            event.packet->data,
                            event.packet->dataLength,
                            event.packet->flags
                        );
                        enet_peer_send(m3->client, 0, forward);
                    }
                    enet_packet_destroy(event.packet);
                    break;
                }
            }
        }

        // Jos kummallakaan hostilla ei ollut mitaan tapahtumaa, nukutaan hetki
        // ettei silmukka syo koko CPU:ta busy-loopina
        if (!activity) {
            Sleep(4); // windows.h:n Sleep, millisekunteina - tai kayta enet_time_get -pohjaista throttlea
        }
    }

    printf("Closing proxy...\n");

    enet_host_destroy(serverHost);
    cleanup_client_host:
        enet_host_destroy(clientHost);
    cleanup_enet:
        enet_deinitialize();
    cleanup_https:
        https_stop();
    cleanup_hosts:
        if (remove_hosts() != 0) {
            fprintf(stderr, "remove_hosts epaonnistui - hosts-tiedosto voi olla sotkussa!\n");
            exit_code = 1;
        }

    printf("Proxy Closed.\n");
    return exit_code;
}