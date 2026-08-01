# NebilixOS-ESP32

NebilixOS è un ambiente embedded minimale basato su ESP-IDF e FreeRTOS che
installa ed esegue miniscript NBX per controllare le GPIO senza ricompilare ogni
volta il firmware.

> **Stato:** Core Edition 0.1.0 Developer Preview. Non utilizzare in produzione,
> impianti critici, sistemi di sicurezza fisica o dispositivi senza supervisione.

Copyright (c) 2026 Costa Fabio. Distribuito secondo la Apache License 2.0.

## Funzioni disponibili

- provisioning Wi-Fi tramite access point e pagina web;
- scansione delle reti Wi-Fi presenti;
- dashboard locale HTTPS su `https://nebilixos.local`;
- mini terminale WebSocket protetto da token amministrativo;
- installazione degli script NBX dopo il flash del firmware;
- Pin Manager grafico con associazioni persistenti;
- prenotazione dei GPIO e blocco dei conflitti tra script;
- certificato HTTPS e chiave ECDSA P-256 generati da ogni singola board.

NebilixOS Core utilizza un interprete con validazione preventiva delle
istruzioni. Non è ancora una macchina virtuale con isolamento completo della
memoria e non deve essere descritta come sandbox di sicurezza.

## Hardware supportato

La Developer Preview è destinata a ESP32 classico con 4 MB di flash, incluso il
modulo ESP-WROOM-32 usato nelle schede NodeMCU compatibili. ESP32-S3 e la futura
Ultimate Edition non sono ancora supportati da questa build.

## Primo avvio

1. Installa il firmware e riavvia la board.
2. Collegati alla rete Wi-Fi `NebilixOS` con password `nebilixos`.
3. Apri `http://192.168.4.1` e seleziona il router di casa.
4. Dopo il collegamento apri `https://nebilixos.local`.
5. Accetta l'avviso relativo al certificato self-signed generato dalla board.
6. Recupera il token amministrativo dalla console USB con `remote token`.

La password `nebilixos` protegge soltanto la rete temporanea di provisioning.
Il token amministrativo e l'identità HTTPS vengono generati sulla board e
salvati nella NVS; non sono inclusi nel firmware distribuito.

## Ambiente di sviluppo

Requisiti:

- ESP-IDF 5.5.2;
- target `esp32`;
- Python e tool installati da ESP-IDF;
- cavo USB dati.

Con Visual Studio Code installa l'estensione **Espressif IDF**, seleziona il
target `esp32` e la porta seriale della board.

Da un terminale ESP-IDF:

```sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/cu.usbserialXXXX flash monitor
```

Su macOS la porta può chiamarsi `cu.usbserial...` oppure
`cu.wchusbserial...`. Per uscire dal monitor premi `Ctrl+]`. Se la connessione
fallisce, tieni premuto **BOOT**, premi e rilascia **EN**, quindi rilascia
**BOOT** quando comincia la scrittura.

## Script NBX e Pin Manager

Gli script sono conservati nella partizione SPIFFS `scripts`, hanno una
dimensione massima di 16 KiB e dichiarano pin logici:

```text
# nebilix-script:1
# id=blink-led
# name=Blink LED
# version=1.0.0
# pin.led=output
# autostart=false

gpio.mode led output
gpio.write led high
sleep 500
gpio.write led low
```

L'utente associa poi `led` a un GPIO fisico dal Pin Manager. Le associazioni
sono persistenti, separate dal pacchetto e necessarie prima dell'avvio.

Istruzioni Core 0.1: `gpio.mode`, `gpio.write`, `sleep`, `repeat`/`end` e
`log`. Sono ammessi al massimo otto pin logici per script e due script in
esecuzione contemporanea.

GPIO esclusi o limitati su ESP-WROOM-32:

- GPIO 1 e 3: console seriale;
- GPIO 6-11: flash del modulo;
- GPIO 34-36 e 39: solo ingresso;
- GPIO 37 e 38: non esposti dal modulo supportato;
- GPIO 0, 2, 5, 12 e 15: pin di strapping, segnalati nell'interfaccia.

Esempio completo: [`examples/blink-led.nbx`](examples/blink-led.nbx).

## Marketplace gratuito

Il Marketplace ufficiale gratuito di NebilixOS è disponibile qui:

**[https://www.costafabio.it/NebilixOSMKTPLC.html](https://www.costafabio.it/NebilixOSMKTPLC.html)**

Dal Marketplace è possibile collegarsi alla board sulla rete locale, installare
gli script NBX disponibili e configurarne i GPIO. Tutti gli script pubblicati
sono gratuiti. Durante la Developer Preview, installa pacchetti soltanto dal
sito ufficiale e verifica sempre le informazioni e i permessi GPIO dichiarati.

La scheda Core include anche un Web Flasher per installare il firmware via USB
da Chrome o Edge su computer. Prima del flash verifica che la board sia una
ESP32 classica con almeno 4 MB di flash. L'opzione di cancellazione completa
rimuove anche configurazione Wi-Fi, token, certificato e script già installati.

## Console

```text
script list
script info blink-led
script bindings blink-led
script start blink-led
script stop blink-led
script remove blink-led
pin list
pin assign blink-led led 2
```

## API locale

Le API HTTPS richiedono `Authorization: Bearer <token>`:

```text
GET    /api/v1/scripts
GET    /api/v1/pins
GET    /api/v1/scripts/{id}/bindings
PUT    /api/v1/scripts/{id}          corpo: sorgente NBX
PUT    /api/v1/scripts/{id}/bindings corpo JSON: {"led":2}
POST   /api/v1/scripts/{id}/start
POST   /api/v1/scripts/{id}/stop
DELETE /api/v1/scripts/{id}
```

Le richieste cross-origin sono consentite soltanto dal sito ufficiale
`costafabio.it`. Il token resta necessario anche quando l'origine è ammessa.

## Sicurezza e limiti della Preview

- Nessuna chiave privata TLS statica è incorporata nel binario.
- Il certificato self-signed identifica `nebilixos.local`, ma richiede
  accettazione manuale nel browser.
- Password del router, token e chiave TLS risiedono nella NVS della board.
- Secure Boot, Flash Encryption, NVS Encryption e firma dei pacchetti NBX non
  sono ancora abilitati.
- Installare soltanto script di cui si conosce la provenienza.

Per segnalare una vulnerabilità consulta [`SECURITY.md`](SECURITY.md). Non
inserire credenziali o dettagli sfruttabili nelle issue pubbliche.

## Release e verifica SHA-256

I binari non vengono versionati nel repository. Le build pubbliche devono
essere distribuite tramite GitHub Releases o il sito ufficiale insieme al file
`.sha256`.

Su macOS o Linux:

```sh
shasum -a 256 NebilixOS-Core-0.1.0-Developer-Preview.zip
```

Il valore deve coincidere esattamente con quello pubblicato. Il checksum rileva
corruzioni o modifiche, ma non sostituisce una firma digitale della release.

## Struttura del progetto

```text
components/   kernel, Wi-Fi, script runtime, shell e servizi remoti
examples/     miniscript NBX di esempio
integration/  copia di integrazione del Marketplace web
main/         avvio di NebilixOS
```

## Contribuire e licenza

Leggi [`CONTRIBUTING.md`](CONTRIBUTING.md) prima di proporre modifiche. Il
codice originale è distribuito secondo la [Apache License 2.0](LICENSE); le
attribuzioni sono riportate in [`NOTICE`](NOTICE). Le modifiche pubblicate sono
riassunte in [`CHANGELOG.md`](CHANGELOG.md).
