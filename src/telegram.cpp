#include "telegram.h"
#include "secrets.h"

#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#ifdef ESP_PLATFORM
#include <esp_task_wdt.h>
#endif

// utilities.h sets TINY_GSM_MODEM_xxx based on the LILYGO_T_* build flag —
// it must be included before TinyGsmClient.h to satisfy its modem-model guard.
// telegram.cpp is excluded from the native test env via platformio.ini
// build_src_filter, so TinyGsmClient.h is safe to include here.
#include "utilities.h"
#include <TinyGsmClient.h>
#ifdef TINY_GSM_MODEM_A76XXSSL
// Forward-declared in main.cpp; telegram.cpp borrows a reference via
// extern so we don't duplicate the modem instance.
extern TinyGsm modem;
#endif

static const char *botToken = TELEGRAM_BOT_TOKEN;
// chatID was removed in RFC-0014. The outbound destination is now held in
// RealBotClient::adminChatId_ and set via setAdminChatId() in setup().

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

// WiFi TLS transport (primary).
static WiFiClientSecure telegramWifiClient;

// Note: the modem TLS client (TinyGsmClientSecure / GsmClientSecureA76xxSSL)
// is a function-local static inside setupCellularClient() rather than a
// file-scope static here. Reason: its constructor takes `modem` (defined in
// main.cpp) as an argument, and C++ provides no cross-TU initialization order
// guarantee for file-scope statics — constructing it here would be UB if
// `modem` hasn't been constructed yet. A function-local static is guaranteed
// to initialize on first call, which happens inside setup() after `modem` is
// live. See rfc/0004-cellular-fallback.md §Code Review.

