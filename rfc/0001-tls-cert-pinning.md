---
status: implemented
created: 2026-04-09
updated: 2026-04-09
owner: claude-opus-4-6
---

# RFC-0001: TLS certificate validation for api.telegram.org

## Motivation

`WiFiClientSecure` currently runs with `setInsecure()` (see
`setupTelegramClient()` in `src/telegram.cpp`). Any MITM on the path to
`api.telegram.org` could read the bot token and message contents. This
RFC is a **hard prerequisite** for RFC-0003 (bidirectional Telegram →
SMS): an insecure channel there would let an attacker on the bridge's
network inject fake Telegram updates and use this device as a free SMS
relay.

## Current state

- The TLS client now lives in `src/telegram.cpp` as a file-static
  `WiFiClientSecure telegramClient;`. `setupTelegramClient()` calls
  `telegramClient.setInsecure()` and `(void)isrg_root_x1` to silence
  unused-warnings. Every change this RFC proposes lands in that one
  file; `main.cpp` is untouched.
- The `isrg_root_x1` PEM constant defined in `telegram.cpp` holds the
  real Let's Encrypt ISRG Root X1 (valid until 2035-06-04), but it is
  **not** used.
- During first board test, pinning ISRG Root X1 produced
  `mbedTLS -9984 X509 Certificate verification failed`. Two plausible
  reasons:
  1. The deployment WiFi (`OnAir-IoT`) does TLS interception with its
     own CA — common on captive / corporate networks.
  2. The Telegram CDN edge serving this region presents a chain that
     doesn't terminate at ISRG Root X1.
- `setInsecure()` was applied as a stopgap so the rest of the pipeline
  (modem → SMS decode → Telegram POST) could be validated end-to-end.
- After RFC-0007 lands, `telegram.cpp` becomes a `TelegramBotClient`
  class implementing `IBotClient`, and the `WiFiClientSecure` member
  will be a field on that class. The TLS setup call moves from the
  free function `setupTelegramClient()` into the class constructor
  (or an `init()` method invoked by the composition root in `main.cpp`).
  The substance of this RFC does not change — only where the three
  lines live.

## Plan

Use ESP-IDF's bundled CA store rather than pinning a single root.
arduino-esp32 (which `espressif32@6.11.0` ships) exposes
`WiFiClientSecure::setCACertBundle()` which takes a binary cert bundle.
This is **preferred over pinning ISRG Root X1 alone** because:

- The deployment network already rejected the single-root approach
  once. We don't know whether the cause is a different chain or MITM;
  the bundle handles the "different chain" case for free.
- Telegram can and does rotate its intermediate/root over time. A
  bundle tracks whichever CAs we choose to trust at build time, which
  can match (or be narrower than) what any normal browser would trust.

**Important correction.** `espressif32@6.11.0` / arduino-esp32 ships
only the **runtime support code** for CA bundles (`esp_crt_bundle.c/.h`
and the `WiFiClientSecure::setCACertBundle` wrapper). It does **not**
ship a prebuilt `data/cert/x509_crt_bundle.bin` for the project to pick
up automatically. Each project has to obtain a bundle, place it in its
own tree, and tell the linker to embed it. See arduino-esp32's
`libraries/WiFiClientSecure/README.md` for the canonical instructions.

Steps:

1. **Obtain a CA bundle.** Two paths, pick one:

   - **Easier: download a prebuilt bundle.** arduino-esp32 maintains a
     default bundle at
     `components/esp-mbedtls/data/esp_x509_crt_bundle` and related
     paths; the WiFiClientSecure example references one at
     `libraries/WiFiClientSecure/data/cert/x509_crt_bundle` in its own
     tree. Look in `espressif/arduino-esp32` and `espressif/esp-idf` on
     GitHub for a current path; the file is a concatenation of DER
     certs with a tiny header. Save whatever you download as
     `data/cert/x509_crt_bundle.bin` in this project.

   - **Reproducible: run `gen_crt_bundle.py`.** `esp-idf` ships
     `components/mbedtls/esp_crt_bundle/gen_crt_bundle.py`. From a host
     that has ESP-IDF installed:

     ```bash
     python $IDF_PATH/components/mbedtls/esp_crt_bundle/gen_crt_bundle.py \
         --input cacrt_all.pem \
         --output data/cert/x509_crt_bundle.bin
     ```

     `cacrt_all.pem` is a Mozilla CA list (or a curated subset — e.g.
     just the roots Telegram's edge currently chains to). Narrower is
     better for both binary size and attack surface.

2. **Place the bundle at `data/cert/x509_crt_bundle.bin`** in this
   project's tree (create `data/cert/` if it doesn't exist). Commit it
   to the repo: the bundle is ~150 KB, and pinning the trust set for
   reproducible builds is the entire point of this RFC. Do **not**
   gitignore it.

