#ifndef GETSERVER_H
#define GETSERVER_H
#include <stddef.h>

// Naita paivittaa forward_server_data() aina kun oikea client tekee
// server_data.php-pyynnon - proxy.c:n serverHost kayttaa naita
// ulosmenevan ENet-yhteyden kohteena (oikea Growtopia-palvelin).
extern char g_target_ip[64];
extern int g_target_port;

// Resolvoi domainin oikean IP:n DNS-over-HTTPS:lla (Cloudflare),
// ohittaen OS:n hosts-tiedoston/DNS:n kokonaan. Palauttaa 0 onnistuessa.
int resolve_via_doh(const char* domain, char* out_ip, size_t out_size);

// GTProxy-tyylinen elava valitys: vastaanottaa oikean clientin
// server_data.php-pyynnon bodyn, resolvoi oikean palvelimen, valittaa
// pyynnon sinne sellaisenaan, lukee oikean vastauksen, paivittaa
// g_target_ip/g_target_port siita, ja kirjoittaa out_response_body:hen
// muokatun vastauksen (server/port korvattu paikallisella proxylla).
//
// Palauttaa 0 onnistuessa, 1 epaonnistuessa (kutsuja vastaa clientille
// virheella, esim. 502).
int forward_server_data(
    const char* client_body, size_t client_body_len,
    const char* user_agent,
    char* out_response_body, size_t out_response_size,
    int local_proxy_port
);

#endif // GETSERVER_H