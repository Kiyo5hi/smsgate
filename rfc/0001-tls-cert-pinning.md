---
status: in-progress
created: 2026-04-09
updated: 2026-04-09
---

# RFC-0001: TLS certificate validation for api.telegram.org

## Motivation

`WiFiClientSecure` currently runs with `setInsecure()` (see
`setupTelegramClient()` in `src/main.cpp`). Any MITM on the path to
`api.telegram.org` could read the bot token and message contents.

## Current state

- The `isrg_root_x1` PEM constant in `main.cpp` holds the real Let's
  Encrypt ISRG Root X1 (valid until 2035-06-04), but it is **not**
  used — the call site is `telegramClient.setInsecure()` with a
  `(void)isrg_root_x1` to silence unused-warnings.
- During first board test, pinning ISRG Root X1 produced
  `mbedTLS -9984 X509 Certificate verification failed`. Two plausible
  reasons:
  1. The deployment WiFi (`OnAir-IoT`) does TLS interception with its
     own CA — common on captive / corporate networks.
  2. The Telegram CDN edge serving this region presents a chain that
     doesn't terminate at ISRG Root X1.
- `setInsecure()` was applied as a stopgap so the rest of the pipeline
  (modem → SMS decode → Telegram POST) could be validated end-to-end.

## Plan

Use ESP-IDF's bundled CA store rather than pinning a single root.
arduino-esp32 (which `espressif32@6.11.0` ships) exposes
`WiFiClientSecure::setCACertBundle()` which takes a binary cert bundle.
The framework already includes `data/cert/x509_crt_bundle.bin`.

Steps:

1. Add `board_build.embed_txtfiles` (or `embed_files`) to `platformio.ini`
   pointing at the framework's bundled `x509_crt_bundle` so the linker
   embeds it as a binary blob with the conventional symbol names
   (`_binary_..._start` / `_end`).
2. In `setupTelegramClient()`, replace `setInsecure()` with
   `setCACertBundle(rootca_crt_bundle_start)`.
3. Re-test on the deployment network. If verification still fails, dump
   the presented cert chain (fork `ssl_client.cpp` is overkill — easier
   to use `openssl s_client -connect api.telegram.org:443` from a
   machine that shares the same egress) and decide whether to add an
   intermediate manually or accept that this network requires
   `setInsecure()`.

## Notes for handover

- Do **not** silently delete the `isrg_root_x1` constant when switching
  to bundle mode. Keep it as a documented fallback in case the bundle
  approach has issues; the pinning path is one line to re-enable.
- If the deployment network turns out to be MITM'd, document that
  explicitly and have `setInsecure()` gated behind a build flag like
  `-DALLOW_INSECURE_TLS` so production builds fail closed.
- The bot token is logged on every send via the URL line in
  `sendBotMessage()`. Consider redacting before solving this RFC, since
  the whole point of TLS is keeping that token off the wire.
