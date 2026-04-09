/**
 * @file      main.cpp
 * @brief     SMS -> Telegram bridge for LilyGo A76XX / SIM7xxx boards.
 *
 * Receives SMS via URC (+CMTI), reads each message with AT+CMGR, decodes
 * UCS2 when applicable, and forwards to a Telegram bot over WiFi + HTTPS.
 *
 * Based on examples/ReadSMS.
 */

#include "gpio.h"
#include "utils.h"
#include "secrets.h"

#ifdef TINY_GSM_MODEM_SIM7080
#error "This modem has no SMS function"
#endif

#define TINY_GSM_DEBUG SerialMon
#define SerialMon Serial

// See all AT commands, if wanted
// #define DUMP_AT_COMMANDS

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <TinyGsmClient.h>
#include <ArduinoJson.h>

#ifdef DUMP_AT_COMMANDS
#include <StreamDebugger.h>
StreamDebugger debugger(SerialAT, SerialMon);
TinyGsm modem(debugger);
#else
TinyGsm modem(SerialAT);
#endif

static const char *ssid = WIFI_SSID;
static const char *password = WIFI_PASSWORD;
static const char *botToken = TELEGRAM_BOT_TOKEN;
static const char *chatID = TELEGRAM_CHAT_ID;

// Let's Encrypt ISRG Root X1 — the current root behind api.telegram.org.
// Valid until 2035-06-04.
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

WiFiClientSecure telegramClient;

// After this many consecutive Telegram send failures, reboot to recover
// from stuck TLS / WiFi / DNS / TinyGSM states.
static const int MAX_CONSECUTIVE_FAILURES = 8;
static int consecutiveFailures = 0;

void connectToWiFi()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected!");
}

void syncTime()
{
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print("Syncing time...");
    time_t now = time(nullptr);
    while (now < 8 * 3600 * 2)
    {
        delay(500);
        Serial.print(".");
        now = time(nullptr);
    }
    Serial.printf("\nCurrent time: %s\n", ctime(&now));
}

bool keepTelegramClientAlive()
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
    telegramClient.setCACert(isrg_root_x1);
    telegramClient.setTimeout(15000);
    return keepTelegramClientAlive();
}

bool sendBotMessage(const String &message)
{
    String url = String("/bot") + botToken + "/sendMessage";

    size_t size = JSON_OBJECT_SIZE(2) + message.length() + 256;
    DynamicJsonDocument doc(size);
    doc["chat_id"] = chatID;
    doc["text"] = message;

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

    // Read just enough of the body to confirm "ok":true, then stop.
    bool apiOk = false;
    String body;
    unsigned long deadline = millis() + 5000;
    int toRead = contentLength > 0 ? contentLength : 512;
    while (body.length() < (size_t)toRead && millis() < deadline)
    {
        if (telegramClient.available())
        {
            body += (char)telegramClient.read();
            if (body.indexOf("\"ok\":true") != -1)
            {
                apiOk = true;
                break;
            }
            if (body.indexOf("\"ok\":false") != -1)
            {
                break;
            }
        }
        else
        {
            delay(5);
        }
    }

    return httpOk && apiOk;
}

bool postSMSMessage(const String &sender, const String &timestamp, const String &content)
{
    String formattedMessage = humanReadablePhoneNumber(sender) + " | " + timestampToRFC3339(timestamp) +
                              "\n-----\n" +
                              content;

    return sendBotMessage(formattedMessage);
}

