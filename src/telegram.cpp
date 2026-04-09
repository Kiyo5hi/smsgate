#include "telegram.h"
#include "secrets.h"

#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

static const char *botToken = TELEGRAM_BOT_TOKEN;
static const char *chatID = TELEGRAM_CHAT_ID;

// CA trust bundle embedded into flash by platformio.ini:
//
//     board_build.embed_files = data/cert/x509_crt_bundle.bin
//
// The PlatformIO / ESP-IDF build wraps that file with `_binary_*_start`
// and `_binary_*_end` symbols, with the path mangled by replacing non
// alphanumerics with underscores. `setupTelegramClient()` hands the
// `_start` pointer to `WiFiClientSecure::setCACertBundle()` so TLS
// connections to api.telegram.org are verified against the roots in
// `data/cert/x509_crt_bundle.bin`. See rfc/0001-tls-cert-pinning.md and
// data/cert/README.md for the bundle source and regeneration steps.
extern const uint8_t rootca_crt_bundle_start[]
    asm("_binary_data_cert_x509_crt_bundle_bin_start");

static WiFiClientSecure telegramClient;

static bool keepTelegramClientAlive()
{
    if (telegramClient.connected())
    {
        return true;
    }

    Serial.println("Reconnecting to Telegram API server...");
    telegramClient.stop();
    bool ok = telegramClient.connect("api.telegram.org", 443);
    if (ok)
    {
        Serial.println("Reconnected to Telegram API server!");
    }
    else
    {
        Serial.println("Reconnection to Telegram API server failed!");
    }
    return ok;
}

bool setupTelegramClient()
{
#ifdef ALLOW_INSECURE_TLS
    // Opt-in escape hatch for deployment networks that MITM HTTPS or serve a
    // chain no reasonable public bundle will accept. Must never be the default
    // build. The warning line below is the canonical signal in a serial
    // capture that the firmware is running without TLS verification — do not
    // remove it. See rfc/0001-tls-cert-pinning.md.
    Serial.println("[SECURITY WARNING] TLS verification disabled via -DALLOW_INSECURE_TLS");
    telegramClient.setInsecure();
#else
    telegramClient.setCACertBundle(rootca_crt_bundle_start);
#endif
    telegramClient.setTimeout(15000);
    return keepTelegramClientAlive();
}

bool RealBotClient::sendMessage(const String &text)
{
    String url = String("/bot") + botToken + "/sendMessage";

    size_t size = JSON_OBJECT_SIZE(2) + text.length() + 256;
    DynamicJsonDocument doc(size);
    doc["chat_id"] = chatID;
    doc["text"] = text;

    String payload;
    serializeJson(doc, payload);

    if (!keepTelegramClientAlive())
    {
        return false;
    }

    telegramClient.print(String("POST ") + url + " HTTP/1.1\r\n");
    telegramClient.print("Host: api.telegram.org\r\n");
    telegramClient.print("Connection: keep-alive\r\n");
    telegramClient.print("Content-Type: application/json\r\n");
    telegramClient.print("Content-Length: ");
    telegramClient.print(payload.length());
    telegramClient.print("\r\n\r\n");
    telegramClient.print(payload);

    // Parse status line: "HTTP/1.1 200 OK"
    String statusLine = telegramClient.readStringUntil('\n');
    statusLine.trim();
    Serial.print("Telegram status: ");
    Serial.println(statusLine);

    bool httpOk = statusLine.indexOf(" 200") != -1;

    // Drain headers until blank line
    int contentLength = -1;
    while (telegramClient.connected() || telegramClient.available())
    {
        String line = telegramClient.readStringUntil('\n');
        if (line == "\r" || line.length() == 0)
            break;
        line.toLowerCase();
        if (line.startsWith("content-length:"))
        {
            contentLength = line.substring(15).toInt();
        }
    }

    // Drain the FULL response body. We must consume exactly contentLength
    // bytes (or until the connection closes) — otherwise leftover bytes
    // sit in the TLS buffer and the next keep-alive request will read them
    // back as the new HTTP status line, corrupting parsing.
    String body;
    unsigned long deadline = millis() + 8000;
    size_t target = contentLength > 0 ? (size_t)contentLength : 8192;
    while (body.length() < target && millis() < deadline)
    {
        if (telegramClient.available())
        {
            body += (char)telegramClient.read();
        }
        else if (contentLength <= 0 && !telegramClient.connected())
        {
            break; // server closed and no Content-Length to wait for
        }
        else
        {
            delay(2);
        }
    }

    bool apiOk = body.indexOf("\"ok\":true") != -1;
    return httpOk && apiOk;
}
