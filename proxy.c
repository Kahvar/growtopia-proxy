#include "enet/enet.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "hosts.h"
#include "getserver.h"
#include "https.h"
#include "packet.h"
#include <signal.h>
#include <windows.h>


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

int main(void) {
    signal(SIGINT, handle_sigint);

    int exit_code = 0;

    // --- Vaihe 1: hosts-tiedoston muokkaus ---
    if (add_hosts() != 0) {
        fprintf(stderr, "add hosts failed.\n");
        return 1;
    }

    // --- Vaihe 2: HTTPS-palvelin kayntiin ENNEN ENetia ---
    // Growtopia-client tekee HTTPS-pyynnon server_data.php:hen ennen kuin
    // se yrittaa ENet-yhteytta, joten tama pitaa olla pystyssa jo tassa
    // vaiheessa. hosts.c on jo ohjannut growtopia1.com/growtopia2.com
    // 127.0.0.1:aan, joten pyynto paatyy tanne.
    //
    // HUOM: ei enaa erillista kaynnistysaikaista fetch_server_data()-
    // kutsua - g_target_ip/g_target_port opitaan LIVE:na vasta kun oikea
    // client tekee server_data.php-pyynnon (https.c -> getserver.c:n
    // forward_server_data(), sama periaate kuin GTProxyn web_server.cpp).
    if (https_start() != 0) {
        fprintf(stderr, "HTTPS server failed to start.\n");
        exit_code = 1;
        goto cleanup_hosts;
    }

    // --- Vaihe 4: ENet-alustus ---
    if (enet_initialize() != 0) {
        fprintf(stderr, "Failed to initialize ENet.\n");
        exit_code = 1;
        goto cleanup_https;
    }

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
    ENetHost* serverHost = enet_host_create(NULL, 1, 2, 0, 0);
    if (serverHost == NULL) {
        fprintf(stderr, "Failed to create server host.\n");
        exit_code = 1;
        goto cleanup_client_host;
    }

    ENetPeer* clientPeer = NULL; // oikea Growtopia client, asetetaan CONNECT:ssa
    ENetPeer* serverPeer = NULL; // yhteys oikeaan serveriin, asetetaan kun clientPeer yhdistaa

    ENetEvent event;

    // --- Paasilmukka ---
    while (running) {
        bool activity = false;

        // 1. Kasitellaan clientHostin eventit (Growtopia client <-> proxy)
        if (enet_host_service(clientHost, &event, 0) > 0) {
            activity = true;

            switch (event.type) {
                case ENET_EVENT_TYPE_CONNECT: {
                    char ip[64];
                    enet_address_get_host_ip(&event.peer->address, ip, sizeof(ip));
                    clientPeer = event.peer;
                    printf("Client yhdisti proxyyn (%s:%u)\n", ip, event.peer->address.port);

                    if (g_target_ip[0] == '\0' || g_target_port == 0) {
                        // Client yleensä tekee HTTP server_data.php -pyynnon ensiksi.
                        // Jos tasta kuitenkin johtuu race-condition (HTTP-vastaus
                        // ei ole viela ehtinyt asettaa g_target_ip/g_target_port),
                        // odotetaan lyhyen ajan ennen katkaisua jotta clientin
                        // pyyntö ehtii tulla perille.
                        const int max_wait_ms = 5000;
                        int waited = 0;
                        while ((g_target_ip[0] == '\0' || g_target_port == 0) && waited < max_wait_ms) {
                            Sleep(50);
                            waited += 50;
                        }

                        if (g_target_ip[0] == '\0' || g_target_port == 0) {
                            fprintf(stderr, "Oikean serverin osoitetta ei viela tiedossa - katkaistaan.\n");
                            enet_peer_disconnect(clientPeer, 0);
                            clientPeer = NULL;
                            break;
                        }
                    }

                    // Nyt vasta yhdistetaan oikeaan serveriin
                    ENetAddress serverAddress;
                    enet_address_set_host(&serverAddress, g_target_ip);
                    serverAddress.port = (enet_uint16)g_target_port;

                    serverPeer = enet_host_connect(serverHost, &serverAddress, 2, 0);
                    if (serverPeer == NULL) {
                        fprintf(stderr, "Yhteyden avaaminen serveriin epaonnistui.\n");
                        enet_peer_disconnect(clientPeer, 0);
                        clientPeer = NULL;
                    } else {
                        printf("Yhdistetaan serveriin %s:%d...\n", g_target_ip, g_target_port);
                    }
                    break;
                }

                case ENET_EVENT_TYPE_DISCONNECT:
                    printf("Client katkaisi yhteyden.\n");
                    clientPeer = NULL;
                    if (serverPeer != NULL) {
                        enet_peer_disconnect(serverPeer, 0);
                        serverPeer = NULL;
                    }
                    break;

                case ENET_EVENT_TYPE_RECEIVE: {
                    printf("[client -> server] ");
                    print_packet_hex(event.packet);

                    // Tassa voi lukea/muokata pakettia ennen edelleenlahetysta.
                    // Muokkaus tehdaan suoraan event.packet->data:n paalla,
                    // koska GameUpdatePacket on #pragma pack(1) - ei tarvitse
                    // rakentaa uutta bufferia ellei koko muutu.
                    GameUpdatePacket* tank = packet_as_game_packet(
                        event.packet->data, event.packet->dataLength);
                    if (tank != NULL) {
                        printf("  [tank] type=%u net_id=%d target=%d pos=(%.1f, %.1f)\n",
                               tank->type, tank->net_id, tank->target_net_id,
                               tank->pos_x, tank->pos_y);

                        // Esimerkki muokkauksesta - poistettu kommentista kun
                        // haluat oikeasti muuttaa jotain kenttaa:
                        // if (tank->type == PACKET_STATE) {
                        //     tank->pos_x += 0.0f; // esim. teleport-offset
                        // }
                    }

                    if (serverPeer != NULL) {
                        ENetPacket* forward = enet_packet_create(
                            event.packet->data,
                            event.packet->dataLength,
                            event.packet->flags
                        );
                        enet_peer_send(serverPeer, 0, forward);
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
                    printf("Yhteys serveriin muodostettu (%s:%u)\n", ip, event.peer->address.port);
                    break;
                }

                case ENET_EVENT_TYPE_DISCONNECT:
                    printf("Server katkaisi yhteyden.\n");
                    serverPeer = NULL;
                    if (clientPeer != NULL) {
                        enet_peer_disconnect(clientPeer, 0);
                        clientPeer = NULL;
                    }
                    break;

                case ENET_EVENT_TYPE_RECEIVE:
                    printf("[server -> client] ");
                    print_packet_hex(event.packet);

                    if (clientPeer != NULL) {
                        ENetPacket* forward = enet_packet_create(
                            event.packet->data,
                            event.packet->dataLength,
                            event.packet->flags
                        );
                        enet_peer_send(clientPeer, 0, forward);
                    }
                    enet_packet_destroy(event.packet);
                    break;
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