String decodeUCS2(String hex)
{
    // Remove whitespace/newlines
    String tmp;
    for (size_t i = 0; i < hex.length(); ++i)
    {
        char c = hex[i];
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t')
            continue;
        tmp += c;
    }
    hex = tmp;
    hex.trim();

    // If it doesn't look like hex at all (e.g. module returned GSM 7bit ASCII
    // directly because CSCS != "UCS2"), pass it through unchanged.
    if (!isHexString(hex))
    {
        return hex;
    }

    auto hexVal = [](char c) -> int
    {
        c = toupper(c);
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };

    auto hexByte = [&](int idx) -> int
    {
        if (idx + 1 >= (int)hex.length())
            return -1;
        int hi = hexVal(hex[idx]);
        int lo = hexVal(hex[idx + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        return (hi << 4) | lo;
    };

    String out;
    int len = hex.length();

    // If length is multiple of 4 -> decode as UTF-16BE (UCS2/UTF-16)
    if (len >= 4 && (len % 4) == 0)
    {
        for (int i = 0; i + 3 < len; i += 4)
        {
            int b1 = hexByte(i);
            int b2 = hexByte(i + 2);
            if (b1 < 0 || b2 < 0)
                break;
            uint16_t codeUnit = ((uint16_t)b1 << 8) | (uint16_t)b2;

            // Check for surrogate pair
            if (codeUnit >= 0xD800 && codeUnit <= 0xDBFF && (i + 7) < len)
            {
                int b3 = hexByte(i + 4);
                int b4 = hexByte(i + 6);
                if (b3 >= 0 && b4 >= 0)
                {
                    uint16_t low = ((uint16_t)b3 << 8) | (uint16_t)b4;
                    if (low >= 0xDC00 && low <= 0xDFFF)
                    {
                        uint32_t high = codeUnit - 0xD800;
                        uint32_t lowpart = low - 0xDC00;
                        uint32_t codepoint = (high << 10) + lowpart + 0x10000;
                        out += (char)(0xF0 | ((codepoint >> 18) & 0x07));
                        out += (char)(0x80 | ((codepoint >> 12) & 0x3F));
                        out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        out += (char)(0x80 | (codepoint & 0x3F));
                        i += 4;
                        continue;
                    }
                }
            }

            uint16_t code = codeUnit;
            if (code <= 0x7F)
            {
                out += (char)code;
            }
            else if (code <= 0x7FF)
            {
                out += (char)(0xC0 | ((code >> 6) & 0x1F));
                out += (char)(0x80 | (code & 0x3F));
            }
            else
            {
                out += (char)(0xE0 | ((code >> 12) & 0x0F));
                out += (char)(0x80 | ((code >> 6) & 0x3F));
                out += (char)(0x80 | (code & 0x3F));
            }
        }
        return out;
    }

    // If hex length is even but not multiple of 4 -> treat as ASCII bytes in hex
    if ((len % 2) == 0)
    {
        for (int i = 0; i + 1 < len; i += 2)
        {
            int b = hexByte(i);
            if (b < 0)
                break;
            out += (char)b;
        }
        return out;
    }

    return out;
}

// Parse the body of an AT+CMGR response. Expected shape (text mode):
//
//   +CMGR: "REC UNREAD","<sender-hex>","","<timestamp>"\r\n
//   <content-hex>\r\n
//   \r\n
//   OK\r\n
//
// Returns true if a message was found; fills out sender/timestamp/content.
bool parseCmgrBody(const String &raw, String &sender, String &timestamp, String &content)
{
    int header = raw.indexOf("+CMGR:");
    if (header == -1)
        return false;
    int headerEnd = raw.indexOf("\r\n", header);
    if (headerEnd == -1)
        return false;

    String headerLine = raw.substring(header, headerEnd);

    // Quote positions: 1,2 = status; 3,4 = sender; 5,6 = (alpha, often empty); 7,8 = timestamp
    int q1 = headerLine.indexOf('"');
    int q2 = headerLine.indexOf('"', q1 + 1);
    int q3 = headerLine.indexOf('"', q2 + 1);
    int q4 = headerLine.indexOf('"', q3 + 1);
    int q5 = headerLine.indexOf('"', q4 + 1);
    int q6 = headerLine.indexOf('"', q5 + 1);
    int q7 = headerLine.indexOf('"', q6 + 1);
    int q8 = headerLine.indexOf('"', q7 + 1);
    if (q8 == -1)
        return false;

    String senderHex = headerLine.substring(q3 + 1, q4);
    String ts = headerLine.substring(q7 + 1, q8);

    int bodyStart = headerEnd + 2;
    int bodyEnd = raw.indexOf("\r\nOK", bodyStart);
    if (bodyEnd == -1)
        bodyEnd = raw.length();

    String contentHex = raw.substring(bodyStart, bodyEnd);
    contentHex.trim();

    sender = decodeUCS2(senderHex);
    timestamp = ts;
    content = decodeUCS2(contentHex);
    return true;
}

// Read message at <idx>, forward to Telegram, delete on success.
// Leaves the SMS on the SIM on any failure so a later retry can pick it up.
void handleSmsIndex(int idx)
{
    Serial.printf("-------- SMS @ index %d --------\n", idx);

    String raw;
    modem.sendAT("+CMGR=" + String(idx));
    int8_t res = modem.waitResponse(5000UL, raw);
    if (res != 1)
    {
        Serial.println("CMGR failed");
        return;
    }

    String sender, timestamp, content;
    if (!parseCmgrBody(raw, sender, timestamp, content))
    {
        Serial.println("Unable to parse CMGR body. Raw:");
        Serial.println(raw);
        // Nothing useful here; delete so we don't loop on a malformed slot.
        modem.sendAT("+CMGD=" + String(idx));
        modem.waitResponse(1000UL);
        return;
    }

    Serial.print("Sender:    ");
    Serial.println(sender);
    Serial.print("Timestamp: ");
    Serial.println(timestamp);
    Serial.print("Content:   ");
    Serial.println(content);

    if (postSMSMessage(sender, timestamp, content))
    {
        consecutiveFailures = 0;
        Serial.println("Posted to Telegram OK, deleting SMS.");
        modem.sendAT("+CMGD=" + String(idx));
        modem.waitResponse(1000UL);
    }
    else
    {
        consecutiveFailures++;
        Serial.printf("Post to Telegram FAILED (%d consecutive). Keeping SMS on SIM.\n",
                      consecutiveFailures);
        if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES)
        {
            Serial.println("Too many consecutive failures, rebooting to recover...");
            delay(1000);
            ESP.restart();
        }
    }
}

// Sweep all SMS currently on the SIM and forward each one. Used at startup
// to drain anything that arrived while the bridge was offline.
void sweepExistingSms()
{
    String data;
    modem.sendAT("+CMGL=\"ALL\"");
    int8_t res = modem.waitResponse(10000UL, data);
    if (res != 1)
    {
        Serial.println("Initial CMGL sweep failed");
        return;
    }

    int search = 0;
    while (true)
    {
        int start = data.indexOf("+CMGL:", search);
        if (start == -1)
            break;
        int colon = start + 6;
        int comma = data.indexOf(',', colon);
        if (comma == -1)
            break;
        int idx = data.substring(colon, comma).toInt();
        search = comma;
        if (idx > 0)
        {
            handleSmsIndex(idx);
        }
    }
}

// #define NETWORK_APN     "CHN-CT"

void setup()
{
    Serial.begin(115200);
#ifdef BOARD_POWERON_PIN
    pinMode(BOARD_POWERON_PIN, OUTPUT);
    digitalWrite(BOARD_POWERON_PIN, HIGH);
#endif

#ifdef MODEM_RESET_PIN
    pinMode(MODEM_RESET_PIN, OUTPUT);
    digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
    delay(100);
    digitalWrite(MODEM_RESET_PIN, MODEM_RESET_LEVEL);
    delay(2600);
    digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
#endif

#ifdef MODEM_FLIGHT_PIN
    pinMode(MODEM_FLIGHT_PIN, OUTPUT);
    digitalWrite(MODEM_FLIGHT_PIN, HIGH);
#endif

    pinMode(MODEM_DTR_PIN, OUTPUT);
    digitalWrite(MODEM_DTR_PIN, LOW);

    pinMode(BOARD_PWRKEY_PIN, OUTPUT);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);
    delay(100);
    digitalWrite(BOARD_PWRKEY_PIN, HIGH);
    delay(MODEM_POWERON_PULSE_WIDTH_MS);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);

