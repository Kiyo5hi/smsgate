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

int32_t RealBotClient::doSendMessage(const String &text)
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
        return 0;
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
    if (!httpOk || !apiOk)
    {
        return 0;
    }

    // Pull message_id out of the response. The Telegram sendMessage
    // success body looks like:
    //   {"ok":true,"result":{"message_id":1234,"from":{...},"chat":{...},...}}
    // We do a tiny stream-filtered parse so we don't have to fit the
    // whole envelope in a DynamicJsonDocument.
    StaticJsonDocument<64> filter;
    filter["result"]["message_id"] = true;
    DynamicJsonDocument respDoc(256);
    DeserializationError err = deserializeJson(respDoc, body,
                                               DeserializationOption::Filter(filter));
    if (err)
    {
        Serial.print("sendMessage: response message_id parse failed: ");
        Serial.println(err.c_str());
        // We still know the API said ok, so the SMS was forwarded —
        // just couldn't pull the id. Return a sentinel positive value
        // (1) so callers see "success without id" rather than failure.
        // Reply-target ring-buffer write will store this id; future
        // replies to that exact message can't route, but the original
        // forward did happen.
        return 1;
    }
    int32_t mid = respDoc["result"]["message_id"] | 0;
    if (mid <= 0)
    {
        return 1;
    }
    return mid;
}

bool RealBotClient::sendMessage(const String &text)
{
    return doSendMessage(text) > 0;
}

int32_t RealBotClient::sendMessageReturningId(const String &text)
{
    return doSendMessage(text);
}

// ---------- pollUpdates (RFC-0003) ----------

bool RealBotClient::pollUpdates(int32_t sinceUpdateId, int32_t timeoutSec,
                                std::vector<TelegramUpdate> &out)
{
    out.clear();

    // Build URL: /bot<token>/getUpdates?timeout=<n>[&offset=<m>]
    // Telegram interprets `offset = sinceUpdateId + 1` as "give me
    // everything strictly after this id". We pass offset only when
    // we have a watermark, so the very first poll after a fresh boot
    // gets the full unread queue.
    String url = String("/bot") + botToken + "/getUpdates?timeout=" + String(timeoutSec);
    if (sinceUpdateId > 0)
    {
        url += "&offset=";
        url += String((long)(sinceUpdateId + 1));
    }
    // Limit the response size — we only need a few entries per poll.
    url += "&limit=10";

    if (!keepTelegramClientAlive())
    {
        return false;
    }

    telegramClient.print(String("GET ") + url + " HTTP/1.1\r\n");
    telegramClient.print("Host: api.telegram.org\r\n");
    telegramClient.print("Connection: keep-alive\r\n");
    telegramClient.print("\r\n");

    // Allow at least timeoutSec + a generous slack to read the
    // response. Telegram parks the request on its side until either
    // an update arrives or the timeout fires.
    unsigned long readDeadline = millis() + (unsigned long)(timeoutSec * 1000) + 8000;

    String statusLine = telegramClient.readStringUntil('\n');
    statusLine.trim();
    Serial.print("Telegram getUpdates status: ");
    Serial.println(statusLine);
    bool httpOk = statusLine.indexOf(" 200") != -1;

    int contentLength = -1;
    while (telegramClient.connected() || telegramClient.available())
    {
        if (millis() > readDeadline)
        {
            Serial.println("getUpdates: header read timeout");
            return false;
        }
        String line = telegramClient.readStringUntil('\n');
        if (line == "\r" || line.length() == 0)
            break;
        line.toLowerCase();
        if (line.startsWith("content-length:"))
        {
            contentLength = line.substring(15).toInt();
        }
    }

    // Drain the body — same Content-Length-or-bust rule as sendMessage.
    String body;
    size_t target = contentLength > 0 ? (size_t)contentLength : 16384;
    while (body.length() < target && millis() < readDeadline)
    {
        if (telegramClient.available())
        {
            body += (char)telegramClient.read();
        }
        else if (contentLength <= 0 && !telegramClient.connected())
        {
            break;
        }
        else
        {
            delay(2);
        }
    }

    if (!httpOk)
    {
        Serial.println("getUpdates: HTTP non-200, dropping response");
        return false;
    }

    // Stream filter: extract only the fields RFC-0003 needs. Anything
    // else is dropped before it hits the document, so the doc capacity
    // can stay tiny even when the user sends long messages we don't
    // care about.
    StaticJsonDocument<256> filter;
    filter["ok"] = true;
    filter["result"][0]["update_id"] = true;
    filter["result"][0]["message"]["from"]["id"] = true;
    filter["result"][0]["message"]["chat"]["id"] = true;
    filter["result"][0]["message"]["text"] = true;
    filter["result"][0]["message"]["reply_to_message"]["message_id"] = true;

    // Cap the JSON document; we asked for limit=10 and only keep ~5
    // ints + 1 string per entry, so 4 KB is comfortable headroom.
    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, body,
                                               DeserializationOption::Filter(filter));
    if (err)
    {
        Serial.print("getUpdates: JSON parse error: ");
        Serial.println(err.c_str());
        // Fail-closed: tell the caller "we got something but couldn't
        // parse it". The caller will not advance update_id (the bool
        // false return short-circuits the poller's advance) and will
        // retry next tick. RFC-0003 §5 says to advance past the bad
        // update; the implementation has to choose a granularity, and
        // here we choose "envelope" — if the envelope itself is
        // unparseable we can't know what to advance to anyway.
        return false;
    }

    if (!(doc["ok"] | false))
    {
        Serial.println("getUpdates: API returned ok=false");
        return false;
    }

    JsonArray result = doc["result"].as<JsonArray>();
    if (result.isNull())
    {
        return true; // empty result is success
    }

    for (JsonObject upd : result)
    {
        TelegramUpdate u;
        // ArduinoJson v6: `| <literal>` requires matching types; use
        // explicit `as<T>()` and check truthiness via isNull() to
        // dodge the int / int64_t mismatch landmine.
        if (upd["update_id"].isNull())
        {
            // No update_id at all — drop without an entry, we can't
            // advance past it without corrupting the watermark.
            continue;
        }
        u.updateId = upd["update_id"].as<int32_t>();
        if (u.updateId == 0)
        {
            continue;
        }
        JsonObject msg = upd["message"].as<JsonObject>();
        if (msg.isNull())
        {
            // Non-message updates (channel_post, callback_query, etc.).
            // We still record the entry so the poller advances past it.
            u.valid = false;
            out.push_back(u);
            continue;
        }
        // Prefer from.id, fall back to chat.id (for unusual update
        // shapes where from is missing).
        int64_t fid = 0;
        if (!msg["from"]["id"].isNull())
        {
            fid = msg["from"]["id"].as<int64_t>();
        }
        if (fid == 0 && !msg["chat"]["id"].isNull())
        {
            fid = msg["chat"]["id"].as<int64_t>();
        }
        u.fromId = fid;
        if (!msg["reply_to_message"]["message_id"].isNull())
        {
            u.replyToMessageId = msg["reply_to_message"]["message_id"].as<int32_t>();
        }
        const char *txt = msg["text"].as<const char *>();
        if (txt)
        {
            u.text = txt;
        }
        u.valid = true;
        out.push_back(u);
    }
    return true;
}