// keepTransportAlive() reconnects the active transport if the connection was
// dropped. Works for both WiFiClientSecure and GsmClientSecureA76xxSSL
// because both implement the Arduino Client interface.
static bool keepTransportAlive(Client *transport)
{
    if (!transport)
    {
        Serial.println("keepTransportAlive: no transport set");
        return false;
    }
    if (transport->connected())
    {
        return true;
    }

    Serial.println("Reconnecting to Telegram API server...");
    transport->stop();
    bool ok = transport->connect("api.telegram.org", 443);
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

bool setupTelegramClient(RealBotClient &bot)
{
#ifdef ALLOW_INSECURE_TLS
    // Opt-in escape hatch for deployment networks that MITM HTTPS or serve a
    // chain no reasonable public bundle will accept. Must never be the default
    // build. The warning line below is the canonical signal in a serial
    // capture that the firmware is running without TLS verification — do not
    // remove it. See rfc/0001-tls-cert-pinning.md.
    Serial.println("[SECURITY WARNING] TLS verification disabled via -DALLOW_INSECURE_TLS");
    telegramWifiClient.setInsecure();
#else
    telegramWifiClient.setCACertBundle(rootca_crt_bundle_start);
#endif
    telegramWifiClient.setTimeout(15000);
    bot.setTransport(telegramWifiClient);
    return keepTransportAlive(&telegramWifiClient);
}

bool setupCellularClient(RealBotClient &bot)
{
#ifdef TINY_GSM_MODEM_A76XXSSL
    // Function-local static: guaranteed to initialize on first call, after
    // `modem` (main.cpp file-scope static) is already live. Avoids the
    // cross-TU static-initialization-order UB that a file-scope static would
    // cause. See the comment above this function for details.
    static TinyGsmClientSecure telegramModemClient(modem, 0);

    // CELLULAR TLS WARNING: Certificate verification is NOT active on the
    // modem path. The A76xx modem's TLS stack (AT+CSSLCFG) requires certs
    // to be pre-uploaded to modem flash via AT+CCERTDOWN — there is no
    // equivalent of WiFiClientSecure::setCACertBundle(). Without a cert,
    // authmode stays 0 (no server authentication). This means the cellular
    // path is susceptible to MITM attacks. The warning below is the
    // canonical serial-log signal that the firmware is using an unverified
    // cellular TLS connection. See rfc/0004-cellular-fallback.md.
    Serial.println("[CELLULAR TLS] Certificate verification not available on modem path — connection is unverified");
    telegramModemClient.setTimeout(15000);
    bot.setTransport(telegramModemClient);
    return keepTransportAlive(&telegramModemClient);
#else
    Serial.println("setupCellularClient: modem SSL not compiled (no TINY_GSM_MODEM_A76XXSSL)");
    return false;
#endif
}

bool registerBotCommands(RealBotClient &bot)
{
    // Use the transport that was already set up by setup[Telegram|Cellular]Client.
    Client *active = bot.getTransport();
    if (!active)
    {
        Serial.println("registerBotCommands: no transport set, skipping");
        return false;
    }
    if (!keepTransportAlive(active))
    {
        Serial.println("registerBotCommands: connection failed");
        return false;
    }

    String url = String("/bot") + botToken + "/setMyCommands";

    // Register all commands so Telegram's autocomplete menu shows them.
    DynamicJsonDocument doc(1024);
    JsonArray cmds = doc.createNestedArray("commands");
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "help";
        c["description"] = "Show available commands";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "time";
        c["description"] = "Show current UTC time";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "ntp";
        c["description"] = "Force NTP time resync";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "ping";
        c["description"] = "Check if the bridge is alive";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "echo";
        c["description"] = "Reflect text back (connectivity test)";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "last";
        c["description"] = "Show last N forwarded SMS (default 5)";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "concat";
        c["description"] = "Show in-flight concat reassembly groups";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "debug";
        c["description"] = "Show SMS diagnostic log";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "status";
        c["description"] = "Show device health and stats";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "blocklist";
        c["description"] = "Show SMS sender block list";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "block";
        c["description"] = "Block an SMS sender";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "unblock";
        c["description"] = "Unblock an SMS sender";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "wifi";
        c["description"] = "Force WiFi reconnect";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "mute";
        c["description"] = "Snooze proactive alerts: /mute [minutes]";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "unmute";
        c["description"] = "Cancel alert snooze";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "heap";
        c["description"] = "Show free heap stats";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "csq";
        c["description"] = "Quick signal strength snapshot";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "version";
        c["description"] = "Show firmware build timestamp";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "reboot";
        c["description"] = "Soft reboot the bridge";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "balance";
        c["description"] = "Check SIM balance via USSD";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "send";
        c["description"] = "Send an SMS: /send <number> <message>";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "sendall";
        c["description"] = "Broadcast SMS to all aliases: /sendall <message>";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "test";
        c["description"] = "Send a test SMS to verify outbound path";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "queue";
        c["description"] = "Show pending outbound SMS queue";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "flushqueue";
        c["description"] = "Immediately retry all pending outbound SMS";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "clearqueue";
        c["description"] = "Discard all pending outbound SMS";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "cleardebug";
        c["description"] = "Clear the SMS debug log";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "cancel";
        c["description"] = "Cancel a queued outbound SMS: /cancel <N>";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "aliases";
        c["description"] = "List phone number aliases";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "addalias";
        c["description"] = "Add/replace alias: /addalias <name> <number>";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "rmalias";
        c["description"] = "Remove an alias: /rmalias <name>";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "sim";
        c["description"] = "SIM identity: ICCID, IMEI, operator";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "ussd";
        c["description"] = "Send USSD code and get response: /ussd *100#";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "uptime";
        c["description"] = "Show device uptime";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "network";
        c["description"] = "Show cellular operator + registration + CSQ";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "logs";
        c["description"] = "Show last N SMS log entries: /logs [N]";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "boot";
        c["description"] = "Show boot count, reset reason, and boot timestamp";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "count";
        c["description"] = "Session SMS/call counter summary";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "me";
        c["description"] = "Show your Telegram fromId and chatId";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "ip";
        c["description"] = "Show WiFi IP address, SSID, and RSSI";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "smsslots";
        c["description"] = "Show SIM SMS slot usage";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "lifetime";
        c["description"] = "Show lifetime SMS count and boot count";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "announce";
        c["description"] = "Broadcast to all authorized users: /announce <msg>";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "digest";
        c["description"] = "Show on-demand stats digest";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "note";
        c["description"] = "Show device note";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "setnote";
        c["description"] = "Save device note: /setnote <text> (max 120 chars)";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "exportaliases";
        c["description"] = "Export all phone aliases as name=number lines";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "shortcuts";
        c["description"] = "Quick reference of common commands";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "clearaliases";
        c["description"] = "Remove all phone aliases";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "cancelnum";
        c["description"] = "Cancel all queued SMS to a phone number";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "setinterval";
        c["description"] = "Set heartbeat interval: /setinterval <seconds> (0=off)";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "setmaxfail";
        c["description"] = "Set failure reboot threshold: /setmaxfail <N> (0=never)";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "flushsim";
        c["description"] = "Delete all SMS from SIM: /flushsim yes";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "simlist";
        c["description"] = "List all SMS stored in SIM";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "simread";
        c["description"] = "Read a SIM slot: /simread <idx>";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "modeminfo";
        c["description"] = "Show IMEI, model, and firmware revision";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "setconcatttl";
        c["description"] = "Set concat fragment TTL: /setconcatttl <seconds>";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "setdedup";
        c["description"] = "Set SMS dedup window: /setdedup <seconds> (0=off)";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "cleardedup";
        c["description"] = "Clear SMS dedup ring buffer";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "forwardsim";
        c["description"] = "Force-forward a SIM slot: /forwardsim <idx>";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "setpollinterval";
        c["description"] = "Set Telegram poll interval: /setpollinterval <seconds>";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "sweepsim";
        c["description"] = "Manually sweep SIM for pending SMS";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "health";
        c["description"] = "Quick health check (WiFi/CSQ/heap/uptime)";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "setautoreply";
        c["description"] = "Set SMS auto-reply: /setautoreply <text>";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "getautoreply";
        c["description"] = "Show current SMS auto-reply text";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "clearautoreply";
        c["description"] = "Disable SMS auto-reply";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "resetwatermark";
        c["description"] = "Reset Telegram update_id to re-process recent updates";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "setforward";
        c["description"] = "Toggle SMS forwarding: /setforward on|off";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "logstats";
        c["description"] = "Aggregate outcome statistics from SMS debug log";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "logsoutcome";
        c["description"] = "Filter SMS log by outcome keyword: /logsoutcome <fail|fwd|dup|...>";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "simstatus";
        c["description"] = "Live SIM + network status (registration, CSQ, operator, ICCID)";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "topn";
        c["description"] = "Top N SMS senders by frequency: /topn [N] (default 5)";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "wifiscan";
        c["description"] = "Scan nearby WiFi networks (SSID, channel, RSSI)";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "logsince";
        c["description"] = "Show SMS log from past N hours: /logsince <1-168>";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "setmaxparts";
        c["description"] = "Set max outbound SMS concat parts (1-10): /setmaxparts <N>";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "smscount";
        c["description"] = "SIM SMS storage capacity (used/total) via AT+CPMS?";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "setblockmode";
        c["description"] = "Toggle block list enforcement: /setblockmode on|off";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "blockcheck";
        c["description"] = "Check if a number would be blocked: /blockcheck <phone>";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "setcallnotify";
        c["description"] = "Enable/mute call notifications: /setcallnotify on|off";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "setcalldedup";
        c["description"] = "Set call dedup window in seconds (1-60): /setcalldedup <N>";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "setunknowndeadline";
        c["description"] = "Set RING-without-CLIP deadline in ms (500-10000): /setunknowndeadline <ms>";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "settings";
        c["description"] = "Show all runtime-configurable parameters";
    }
    {
        JsonObject c = cmds.createNestedObject();
        c["command"] = "nvsinfo";
        c["description"] = "NVS flash storage usage (used/free/total entries)";
    }
    String payload;
    serializeJson(doc, payload);

    active->print(String("POST ") + url + " HTTP/1.1\r\n");
    active->print("Host: api.telegram.org\r\n");
    active->print("Connection: keep-alive\r\n");
    active->print("Content-Type: application/json\r\n");
    active->print("Content-Length: ");
    active->print(payload.length());
    active->print("\r\n\r\n");
    active->print(payload);

    String statusLine = active->readStringUntil('\n');
    statusLine.trim();
    Serial.print("setMyCommands status: ");
    Serial.println(statusLine);
    bool httpOk = statusLine.indexOf(" 200") != -1;

    int contentLength = -1;
    while (active->connected() || active->available())
    {
        String line = active->readStringUntil('\n');
        if (line == "\r" || line.length() == 0)
            break;
        line.toLowerCase();
        if (line.startsWith("content-length:"))
        {
            contentLength = line.substring(15).toInt();
        }
    }

    // Drain the full response body (keep-alive safety).
    String body;
    unsigned long deadline = millis() + 8000;
    size_t target = contentLength > 0 ? (size_t)contentLength : 8192;
    while (body.length() < target && millis() < deadline)
    {
        if (active->available())
        {
            body += (char)active->read();
        }
        else if (contentLength <= 0 && !active->connected())
        {
            break;
        }
        else
        {
            delay(2);
        }
    }

    bool ok = httpOk && body.indexOf("\"ok\":true") != -1;
    if (ok)
    {
        Serial.println("Bot commands registered: /help /echo /time /ntp /ping /last /concat /debug /cleardebug /status /blocklist /block /unblock /wifi /mute /unmute /heap /csq /sim /ussd /balance /at /version /label /setlabel /reboot /send /sendall /test /queue /flushqueue /clearqueue /resetstats /cancel /aliases /addalias /rmalias");
    }
    else
    {
        Serial.println("registerBotCommands: failed");
    }
    return ok;
}

