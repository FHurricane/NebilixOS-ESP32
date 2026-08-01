# Changelog

Le modifiche rilevanti di NebilixOS sono documentate in questo file.

## 0.1.0 - Developer Preview

### Aggiunto

- kernel minimale ESP-IDF/FreeRTOS per ESP32 classico;
- provisioning Wi-Fi con scansione SSID e password AP fissa `nebilixos`;
- dashboard HTTPS e mini terminale WebSocket;
- token amministrativo casuale persistente in NVS;
- runtime NBX con limiti di dimensione e istruzioni validate;
- installazione post-flash degli script nella partizione SPIFFS;
- Pin Manager grafico, binding persistenti e protezione dai conflitti GPIO;
- API HTTPS per Marketplace e futura applicazione mobile;
- Marketplace web con installazione di Blink LED e verifica SHA-256;
- Web Flasher USB per installare Core Edition direttamente da Chrome o Edge;
- recupero guidato del token amministrativo dalla seriale USB dopo il flash,
  senza persistenza nel browser.

### Sicurezza

- rimossi certificato e chiave privata TLS condivisi;
- generazione sulla board di identità ECDSA P-256 unica e persistente;
- CORS limitato al dominio ufficiale;
- esclusione da Git di build, NVS, credenziali, chiavi e artefatti firmware;
- rimossa la dipendenza JavaScript remota non versionata dal Marketplace.

### Limitazioni note

- Secure Boot, Flash Encryption, NVS Encryption e firma NBX non disponibili;
- certificato HTTPS self-signed;
- interprete NBX validato ma non ancora isolato come sandbox completa;
- supporto ufficiale limitato alla Core Edition per ESP32/ESP-WROOM-32.
