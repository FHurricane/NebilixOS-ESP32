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