int32_t RealBotClient::doSendMessage(const String &text, int64_t chatId)
{
    if (chatId == 0)
    {
        Serial.println("doSendMessage: chatId is 0, skipping send");
        return -1;
    }
    if (!transport_)
    {
        Serial.println("doSendMessage: no transport set");
        return 0;
    }

#ifdef ESP_PLATFORM
    esp_task_wdt_reset();  // RFC-0028: doSendMessage can block up to 8 s on TLS drain
#endif

    String url = String("/bot") + botToken + "/sendMessage";

    size_t size = JSON_OBJECT_SIZE(2) + text.length() + 256;
    DynamicJsonDocument doc(size);
    doc["chat_id"] = chatId;   // int64_t; ArduinoJson v6 serialises as JSON number
    doc["text"] = text;

    String payload;
    serializeJson(doc, payload);

    if (!keepTransportAlive(transport_))
    {
        return 0;
    }

    transport_->print(String("POST ") + url + " HTTP/1.1\r\n");
    transport_->print("Host: api.telegram.org\r\n");
    transport_->print("Connection: keep-alive\r\n");
    transport_->print("Content-Type: application/json\r\n");
    transport_->print("Content-Length: ");
    transport_->print(payload.length());
    transport_->print("\r\n\r\n");
    transport_->print(payload);

    // Parse status line: "HTTP/1.1 200 OK"
    String statusLine = transport_->readStringUntil('\n');
    statusLine.trim();
    Serial.print("Telegram status: ");
    Serial.println(statusLine);

    bool httpOk = statusLine.indexOf(" 200") != -1;

    // Drain headers until blank line
    int contentLength = -1;
    while (transport_->connected() || transport_->available())
    {
        String line = transport_->readStringUntil('\n');
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
        if (transport_->available())
        {
            body += (char)transport_->read();
        }
        else if (contentLength <= 0 && !transport_->connected())
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
    return doSendMessage(text, adminChatId_) > 0;
}

int32_t RealBotClient::sendMessageReturningId(const String &text)
{
    return doSendMessage(text, adminChatId_);
}

bool RealBotClient::sendMessageTo(int64_t chatId, const String &text)
{
    return doSendMessage(text, chatId) > 0;
}

int32_t RealBotClient::sendMessageToReturningId(int64_t chatId, const String &text)
{
    return doSendMessage(text, chatId);
}

// ---------- pollUpdates (RFC-0003) ----------

bool RealBotClient::pollUpdates(int32_t sinceUpdateId, int32_t timeoutSec,
                                std::vector<TelegramUpdate> &out)
{
    out.clear();

    if (!transport_)
    {
        Serial.println("pollUpdates: no transport set");
        return false;
    }

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

    if (!keepTransportAlive(transport_))
    {
        return false;
    }

    transport_->print(String("GET ") + url + " HTTP/1.1\r\n");
    transport_->print("Host: api.telegram.org\r\n");
    transport_->print("Connection: keep-alive\r\n");
    transport_->print("\r\n");

    // Allow at least timeoutSec + a generous slack to read the
    // response. Telegram parks the request on its side until either
    // an update arrives or the timeout fires.
    unsigned long readDeadline = millis() + (unsigned long)(timeoutSec * 1000) + 8000;

    String statusLine = transport_->readStringUntil('\n');
    statusLine.trim();
    Serial.print("Telegram getUpdates status: ");
    Serial.println(statusLine);
    bool httpOk = statusLine.indexOf(" 200") != -1;

    int contentLength = -1;
    while (transport_->connected() || transport_->available())
    {
        if (millis() > readDeadline)
        {
            Serial.println("getUpdates: header read timeout");
            return false;
        }
        String line = transport_->readStringUntil('\n');
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
        if (transport_->available())
        {
            body += (char)transport_->read();
        }
        else if (contentLength <= 0 && !transport_->connected())
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
        // chatId: always the message's chat.id (DM or group). Used as the
        // reply target by processUpdate so responses go back to the originating
        // context (DM or group), not to the admin chat.
        if (!msg["chat"]["id"].isNull())
        {
            u.chatId = msg["chat"]["id"].as<int64_t>();
        }

        // Prefer from.id, fall back to chat.id (for unusual update
        // shapes where from is missing). fromId is used for the auth gate.
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
