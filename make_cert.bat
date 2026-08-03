@echo off
setlocal

echo ============================================
echo   Oman CA:n luonti + leaf-sertti proxylle
echo ============================================
echo.
echo HUOM: Tama skripti PYYTAA ADMIN-OIKEUKSIA lopussa
echo       ^(certutil -addstore^) - hyvaksy UAC-kysely.
echo.

REM --- Vaihe 1: juuri-CA:n avain + sertti (voimassa pitkaan, kerran riittaa) ---
echo [1/5] Luodaan juuri-CA...
openssl genrsa -out rootCA.key 4096
openssl req -x509 -new -nodes -key rootCA.key -sha256 -days 3650 ^
    -out rootCA.crt -subj "/CN=Local Proxy Dev CA"

if not exist rootCA.crt (
    echo [VIRHE] rootCA.crt ei syntynyt - katso ylla olevat OpenSSL-virheet.
    goto :fail
)
echo [OK] rootCA.key + rootCA.crt luotu.
echo.

REM --- Vaihe 2: leaf-sertin avain + CSR ---
echo [2/5] Luodaan leaf-sertin avain ja CSR...
openssl genrsa -out server.key 2048
openssl req -new -key server.key -out server.csr ^
    -subj "/CN=login.growtopiagame.com"

if not exist server.csr (
    echo [VIRHE] server.csr ei syntynyt.
    goto :fail
)
echo [OK] server.key + server.csr luotu.
echo.

REM --- Vaihe 3: SAN-tiedosto (nykyaikaiset TLS-pinot vaativat taman - pelkka CN ei riita) ---
echo [3/5] Kirjoitetaan SAN-laajennus...
(
    echo authorityKeyIdentifier=keyid,issuer
    echo basicConstraints=CA:FALSE
    echo keyUsage = digitalSignature, nonRepudiation, keyEncipherment, dataEncipherment
    echo subjectAltName = @alt_names
    echo.
    echo [alt_names]
    echo DNS.1 = login.growtopiagame.com
    echo DNS.2 = growtopiagame.com
    echo DNS.3 = www.growtopiagame.com
    echo DNS.4 = growtopia1.com
    echo DNS.5 = www.growtopia1.com
) > server.ext
echo [OK] server.ext kirjoitettu.
echo.

REM --- Vaihe 4: allekirjoitetaan leaf-sertti omalla CA:lla ---
echo [4/5] Allekirjoitetaan server.crt CA:lla...
openssl x509 -req -in server.csr -CA rootCA.crt -CAkey rootCA.key ^
    -CAcreateserial -out server.crt -days 825 -sha256 -extfile server.ext

if not exist server.crt (
    echo [VIRHE] server.crt ei syntynyt.
    goto :fail
)
echo [OK] server.crt luotu ja allekirjoitettu.
echo.

REM --- Vaihe 5: asenna juuri-CA Windowsin luotettuihin juurivarmentajiin ---
echo [5/5] Asennetaan rootCA.crt Windowsin luotettuihin juuriin...
echo       (tama nostaa UAC-kyselyn - hyvaksy se)
certutil -addstore -f "ROOT" rootCA.crt

if errorlevel 1 (
    echo [VIRHE] certutil epaonnistui - aja tama skripti admin-oikeuksin.
    goto :fail
)

echo.
echo ============================================
echo   VALMIS
echo ============================================
echo.
echo server.crt + server.key ovat nyt proxysi https.c:n kayttamat
echo tiedostot, allekirjoitettuna omalla CA:llasi, joka on nyt
echo asennettu Windowsin luotettuihin juurivarmentajiin.
echo.
echo Kaynnista proxy.exe uudestaan ja kokeile.
echo.
echo Jos Growtopia yhaan hylkaa sertin, client todennakoisesti EI
echo kayta Windowsin sertifikaattivarastoa vaan omaa sisaanrakennettua
echo CA-nippua ^(esim. mbedTLS/BoringSSL-pohjainen^) - silloin taman
echo tason CA-asennus ei riita, ja seuraava askel olisi clientin oma
echo cert-tarkistuksen ohittaminen, mihin en voi auttaa.
echo.
pause
exit /b 0

:fail
echo.
pause
exit /b 1