#ifdef MODEM_RING_PIN
    pinMode(MODEM_RING_PIN, INPUT_PULLUP);
#endif

    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

    Serial.println("Start modem...");
    delay(3000);

    while (!modem.testAT())
    {
        delay(10);
    }

    String modemName = "UNKNOWN";
    while (1)
    {
        modemName = modem.getModemName();
        if (modemName == "UNKNOWN")
        {
            Serial.println("Unable to obtain module information normally, try again");
            delay(1000);
        }
        else if (modemName.startsWith("SIM7670"))
        {
            while (1)
            {
                Serial.println("SIM7670 does not support SMS Function");
                delay(1000);
            }
        }
        else
        {
            Serial.print("Model Name:");
            Serial.println(modemName);
            break;
        }
        delay(5000);
    }

    Serial.println("Wait SMS Done.");
    if (!modem.waitResponse(100000UL, "SMS DONE"))
    {
        Serial.println("Can't wait from sms register ....");
        return;
    }

#ifdef NETWORK_APN
    Serial.printf("Set network apn : %s\n", NETWORK_APN);
    if (!modem.setNetworkAPN(NETWORK_APN))
    {
        Serial.println("Set network apn error !");
    }
#endif

    int16_t sq;
    Serial.print("Wait for the modem to register with the network.");
    RegStatus status = REG_NO_RESULT;
    while (status == REG_NO_RESULT || status == REG_SEARCHING || status == REG_UNREGISTERED)
    {
        status = modem.getRegistrationStatus();
        switch (status)
        {
        case REG_UNREGISTERED:
        case REG_SEARCHING:
            sq = modem.getSignalQuality();
            Serial.printf("[%lu] Signal Quality:%d\n", millis() / 1000, sq);
            delay(1000);
            break;
        case REG_DENIED:
            Serial.println("Network registration was rejected, please check if the APN is correct");
            return;
        case REG_OK_HOME:
            Serial.println("Online registration successful");
            break;
        case REG_OK_ROAMING:
            Serial.println("Network registration successful, currently in roaming mode");
            break;
        default:
            Serial.printf("Registration Status:%d\n", status);
            delay(1000);
            break;
        }
    }
    Serial.println();

    Serial.printf("Registration Status:%d\n", status);
    delay(1000);

    // SMS in text mode, UCS2 character set so both sender and content come
    // back as hex that our decoder can handle regardless of language.
    modem.sendAT("+CMGF=1");
    modem.waitResponse(10000);
    modem.sendAT("+CSCS=\"UCS2\"");
    modem.waitResponse(2000);

    // Show text parameters in CMGR/CMGL headers.
    modem.sendAT("+CSDH=1");
    modem.waitResponse(2000);

    // Route new-message indications to TE as +CMTI URCs (store in SIM, notify us).
    modem.sendAT("+CNMI=2,1,0,0,0");
    modem.waitResponse(2000);

    connectToWiFi();
    syncTime();
    if (!setupTelegramClient())
    {
        Serial.println("Telegram client setup failed, aborting SMS bridge.");
        return;
    }

    sendBotMessage("🚀 Modem SMS to Telegram Bridge is now online!");

    // Drain anything that arrived while we were offline.
    sweepExistingSms();
}

