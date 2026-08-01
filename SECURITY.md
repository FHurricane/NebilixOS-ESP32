# Security Policy

## Supported versions

NebilixOS Core 0.1.0 is a Developer Preview. It receives security fixes during
active development but is not intended for production systems, safety-critical
equipment or physical-security applications.

Secure Boot, Flash Encryption and NVS Encryption are not enabled in this
preview. Each board generates its own HTTPS private key and self-signed
certificate at first boot; no shared private key is distributed in the
firmware.

## Reporting a vulnerability

Please do not publish exploitable details in a public GitHub issue. Report the
problem privately to the project owner, Costa Fabio, through the contact method
published on https://www.costafabio.it/.

Include the affected version, ESP32 model, reproduction steps and the expected
impact. Do not include Wi-Fi credentials, administrator tokens, private keys or
personal data.

Reports will be acknowledged as soon as practical. After verification, a fix
and coordinated disclosure timeline will be prepared.

## Secrets and public reports

Never attach administrator tokens, router credentials, NVS partitions, serial
logs containing tokens, TLS private keys or unredacted network information to a
public issue. If a log is required, replace those values before sharing it.

The fixed provisioning password `nebilixos` is public by design and is not an
administrator credential. Provisioning is intended for initial, local setup;
do not leave an unconfigured board powered in an untrusted environment.

The official Marketplace can retrieve the administrator token after flashing
by opening the board's USB serial port and issuing `remote token`. Browser port
selection requires an explicit user action. The token is displayed only in the
current page, is not uploaded to the website and is not stored in browser
storage. Treat clipboard contents and screenshots as sensitive information.

## Current security boundaries

- NBX input is validated and size-limited, but the runtime is not yet a complete
  security sandbox.
- HTTPS protects transport after the user accepts the board's self-signed
  certificate; it does not provide public-CA identity verification.
- NVS contents are not encrypted in Core 0.1.0 Developer Preview.
- Marketplace package signing and verified updates are planned but unavailable.
