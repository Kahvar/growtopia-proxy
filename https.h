// https.h
#ifndef HTTPS_H
#define HTTPS_H

// Portti jota HTTPS-palvelin kuuntelee (yleensa 443, koska
// hosts.c ohjaa growtopia1.com/growtopia2.com -> 127.0.0.1)
#define HTTPS_PORT 443

// Polku, johon client-yhdistelmä lahettaa POST-pyynnon
// server_data.php:n hakemiseksi
#define SERVER_DATA_PATH "/growtopia/server_data.php"

// Polku itse .pem-tiedostoihin (self-signed, koska Growtopia-client
// ei validoi sertifikaattiketjua tiukasti)
#define TLS_CERT_FILE "server.crt"
#define TLS_KEY_FILE  "server.key"

// Kiintea paikallinen portti jota SINUN ENet-proxysi kuuntelee (proxy.c:n
// clientHost). Taman EI pida riippua oikean palvelimen portista - se
// opitaan vasta live server_data.php-valityksesta (ks. getserver.c:n
// forward_server_data). Sama periaate kuin GTProxyn ServerConfig::port.
#define LOCAL_PROXY_PORT 17091

int https_start(void);
void https_poll(void);
void https_stop(void);

#endif // HTTPS_H