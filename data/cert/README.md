# x509_crt_bundle.bin

This directory holds the CA trust store that `WiFiClientSecure::setCACertBundle()`
loads at runtime to verify TLS connections to `api.telegram.org`. See
`rfc/0001-tls-cert-pinning.md` for the design rationale.

The file `x509_crt_bundle.bin` is a binary blob in ESP-IDF's custom format
(number of certs, then per-cert subject + public key records). It is
**not** a PEM file and is not human-readable. It is embedded into flash
at build time via `board_build.embed_files` in `platformio.ini`.

## What's in this bundle

A narrow, deliberately small set of root CAs chosen to cover
`api.telegram.org` today and leave headroom for likely future rotations:

| Common name                                    | Why |
| ----------------------------------------------- | --- |
| Go Daddy Root Certificate Authority - G2        | **Current Telegram trust anchor.** The intermediate `Go Daddy Secure Certificate Authority - G2` that signs `*.telegram.org` chains back to this root. |
| Go Daddy Class 2 Certification Authority        | **Required for the chain that Telegram actually serves today.** See "The Class 2 gotcha" below. |
| DigiCert Global Root CA                         | Commonly used by CDNs; cheap insurance for a rotation. |
| DigiCert Global Root G2                         | Same. |
| ISRG Root X1                                    | Let's Encrypt RSA root. Telegram does not currently use LE but may rotate to it; keeping this in avoids a firmware rebuild if they do. |
| ISRG Root X2                                    | Let's Encrypt ECDSA root. Same future-proofing reason. |

The resulting binary is ~2.5 KB because `gen_crt_bundle.py` only keeps
the subject name and public key from each input cert — the rest of the
PEM (signature, validity, extensions) is discarded since mbedTLS rebuilds
the chain at verify time from the stored roots.

## The Class 2 gotcha

The chain `api.telegram.org` presents today (verified 2026-04-09) has
four certs:

```
0  CN=api.telegram.org                                        leaf
1  Go Daddy Secure Certificate Authority - G2                 intermediate
2  Go Daddy Root Certificate Authority - G2                   root (cross-signed)
3  Go Daddy Class 2 Certification Authority                   old self-signed root
```

Note that cert #2 is **not** the terminal self-signed root in the
presented chain — the server keeps pushing cert #3, the older Class 2
root, cross-signing it. That's a legacy GoDaddy trick to extend the
trust path for clients that only know the Class 2 root.

ESP-IDF's `esp_crt_bundle` verification is strict about this: when
mbedTLS asks its verify callback whether cert #3 is trusted, the
callback looks up cert #3's **issuer** (itself, since Class 2 is
self-signed) in the bundle. If the Class 2 root is not in the bundle,
lookup fails and the whole handshake is rejected — **even though
cert #2 (Root G2) is in the bundle and would otherwise be a valid
terminal trust anchor**. See
`libraries/WiFiClientSecure/src/esp_crt_bundle.c` in arduino-esp32 for
the callback.

So: as long as Telegram's edge keeps serving the cross-signed chain,
we **have to** keep Class 2 in the bundle. If Telegram ever stops
sending cert #3 (they'd see a few KB saved per handshake), Class 2
can be removed here — but until then, removing it breaks TLS.

The Class 2 root was removed from the Mozilla trust store in ~2023 for
policy reasons, not because of any known compromise. It's still safe
to pin in a firmware trust set that only talks to Telegram.

## Provenance of Class 2

The Class 2 cert included in `cacrt_narrow.pem` during regeneration
must be obtained from a source other than curl.se/cacert.pem, which no
longer ships it. The canonical source is GoDaddy's own repository:

```
https://certs.godaddy.com/repository/gd-class2-root.crt
```

SHA-256 fingerprint (verify after download):

```
C3:84:6B:F2:4B:9E:93:CA:64:27:4C:0E:C6:7C:1E:CC:5E:02:4F:FC:AC:D2:D7:40:19:35:0E:81:FE:54:6A:E4
```

This matches the serial 00, validity 2004-06-29 to 2034-06-29 cert
that has been in use by GoDaddy since 2004.

## Regeneration

Run this from a host that has Python + the `cryptography` package,
plus `openssl` and `curl` on PATH. It pulls the current Mozilla CA
list from curl.se (the canonical upstream extraction), filters it to
the roots we actually want, and concatenates the GoDaddy Class 2 root
(which Mozilla no longer ships — see above).

```bash
# 1. Grab Mozilla's current CA bundle (PEM).
curl -sL https://curl.se/ca/cacert.pem -o cacert_all.pem

# 2. Grab the GoDaddy Class 2 root (no longer in Mozilla).
curl -sL https://certs.godaddy.com/repository/gd-class2-root.crt -o gd-class2-root.crt
openssl x509 -in gd-class2-root.crt -noout -fingerprint -sha256
# Expect: C3:84:6B:F2:4B:9E:93:CA:64:27:4C:0E:C6:7C:1E:CC:5E:02:4F:FC:AC:D2:D7:40:19:35:0E:81:FE:54:6A:E4

# 3. Extract the five Mozilla roots by common name into a narrow PEM,
#    then append the Class 2 root.
python - <<'PY'
import re, subprocess, tempfile, os
WANTED = {
    "Go Daddy Root Certificate Authority - G2",
    "DigiCert Global Root CA",
    "DigiCert Global Root G2",
    "ISRG Root X1",
    "ISRG Root X2",
}
with open("cacert_all.pem") as f:
    text = f.read()
certs = re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----",
                   text, re.DOTALL)
picked = []
for c in certs:
    tf = tempfile.NamedTemporaryFile("w", suffix=".pem", delete=False)
    tf.write(c + "\n"); tf.close()
    out = subprocess.run(["openssl", "x509", "-in", tf.name,
                          "-noout", "-subject"],
                         capture_output=True, text=True).stdout
    os.unlink(tf.name)
    for w in WANTED:
        if f"CN = {w}" in out or f"CN={w}" in out:
            picked.append(c); break
with open("gd-class2-root.crt") as f:
    picked.append(f.read().strip())
with open("cacrt_narrow.pem", "w") as f:
    for c in picked:
        f.write(c.strip() + "\n")
print(f"picked {len(picked)} certs")  # expect 6
PY

# 4. Grab gen_crt_bundle.py from esp-idf and convert.
curl -sL https://raw.githubusercontent.com/espressif/esp-idf/release/v5.1/components/mbedtls/esp_crt_bundle/gen_crt_bundle.py \
    -o gen_crt_bundle.py
python gen_crt_bundle.py --input cacrt_narrow.pem
mv x509_crt_bundle x509_crt_bundle.bin
```

## When to regenerate

- Verification starts failing against `api.telegram.org` from a trusted
  (non-MITM) network. Telegram has rotated chains; confirm with
  `openssl s_client -connect api.telegram.org:443 -showcerts` from a
  clean egress, compare the root CN(s) in the presented chain to the
  `WANTED` list above, and add whatever's new.
- Mozilla distrusts one of the current bundle roots. Re-running the
  script picks up the then-current Mozilla list automatically, so any
  removed root just silently drops out of the bundle (assuming its
  name is still in `WANTED`). If a removed root is actually needed
  (as with Class 2 today), add a direct fetch step like the one above.

## Why not ship the full Mozilla bundle

The full Mozilla bundle is ~65 KB of flash and trusts ~145 CAs, which
is several orders of magnitude more attack surface than this device
needs. Pinning a narrow list that we review by hand when rotating is
both smaller and easier to reason about.