void loop()
{
    // Drive TinyGSM's URC handling / keep its buffer drained.
    modem.maintain();

    // Consume unsolicited lines and react to +CMTI: "SM",<idx>
    while (SerialAT.available())
    {
        String line = SerialAT.readStringUntil('\n');
        line.trim();
        if (line.length() == 0)
            continue;

        if (line.startsWith("+CMTI:"))
        {
            int comma = line.indexOf(',');
            if (comma != -1)
            {
                int idx = line.substring(comma + 1).toInt();
                if (idx > 0)
                {
                    handleSmsIndex(idx);
                }
            }
        }
        // Other URCs are ignored for now.
    }

    // Periodically verify WiFi is still up; ESP will auto-reconnect on its own
    // but if it can't, we'd rather reboot than loop forever.
    static unsigned long lastWifiCheck = 0;
    if (millis() - lastWifiCheck > 30000)
    {
        lastWifiCheck = millis();
        if (WiFi.status() != WL_CONNECTED)
        {
            Serial.println("WiFi dropped, attempting reconnect...");
            WiFi.reconnect();
        }
    }

    delay(50);
}

#ifndef TINY_GSM_FORK_LIBRARY
#error "No correct definition detected, Please copy all the [lib directories](https://github.com/Xinyuan-LilyGO/LilyGO-T-A76XX/tree/main/lib) to the arduino libraries directory , See README"
#endif
