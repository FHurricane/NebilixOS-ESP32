# Contributing to NebilixOS

Thank you for your interest in NebilixOS-ESP32.

## Development setup

- Use ESP-IDF 5.5.2 and target `esp32`.
- Build with `idf.py build`.
- Keep changes compatible with the Apache License 2.0.
- Do not commit build output, firmware binaries, credentials, NVS dumps,
  certificates or private keys.

## Changes and pull requests

Keep each change focused and explain how it was tested. GPIO changes must
preserve the protected-pin rules and must not allow scripts to bypass the Pin
Manager. Network-facing changes must validate input lengths, require the
administrator token where appropriate and avoid exposing secrets in responses.

For security vulnerabilities, follow `SECURITY.md` instead of opening a public
issue.