3. **Embed the bundle.** Add to `platformio.ini`, in the `[esp32dev_base]`
   section (both `T-A7670X` and `T-A7608X` inherit from it):

   ```ini
   board_build.embed_files = data/cert/x509_crt_bundle.bin
   ```

   PlatformIO feeds this through the component build, which produces
   the conventional linker symbols for the embedded blob.

4. **Wire it up in `src/telegram.cpp`** (today inside
   `setupTelegramClient()`; post-RFC-0007 inside the `TelegramBotClient`
   constructor or `init()`). Declare the extern symbol near the top of
   the file, next to the existing `isrg_root_x1` definition:

   ```cpp
   extern const uint8_t rootca_crt_bundle_start[]
       asm("_binary_data_cert_x509_crt_bundle_bin_start");
   ```

   (The symbol name is derived from the embedded file path with
   non-alphanumerics replaced by underscores. If you put the bundle at
   a different path, adjust accordingly.)

   Then replace the existing `telegramClient.setInsecure()` call with:

   ```cpp
   telegramClient.setCACertBundle(rootca_crt_bundle_start);
   ```

   This is a single-line change at a single call site.

5. **Add an insecure build-flag escape hatch.** Add a
   `-DALLOW_INSECURE_TLS` build flag, default **OFF**. When set, the
   `setupTelegramClient()` / `TelegramBotClient` path uses the existing
   `setInsecure()` code and logs a single conspicuous warning line at
   boot:

   ```
   [TLS] WARNING: ALLOW_INSECURE_TLS is set — certificate validation disabled
   ```

   This flag is the documented deployment workaround for networks that
   MITM HTTPS or serve a chain no reasonable public bundle will accept.
   It must never be the default, and any production build should have
   it off.

6. **Re-test on the deployment network.** If verification still fails,
   dump the presented cert chain with
   `openssl s_client -connect api.telegram.org:443` from a machine that
   shares the same egress, and decide whether to:
   (a) regenerate the bundle with an explicit intermediate included,
   (b) accept that this network requires `ALLOW_INSECURE_TLS` as a
       deployment workaround, with the warning visible in the boot log.

7. **Acceptance criterion for marking this RFC `implemented`:** either
   (a) `setCACertBundle` succeeds against `api.telegram.org` from the
   deployment network and a normal Telegram POST round-trips, OR
   (b) the user has explicitly opted in to `-DALLOW_INSECURE_TLS` with
   the warning line visible in the boot log. Ship no in-between state.

## Notes for handover

- Do **not** silently delete the `isrg_root_x1` constant when switching
  to bundle mode. Keep it as a documented fallback in case the bundle
  approach has issues; the pinning path is one line to re-enable.
- The `-DALLOW_INSECURE_TLS` escape hatch specified in the Plan is the
  canonical way to handle a MITM deployment network. It must be
  explicit in the user's `platformio.ini` (or `PLATFORMIO_BUILD_FLAGS`)
  — never the default — and it must log the boot-time warning line so
  it's obvious from a serial capture that the build is insecure.
- The bot token is **not** currently logged. The URL composed in
  `sendBotMessage()` (`src/telegram.cpp:85` region) exists as a
  `String` in RAM and is passed into the HTTPS request, but nothing
  writes it to `Serial`. The log output is limited to the HTTP status
  line. Be careful not to introduce a `Serial.println(url)` during
  future debugging — if someone ever adds verbose request logging,
  gate it behind a build flag and redact the token path segment
  (`/bot<token>/`). Track this as a "future redaction TODO" for
  telegram.cpp rather than as a current bug.
- RFC-0003 has a **hard dependency** on this RFC being `implemented`
  (not merely `accepted`). Do not start 0003 implementation work until
  the TLS path has been verified on real hardware (or the insecure
  flag has been explicitly opted in with the warning line visible in
  the boot log).
