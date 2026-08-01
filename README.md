# NebilixOS per ESP32

Copyright (c) 2026 Costa Fabio. Distribuito con licenza Apache License 2.0.

Progetto base per una scheda NodeMCU con modulo ESP-WROOM-32, CPU dual-core e
convertitore USB/seriale CH340C. Usa esclusivamente ESP-IDF nativo e FreeRTOS;
Arduino non è una dipendenza del progetto.

NebilixOS è un sistema operativo embedded minimale per eseguire miniscript con
accesso controllato alle GPIO. Gli script potranno essere installati da un
Marketplace, verificati e avviati in una macchina virtuale isolata.

## Licenza

Il codice originale di NebilixOS è distribuito secondo la Apache License 2.0.
Consulta `LICENSE` per i termini completi e `NOTICE` per l'attribuzione.

## Ambiente consigliato (macOS)

1. Installa Visual Studio Code.
2. Installa l'estensione consigliata **Espressif IDF** quando VS Code la propone.
3. Apri la palette comandi (`Shift+Cmd+P`) e avvia
   `ESP-IDF: Open ESP-IDF Installation Manager`.
4. Usa ESP-IDF 5.5.2 e completa la configurazione proposta dall'estensione.
5. Apri questa cartella in VS Code e seleziona il target `esp32`.

Il percorso del progetto contiene spazi; è supportato dalle versioni moderne di
ESP-IDF. Non spostare o rinominare la cartella mentre VS Code è aperto.

## Compilazione e installazione

Collega la scheda con un cavo USB dati. In VS Code esegui nell'ordine:

- `ESP-IDF: Set Espressif Device Target` e scegli `esp32`;
- `ESP-IDF: Select Port to Use` e scegli la porta `cu.wchusbserial...` o
  `cu.usbserial...`;
- `ESP-IDF: Build your project`;
- `ESP-IDF: Flash your project`;
- `ESP-IDF: Monitor your device`.

Da un terminale ESP-IDF equivalente:

```sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/cu.wchusbserialXXXX flash monitor
```

Sostituisci la porta con quella rilevata sul Mac. Per uscire dal monitor premi
`Ctrl+]`.

## Modalità download e driver CH340C

Normalmente il circuito della scheda entra automaticamente in modalità download.
Se compare `Failed to connect`, tieni premuto **BOOT**, premi e rilascia **EN**, poi
rilascia **BOOT** quando inizia la scrittura.

macOS recenti spesso riconosce CH340C senza driver aggiuntivi. Prima di installare
driver di terze parti, verifica la presenza della porta con:

```sh
ls /dev/cu.*
```

All'avvio il firmware stampa modello del chip, quantità di flash rilevata e memoria
heap disponibile. Questi dati permettono di confermare le caratteristiche effettive
della scheda prima di definire il partizionamento definitivo di NebilixOS.

## Script NBX

Gli script sono installati dopo il firmware nella partizione SPIFFS `scripts` e non
richiedono un nuovo flash. Ogni file è limitato a 16 KiB e dichiara pin logici,
per esempio `# pin.led=output`. L'utente associa poi `led` a un GPIO fisico dal
Pin Manager della dashboard; l'assegnazione è persistente e separata dal pacchetto.
Un esempio completo è disponibile in `examples/blink-led.nbx`.

Istruzioni Core Edition 0.1: `gpio.mode`, `gpio.write`, `sleep`, `repeat`/`end` e
`log`. GPIO 6-11 sono sempre vietate perché collegate alla flash; GPIO 34-39 sono
accettate soltanto come ingressi. Possono essere eseguiti al massimo due script
contemporaneamente.

NebilixOS prenota i GPIO mentre uno script è in esecuzione, impedisce che due
script usino contemporaneamente lo stesso pin e lo ripristina quando lo script
termina. I GPIO 0, 2, 5, 12 e 15 sono segnalati come pin di boot e vanno scelti
soltanto conoscendone l'effetto sulla scheda.

Comandi shell e terminale web:

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

API HTTPS per l'app, protette da `Authorization: Bearer <token>`:

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

## Identità HTTPS della board

Il firmware distribuito non contiene certificati o chiavi private predefiniti.
Al primo avvio ogni ESP32 genera una chiave ECDSA P-256 e un certificato
self-signed per `nebilixos.local`, quindi li conserva nella propria NVS. Gli
avvii successivi riutilizzano la stessa identità. Un'installazione che cancella
l'intera flash genera una nuova identità e richiede di accettare nuovamente il
certificato nel browser.

La Developer Preview non abilita ancora la cifratura NVS, Secure Boot o Flash
Encryption: non è destinata a impianti critici o alla produzione.
