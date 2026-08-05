// packet.h
//
// Growtopian ENet-hyötykuorman rakenne. Tama on julkisesti dokumentoitu
// protokolla (sama jota GTProxy ja muut avoimen lahdekoodin proxyt
// kayttavat) - ei mitaan salaista.
//
// HUOM TARKKUUDESTA: kentat/offsetit alla vastaavat yleisesti jaettua
// GameUpdatePacket-layoutia, mutta Growtopia on muuttanut protokollaansa
// versioittain. ENNEN KUIN LUOTAT NAIHIN OFFSETEIHIN, vertaa niita
// proxy.c:n print_packet_hex()-tulosteeseen oikealla liikenteella -
// jos esim. pos_x/pos_y nayttavat jarjettomilta floateilta, offset on
// hieman pielessa ja rakenne pitaa saataa oikean datan mukaan.

#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>
#include <string.h>
#include <stddef.h>

#pragma pack(push, 1)

// Jokaisen ENet-paketin ensimmainen 4 tavua (little-endian) kertoo
// minka tyyppinen viesti on kyseessa.
typedef enum {
    NET_MESSAGE_UNKNOWN = 0,
    NET_MESSAGE_SERVER_HELLO,
    NET_MESSAGE_GENERIC_TEXT,      // teksti-pohjaiset viestit, esim. "action|..."
    NET_MESSAGE_GAME_MESSAGE,      // samanlainen, mutta pelin sisainen
    NET_MESSAGE_GAME_PACKET,       // taman jalkeen seuraa GameUpdatePacket
    NET_MESSAGE_ERROR,
    NET_MESSAGE_TRACK,
    NET_MESSAGE_CLIENT_LOG_REQUEST,
    NET_MESSAGE_CLIENT_LOG_RESPONSE,
} eNetMessageType;

// "Tank packet" -tyyppi (GameUpdatePacket.type-kentta) - kuvaa mita
// pelitoimintoa paketti edustaa. Laajenna listaa tarpeen mukaan, kun
// loydat lisaa arvoja liikenteesta.
typedef enum {
    PACKET_STATE = 0,
    PACKET_CALL_FUNCTION,
    PACKET_UPDATE_STATUS,
    PACKET_TILE_CHANGE_REQUEST,
    PACKET_SEND_MAP_DATA,
    PACKET_SEND_TILE_UPDATE_DATA,
    PACKET_SEND_TILE_UPDATE_DATA_MULTIPLE,
    PACKET_TILE_ACTIVATE_REQUEST,
    PACKET_TILE_APPLY_DAMAGE,
    PACKET_SEND_INVENTORY_STATE,
    PACKET_ITEM_ACTIVATE_REQUEST,
    PACKET_ITEM_ACTIVATE_OBJECT_REQUEST,
    PACKET_SEND_TILE_TREE_STATE,
    PACKET_MODIFY_ITEM_INVENTORY,
    PACKET_ITEM_CHANGE_OBJECT,
    PACKET_SEND_LOCK,
    PACKET_SEND_ITEM_DATABASE_DATA,
    PACKET_SEND_PARTICLE_EFFECT,
    PACKET_SET_ICON_STATE,
    PACKET_ITEM_EFFECT,
    PACKET_SET_CHARACTER_STATE,
    PACKET_PING_REPLY,
    PACKET_PING_REQUEST,
    PACKET_GOT_PUNCHED,
    PACKET_APP_CHECK_RESPONSE,
    PACKET_APP_INTEGRITY_FAIL,
    PACKET_DISCONNECT,
    PACKET_BATTLE_JOIN,
    PACKET_BATTLE_EVENT,
    PACKET_USE_DOOR,
    PACKET_SEND_PARENTAL,
    PACKET_GONE_FISHIN,
} eTankPacketType;

// GameUpdatePacket - seuraa suoraan 4-tavuista NET_MESSAGE_GAME_PACKET
// headeria. Tassa kulkee kaikki liike, iskut, esineiden aktivointi jne.
// Kokonaispituus tyypillisesti 56 tavua + mahdollinen extra_data
// (esim. chat-teksti), jonka pituuden extra_data_len kertoo.
typedef struct {
    uint8_t  type;
    uint8_t  pad[3];
    uint32_t net_id;
    int32_t  item_net_id;
    uint32_t flags;
    float    float_var;
    int32_t  int_data;
    float    pos_x;
    float    pos_y;
    float    pos_x2;
    float    pos_y2;
    uint8_t  pad2[4];
    int32_t  int_x;
    int32_t  int_y;
    uint32_t data_size;
} GameUpdatePacket;

#pragma pack(pop)

// Palauttaa paketin envelope-tyypin (ensimmaiset 4 tavua). Palauttaa
// NET_MESSAGE_UNKNOWN jos paketti on liian lyhyt sisaltaakseen sen.
static inline eNetMessageType packet_get_type(const uint8_t* data, size_t len) {
    if (len < 4) return NET_MESSAGE_UNKNOWN;
    uint32_t raw;
    memcpy(&raw, data, 4);
    return (eNetMessageType)raw;
}

// Palauttaa osoittimen GameUpdatePacket-structiin datan sisalla, tai
// NULL jos paketti ei ole NET_MESSAGE_GAME_PACKET tai on liian lyhyt.
// HUOM: osoitin viittaa suoraan alkuperaiseen bufferiin - GameUpdatePacket
// on pakattu (#pragma pack(1)) joten muokkaus paikallaan on turvallista.
static inline GameUpdatePacket* packet_as_game_packet(uint8_t* data, size_t len) {
    if (packet_get_type(data, len) != NET_MESSAGE_GAME_PACKET) return NULL;
    if (len < 4 + sizeof(GameUpdatePacket)) return NULL;
    return (GameUpdatePacket*)(data + 4);
}

#endif // PACKET_H