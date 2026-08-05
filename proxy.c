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

static const char* enet_event_type_name(ENetEventType type) {
    switch (type) {
        case ENET_EVENT_TYPE_NONE: return "NONE";
        case ENET_EVENT_TYPE_CONNECT: return "CONNECT";
        case ENET_EVENT_TYPE_DISCONNECT: return "DISCONNECT";
        case ENET_EVENT_TYPE_RECEIVE: return "RECEIVE";
        default: return "UNKNOWN";
    }
}

static volatile sig_atomic_t running = 1;

void handle_sigint(int sig) {
    (void)sig;
    running = 0;
}

int main(void) {
    signal(SIGINT, handle_sigint);
    int exit_code = 0;

    if (add_hosts() != 0) {
        fprintf(stderr, "add hosts failed.\n");
        return 1;
    }

    if (https_start() != 0) {
        fprintf(stderr, "HTTPS server failed to start.\n");
        exit_code = 1;
        goto cleanup_hosts;
    }

    if (enet_initialize() != 0) {
        fprintf(stderr, "Failed to initialize ENet.\n");
        exit_code = 1;
        goto cleanup_https;
    }

    // --- Client Host (Proxy Server) ---
    ENetAddress listenAddress;
    listenAddress.host = ENET_HOST_ANY;
    listenAddress.port = (enet_uint16)LOCAL_PROXY_PORT;

    ENetHost* clientHost = enet_host_create(&listenAddress, 1, 2, 0, 0);
    if (clientHost == NULL) {
        fprintf(stderr, "Failed to create client host.\n");
        exit_code = 1;
        goto cleanup_enet;
    }
    enet_host_compress_with_range_coder(clientHost);
    clientHost->checksum = enet_crc32;
    clientHost->usingNewPacket = 1; // ENABLE FOR GROWTOPIA CLIENT
    printf("[ENET] Proxy listening on port %u\n", clientHost->address.port);

    // --- Server Host (Proxy Client) ---
    ENetHost* serverHost = enet_host_create(NULL, 1, 2, 0, 0);
    if (serverHost == NULL) {
        fprintf(stderr, "Failed to create server host.\n");
        exit_code = 1;
        goto cleanup_client_host;
    }
    enet_host_compress_with_range_coder(serverHost);
    serverHost->checksum = enet_crc32;
    serverHost->usingNewPacketForServer = 1; // ENABLE FOR GROWTOPIA SERVER

    ENetPeer* clientPeer = NULL;
    ENetPeer* serverPeer = NULL;
    ENetEvent event;

    while (running) {
        bool activity = false;

        // 1. Handle events from the real Growtopia Client
        if (enet_host_service(clientHost, &event, 0) > 0) {
            activity = true;
            switch (event.type) {
                case ENET_EVENT_TYPE_CONNECT:
                    clientPeer = event.peer;
                    printf("[ENET] Client connected. Waiting for target info...\n");
                    
                    // Wait for HTTPS server to get target IP from server_data.php
                    int waited = 0;
                    while (g_target_ip[0] == '\0' && waited < 5000) { Sleep(50); waited += 50; }

                    if (g_target_ip[0] == '\0') {
                        printf("[ERROR] No target IP found from HTTPS!\n");
                        enet_peer_disconnect(clientPeer, 0);
                    } else {
                        ENetAddress serverAddress;
                        enet_address_set_host(&serverAddress, g_target_ip);
                        serverAddress.port = (enet_uint16)g_target_port;
                        serverPeer = enet_host_connect(serverHost, &serverAddress, 2, 0);
                        printf("[ENET] Connecting to real server %s:%d\n", g_target_ip, g_target_port);
                    }
                    break;

                case ENET_EVENT_TYPE_RECEIVE:
                    if (serverPeer && (serverPeer->state == ENET_PEER_STATE_CONNECTED || serverPeer->state == ENET_PEER_STATE_CONNECTION_SUCCEEDED)) {
                        ENetPacket* forward = enet_packet_create(event.packet->data, event.packet->dataLength, event.packet->flags);
                        enet_peer_send(serverPeer, 0, forward);
                    }
                    enet_packet_destroy(event.packet);
                    break;

                case ENET_EVENT_TYPE_DISCONNECT:
                    printf("[ENET] Client disconnected.\n");
                    if (serverPeer) enet_peer_disconnect(serverPeer, 0);
                    clientPeer = NULL;
                    break;
                default: break;
            }
        }

        // 2. Handle events from the real Growtopia Server
        if (enet_host_service(serverHost, &event, 0) > 0) {
            activity = true;
            switch (event.type) {
                case ENET_EVENT_TYPE_CONNECT:
                    printf("[ENET] Connected to real Growtopia server.\n");
                    break;

                case ENET_EVENT_TYPE_RECEIVE:
                    if (clientPeer) {
                        // Check for OnSendToServer redirect
                        GameUpdatePacket* tank = packet_as_game_packet(event.packet->data, event.packet->dataLength);
                        if (tank && tank->type == 1) { // CallFunction
                            unsigned char* extraData = event.packet->data + 4 + sizeof(GameUpdatePacket);
                            if (tank->data_size > 0 && strstr((char*)extraData, "OnSendToServer")) {
                                printf("[!] Intercepted OnSendToServer. Redirecting client to proxy...\n");
                                
                                // Rewrite the port in the packet to our local proxy port
                                char* portPtr = strstr((char*)extraData, "port|");
                                if (portPtr) {
                                    char newPortStr[32];
                                    sprintf(newPortStr, "port|%d", LOCAL_PROXY_PORT);
                                    memcpy(portPtr, newPortStr, strlen(newPortStr));
                                }
                            }
                        }

                        ENetPacket* forward = enet_packet_create(event.packet->data, event.packet->dataLength, event.packet->flags);
                        enet_peer_send(clientPeer, 0, forward);
                    }
                    enet_packet_destroy(event.packet);
                    break;

                case ENET_EVENT_TYPE_DISCONNECT:
                    printf("[ENET] Real server disconnected.\n");
                    if (clientPeer) enet_peer_disconnect(clientPeer, 0);
                    serverPeer = NULL;
                    break;
                default: break;
            }
        }

        if (!activity) Sleep(1);
    }

    enet_host_destroy(serverHost);
cleanup_client_host:
    enet_host_destroy(clientHost);
cleanup_enet:
    enet_deinitialize();
cleanup_https:
    https_stop();
cleanup_hosts:
    remove_hosts();
    return exit_code;
}