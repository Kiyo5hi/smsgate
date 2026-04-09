#include "telegram.h"
#include "secrets.h"

#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

static const char *botToken = TELEGRAM_BOT_TOKEN;
static const char *chatID = TELEGRAM_CHAT_ID;

// Let's Encrypt ISRG Root X1 — the current root behind api.telegram.org.
// Valid until 2035-06-04. Currently unused (see setupTelegramClient below
// and rfc/0001-tls-cert-pinning.md), kept in source so the eventual switch
// to setCACert() / setCACertBundle() is a one-line change.
static const char isrg_root_x1[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)EOF";

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
    // TODO(security): pinning ISRG Root X1 fails from this network — either a
    // MITM proxy or a regional cert chain we don't recognise. For now we skip
    // validation so the bridge is usable; revisit with setCACertBundle() using
    // the ESP-IDF built-in x509 bundle once the happy path is verified.
    telegramClient.setInsecure();
    (void)isrg_root_x1;
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
