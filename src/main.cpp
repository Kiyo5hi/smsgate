/**
 * @file      main.cpp
 * @brief     SMS -> Telegram bridge for LilyGo A76XX / SIM7xxx boards.
 *
 * Composition root. Builds the real modem adapter, real Telegram bot
 * client, and a reboot callback, then wires them into an SmsHandler.
 * The SMS logic itself lives in src/sms_handler.{h,cpp} and the pure
 * decoding helpers in src/sms_codec.{h,cpp} — both reachable from the
 * native test env via platformio.ini's [env:native].
 *
 * Based on examples/ReadSMS.
 */

#include "utilities.h"
#include "secrets.h"
#include "allow_list.h"
#include "sms_block_list.h"
#include "telegram.h"
#include "sms_handler.h"
#include "call_handler.h"
#include "real_modem.h"
#include "real_persist.h"
#include "reply_target_map.h"
#include "sms_sender.h"
#include "sms_debug_log.h"
#include "telegram_poller.h"
#ifdef ENABLE_DELIVERY_REPORTS
#include "delivery_report_map.h"
#include "delivery_report_handler.h"
#endif

#include <esp_system.h>
#include <esp_task_wdt.h>

#ifdef TINY_GSM_MODEM_SIM7080
#error "This modem has no SMS function"
#endif

// RFC-0014: Multi-user allow list.
// Backward-compat shim: if only TELEGRAM_CHAT_ID is defined (pre-RFC-0014
// secrets.h), treat it as a single-entry TELEGRAM_CHAT_IDS automatically.
// If BOTH are defined simultaneously the build is ambiguous — error out so
// the developer knows to clean up their secrets.h.
#if defined(TELEGRAM_CHAT_ID) && defined(TELEGRAM_CHAT_IDS)
#error "Define either TELEGRAM_CHAT_ID (single user) or TELEGRAM_CHAT_IDS (multi-user CSV), not both."
#endif
#ifndef TELEGRAM_CHAT_IDS
#  ifdef TELEGRAM_CHAT_ID
#    define TELEGRAM_CHAT_IDS TELEGRAM_CHAT_ID
#  else
#    error "Define TELEGRAM_CHAT_IDS (or legacy TELEGRAM_CHAT_ID) in secrets.h — see secrets.h.example"
#  endif
#endif

// RFC-0017: Scheduled heartbeat. Default 6 hours; 0 = disabled.
// Override in secrets.h or via -DHEARTBEAT_INTERVAL_SEC=... in platformio.ini.
#ifndef HEARTBEAT_INTERVAL_SEC
#  define HEARTBEAT_INTERVAL_SEC 21600  // 6 hours
#endif
#if HEARTBEAT_INTERVAL_SEC != 0 && HEARTBEAT_INTERVAL_SEC < 300
#  error "HEARTBEAT_INTERVAL_SEC must be 0 (disabled) or >= 300 (5 minutes)"
#endif

// Timezone offset from UTC in seconds for the /status time display.
// Set in secrets.h, e.g.: #define TIMEZONE_OFFSET_SEC 28800  // UTC+8
#ifndef TIMEZONE_OFFSET_SEC
#  define TIMEZONE_OFFSET_SEC 0
#endif

#define SerialMon Serial
#define TINY_GSM_DEBUG SerialMon

// See all AT commands, if wanted
// #define DUMP_AT_COMMANDS

// Uncomment / set via -D to provide a network APN if your operator requires
// one (e.g. "CHN-CT" for China Telecom). Without it, network registration
// may be silently denied on some Chinese / regional SIMs.
// #define NETWORK_APN     "CHN-CT"

#include <WiFi.h>
#include <TinyGsmClient.h>

#ifndef TINY_GSM_FORK_LIBRARY
#error "No correct definition detected, Please copy all the [lib directories](https://github.com/Xinyuan-LilyGO/LilyGO-T-A76XX/tree/main/lib) to the arduino libraries directory , See README"
#endif

#ifdef DUMP_AT_COMMANDS
#include <StreamDebugger.h>
StreamDebugger debugger(SerialAT, SerialMon);
TinyGsm modem(debugger);
#else
TinyGsm modem(SerialAT);
#endif

static const char *ssid = WIFI_SSID;
static const char *password = WIFI_PASSWORD;

// Track which transport is currently active so the loop() WiFi-drop
// handler knows whether to attempt a cellular takeover.
static enum class ActiveTransport { kNone, kWiFi, kCellular } activeTransport = ActiveTransport::kNone;

// Composition root state. These objects are singletons for the
// lifetime of the process; the handlers borrow references to them.
static RealModem realModem(modem, SerialAT);
static RealBotClient realBot;
static SmsDebugLog smsDebugLog;
static RealPersist realPersist;
static ReplyTargetMap replyTargets(realPersist);
static SmsSender smsSender(realModem);
#ifdef ENABLE_DELIVERY_REPORTS
static DeliveryReportMap deliveryReportMap;
static DeliveryReportHandler deliveryReportHandler(
    realBot, deliveryReportMap,
    []() -> uint32_t { return (uint32_t)millis(); });
#endif
static SmsHandler smsHandler(
    realModem, realBot,
    []() {
        // Production reboot callback. Short delay gives the last Serial
        // line a chance to flush before the chip resets.
        delay(1000);
        ESP.restart();
    },
    []() -> unsigned long { return millis(); });
static CallHandler callHandler(realModem, realBot, []() -> uint32_t {
    return (uint32_t)millis();
});

// RFC-0014: Process-lifetime allow list, parsed in setup() from
// TELEGRAM_CHAT_IDS. allowedIds[0] is the admin (forward target).
// All entries may send replies and use bot commands.
// parseAllowedIds() is defined in src/allow_list.h (inline, also
// reachable by the native test env).
static int64_t allowedIds[10] = {};
static int     allowedIdCount = 0;

// RFC-0018: SMS sender block list. Declared as file-scope statics so
// the pointer passed to smsHandler.setBlockList() remains valid for the
// process lifetime (a setup()-local variable would dangle after return).
#ifdef SMS_BLOCK_LIST
static char sBlockList[kSmsBlockListMaxEntries][kSmsBlockListMaxNumberLen + 1];
static int  sBlockListCount = 0;
#else
// Declare empty arrays even when SMS_BLOCK_LIST is not defined so the
// SmsBlockMutatorFn lambda can reference them unconditionally (the lambda
// will report count == 0 and nothing will be blocked).
static char sBlockList[kSmsBlockListMaxEntries][kSmsBlockListMaxNumberLen + 1] = {};
static int  sBlockListCount = 0;
#endif

// RFC-0021: Runtime SMS block list. Loaded from NVS at startup and mutated
// at runtime via /block and /unblock. File-scope so the pointer passed to
// smsHandler.setRuntimeBlockList() remains valid for the process lifetime.
static char sRuntimeBlockList[kSmsBlockListMaxEntries][kSmsBlockListMaxNumberLen + 1] = {};
static int  sRuntimeBlockListCount = 0;

// RFC-0023: Deferred soft restart flag. Set by the /restart command lambda
// inside ListMutatorFn; loop() checks and fires ESP.restart() on the next
// iteration AFTER the Telegram "Restarting..." message has been sent.
static bool s_pendingRestart = false;

// The poller is a process-lifetime singleton; we heap-allocate it once
// in setup() and never free it. A raw pointer keeps the call site clean.
static TelegramPoller *telegramPoller = nullptr;

// Cached modem signal/registration values, refreshed every 30 s in
// loop() from a safe point OUTSIDE the AT-response read window.
// Default values (0 / REG_NO_RESULT) shown if loop hasn't run yet.
static int cachedCsq = 0;
static RegStatus cachedRegStatus = REG_NO_RESULT;
// RFC-0027: Operator name (AT+COPS?), refreshed every 30s alongside CSQ.
static String cachedOperatorName;
// RFC-0031: CSQ trend — last 6 readings (one per 30s refresh = 3 min window).
static int csqHistory[6] = {0, 0, 0, 0, 0, 0};
static int csqHistoryIdx = 0;
static bool csqHistoryFull = false;

// RFC-0017: StatusFn promoted to file scope so loop() can call it for
// the scheduled heartbeat. Assigned in setup() before TelegramPoller is
// constructed. Default-constructed to nullptr; guarded with `if (statusFn)`
// before use.
static std::function<String()> statusFn;

// RFC-0017: Heartbeat timer. Initialised to 0 so the first heartbeat fires
// one full interval after boot (the boot banner already covers "just started").
#if HEARTBEAT_INTERVAL_SEC != 0
static uint32_t lastHeartbeatMs = 0;
#endif

// Try to connect to WiFi. Returns true if connected within the timeout.
// Times out after ~15 s (30 retries × 500 ms) so we don't block setup()
// forever when WiFi is unavailable and cellular fallback is configured.
bool connectToWiFi()
{
    if (strlen(ssid) == 0)
    {
        Serial.println("WiFi: SSID not configured, skipping.");
        return false;
    }
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++)
    {
        delay(500);
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("\nWiFi connected!");
        return true;
    }
    Serial.println("\nWiFi connection failed (timeout).");
    return false;
}

void syncTime()
{
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print("Syncing time...");
    time_t now = time(nullptr);
    while (now < 8 * 3600 * 2)
    {
        esp_task_wdt_reset();  // RFC-0015: syncTime() is also called from loop()
        delay(500);
        Serial.print(".");
        now = time(nullptr);
    }
    Serial.printf("\nCurrent time: %s\n", ctime(&now));
}

// RFC-0020: Map esp_reset_reason_t to a human-readable string for /status.
// Lives in main.cpp (not sms_debug_log.cpp) to keep hardware headers out of
// the files compiled in the native test environment.
static const char *resetReasonStr(esp_reset_reason_t r)
{
    switch (r)
    {
    case ESP_RST_POWERON:   return "Power-on";
    case ESP_RST_SW:        return "Software reset";
    case ESP_RST_PANIC:     return "Panic/exception";
    case ESP_RST_WDT:       return "Watchdog (TWDT)";
    case ESP_RST_BROWNOUT:  return "Brownout";
    case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
    default:                return "Unknown";
    }
}

void setup()
{
    // RFC-0020: Capture reset reason once at startup (before any code path
    // can trigger another reset). Used in the /status lambda below.
    static esp_reset_reason_t s_resetReason = esp_reset_reason();

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

#ifdef MODEM_RING_PIN
    pinMode(MODEM_RING_PIN, INPUT_PULLUP);
#endif

    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

    // The modem and ESP32 are on independent power rails. After an ESP-only
    // reset (upload, watchdog, ESP.restart) the modem may still be powered on
    // from the previous session. Pulsing PWRKEY in that case would TURN IT
    // OFF, leaving testAT() to spin forever. Probe first; only pulse PWRKEY
    // if the modem is genuinely silent.
    Serial.println("Probing modem...");
    bool modemAlreadyOn = false;
    for (int i = 0; i < 5; i++)
    {
        if (modem.testAT(500))
        {
            modemAlreadyOn = true;
            break;
        }
    }

    if (modemAlreadyOn)
    {
        Serial.println("Modem already powered on, skipping PWRKEY pulse.");
    }
    else
    {
        Serial.println("Modem silent, pulsing PWRKEY...");
        digitalWrite(BOARD_PWRKEY_PIN, LOW);
        delay(100);
        digitalWrite(BOARD_PWRKEY_PIN, HIGH);
        delay(MODEM_POWERON_PULSE_WIDTH_MS);
        digitalWrite(BOARD_PWRKEY_PIN, LOW);

        Serial.println("Start modem...");
        delay(3000);
        while (!modem.testAT())
        {
            delay(10);
        }
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

    // SMS in PDU mode (RFC-0002). Raw PDU bytes come back from AT+CMGR /
    // AT+CMGL as a hex blob that parseSmsPdu() decodes. CSCS is
    // irrelevant in PDU mode, so we don't configure it on the receive
    // path — TinyGSM's sendSMS / sendSMS_UTF16 flip the module back
    // into text mode with their own CSCS before each send, and this
    // setup handles re-entry to PDU mode next time a URC fires.
    modem.sendAT("+CMGF=0");
    modem.waitResponse(10000);

    // Show text parameters in CMGR/CMGL headers.
    modem.sendAT("+CSDH=1");
    modem.waitResponse(2000);

    // Route new-message indications to TE as +CMTI URCs (store in SIM, notify us).
    // When -DENABLE_DELIVERY_REPORTS is set, also route +CDS status report URCs
    // to the TE by setting ds=1 (4th parameter). RFC-0011.
#ifdef ENABLE_DELIVERY_REPORTS
    modem.sendAT("+CNMI=2,1,0,1,0");
    modem.waitResponse(2000);
    // Enable Phase 2+ mode so the modem processes incoming status reports.
    modem.sendAT("+CSMS=1");
    modem.waitResponse(2000);
    smsSender.setDeliveryReportMap(&deliveryReportMap);
    Serial.println("Delivery reports enabled (+CDS routing active, RFC-0011).");
#else
    modem.sendAT("+CNMI=2,1,0,0,0");
    modem.waitResponse(2000);
#endif

    // Enable Caller Line Identification Presentation so incoming RINGs
    // are followed by a +CLIP: "<number>",<type>,... URC carrying the
    // caller's number. See RFC-0005 / call_handler.{h,cpp}.
    modem.sendAT("+CLIP=1");
    modem.waitResponse(2000);

    // --- Network / Telegram transport setup (RFC-0004) ---
    //
    // Strategy: WiFi primary, cellular fallback.
    //   1. Try WiFi. If connected, use WiFiClientSecure with the CA bundle
    //      (RFC-0001 full TLS verification). setupTelegramClient() also calls
    //      realBot.setTransport() to inject the WiFi client.
    //   2. If WiFi unavailable (SSID not set, AP unreachable, timeout), try
    //      GPRS/LTE data via the modem. The modem TLS path uses authmode=0
    //      (no server certificate verification — see the [CELLULAR TLS]
    //      warning printed by setupCellularClient() for the rationale).
    //      Requires CELLULAR_APN to be defined in secrets.h.
    //
    // NTP sync is attempted after WiFi connects. It is skipped on the
    // cellular path (the modem provides time via AT+CCLK; NTP over LTE is
    // possible but not yet implemented).
    bool transportReady = false;
    if (connectToWiFi())
    {
        syncTime();
        // setupTelegramClient() configures the CA bundle and sets the
        // WiFi client as the active transport on realBot.
        if (setupTelegramClient(realBot))
        {
            activeTransport = ActiveTransport::kWiFi;
            transportReady = true;
        }
        else
        {
            Serial.println("WiFi Telegram setup failed.");
        }
    }

    if (!transportReady)
    {
        Serial.println("WiFi unavailable; attempting cellular fallback...");
#if defined(CELLULAR_APN) && defined(TINY_GSM_MODEM_A76XXSSL)
        const char *apn = CELLULAR_APN;
        if (strlen(apn) == 0)
        {
            Serial.println("Cellular fallback: CELLULAR_APN is empty, trying without APN.");
        }
        Serial.printf("Connecting to GPRS (APN: \"%s\")...\n", apn);
        if (modem.gprsConnect(apn))
        {
            Serial.println("GPRS connected.");
            // setupCellularClient() sets authmode=0 (no cert verification),
            // prints the [CELLULAR TLS] warning, and sets the modem client
            // as the active transport on realBot.
            if (setupCellularClient(realBot))
            {
                activeTransport = ActiveTransport::kCellular;
                transportReady = true;
            }
            else
            {
                Serial.println("Cellular Telegram setup failed.");
                modem.gprsDisconnect();
            }
        }
        else
        {
            Serial.println("GPRS connect failed.");
        }
#else
        Serial.println("Cellular fallback not available (define CELLULAR_APN in secrets.h and rebuild).");
#endif
    }

    if (!transportReady)
    {
        Serial.println("No network transport available. SMS bridge running in receive-only mode (no Telegram forwarding).");
        // Continue: SMS receive still works; Telegram sends will fail gracefully.
    }

    // RFC-0014: Parse the allow list and wire the admin chat id into the
    // bot client. Must happen before registerBotCommands (which fires off
    // an API call that doesn't use the chat id, but ordering is cleaner)
    // and before the TelegramPoller is constructed (AuthFn uses allowedIds).
    allowedIdCount = parseAllowedIds(TELEGRAM_CHAT_IDS, allowedIds, 10);
    if (allowedIdCount == 0)
    {
        Serial.println("WARNING: no valid Telegram chat IDs found in TELEGRAM_CHAT_IDS — all updates will be rejected.");
    }
    else
    {
        Serial.printf("Allow list: %d user(s). Admin (forward target): %lld\n",
                      allowedIdCount, (long long)allowedIds[0]);
    }
    realBot.setAdminChatId(allowedIdCount > 0 ? allowedIds[0] : 0);

    registerBotCommands(realBot);

    // RFC-0003 / RFC-0010: Build the TelegramPoller with a StatusFn
    // that closes over all composition-root objects. All captured
    // references are to file-scope statics with process lifetime.
    // cachedCsq and cachedRegStatus are file-scope statics updated
    // every 30 s in loop() — safe to read from this lambda because
    // the lambda runs inside telegramPoller.tick() which is called
    // AFTER the cache refresh block in loop().
    //
    // RFC-0017: statusFn is a file-scope static (assigned here, read in
    // loop() for the scheduled heartbeat). The lambda body is identical to
    // the previous inline 7th argument; only the assignment location moves.
    statusFn = []() -> String {
        // --- uptime ---
        unsigned long uptimeSec = millis() / 1000UL;
        unsigned long days = uptimeSec / 86400UL;
        unsigned long hours = (uptimeSec % 86400UL) / 3600UL;
        unsigned long mins = (uptimeSec % 3600UL) / 60UL;

        // --- current time with timezone offset ---
        char timeBuf[32] = "(no NTP)";
        {
            time_t now = time(nullptr);
            if (now > 8 * 3600 * 2) // NTP has synced
            {
                time_t local = now + TIMEZONE_OFFSET_SEC;
                struct tm *t = gmtime(&local);
                strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M", t);
            }
        }
        // Timezone label: "UTC" or "UTC+8" / "UTC-5"
        char tzLabel[16] = "UTC";
        if (TIMEZONE_OFFSET_SEC != 0)
        {
            int offsetH = TIMEZONE_OFFSET_SEC / 3600;
            int offsetM = (TIMEZONE_OFFSET_SEC % 3600) / 60;
            if (offsetM != 0)
                snprintf(tzLabel, sizeof(tzLabel), "UTC%+d:%02d", offsetH, offsetM < 0 ? -offsetM : offsetM);
            else
                snprintf(tzLabel, sizeof(tzLabel), "UTC%+d", offsetH);
        }

        // --- CSQ interpretation ---
        const char *csqLabel;
        if (cachedCsq == 99)
            csqLabel = "none";
        else if (cachedCsq <= 9)
            csqLabel = "marginal";
        else if (cachedCsq <= 14)
            csqLabel = "ok";
        else if (cachedCsq <= 19)
            csqLabel = "good";
        else
            csqLabel = "excellent";

        // --- registration status ---
        const char *regStr;
        switch (cachedRegStatus)
        {
        case REG_OK_HOME:      regStr = "home";          break;
        case REG_OK_ROAMING:   regStr = "roaming";       break;
        case REG_SEARCHING:    regStr = "searching";     break;
        case REG_DENIED:       regStr = "denied";        break;
        case REG_UNREGISTERED: regStr = "unregistered";  break;
        default:               regStr = "unknown";       break;
        }

        // --- assemble message ---
        String msg;

        msg += "\xF0\x9F\x93\xA1 Device\n"; // 📡
        msg += "  Time: ";      msg += timeBuf; msg += " "; msg += tzLabel; msg += "\n";
        msg += "  Uptime: ";    msg += String((int)days); msg += "d "; msg += String((int)hours); msg += "h "; msg += String((int)mins); msg += "m\n";
        msg += "  WiFi: ";      msg += String(WiFi.RSSI()); msg += " dBm\n";
        msg += "  Modem: CSQ "; msg += String(cachedCsq); msg += " ("; msg += csqLabel; msg += ")  "; msg += regStr;
        if (cachedOperatorName.length() > 0) { msg += " ("; msg += cachedOperatorName; msg += ")"; }
        // RFC-0031: Append CSQ trend (oldest→newest, only if we have history).
        if (csqHistoryFull || csqHistoryIdx > 0)
        {
            msg += " [";
            int total = csqHistoryFull ? 6 : csqHistoryIdx;
            int start = csqHistoryFull ? csqHistoryIdx : 0;
            for (int i = 0; i < total; i++)
            {
                if (i > 0) msg += " ";
                msg += String(csqHistory[(start + i) % 6]);
            }
            msg += "]";
        }
        msg += "\n";
        msg += "  Heap: ";      msg += String((int)ESP.getFreeHeap()); msg += " B\n";
        // RFC-0030b: Flash usage.
        msg += "  Flash: ";     msg += String((int)(ESP.getSketchSize() / 1024)); msg += "kB / ";
        msg += String((int)((ESP.getSketchSize() + ESP.getFreeSketchSpace()) / 1024)); msg += "kB\n";
        msg += "  Reset: ";     msg += resetReasonStr(s_resetReason); msg += "\n";

        msg += "\n\xF0\x9F\x93\xA8 SMS\n"; // 📨
        msg += "  Forwarded: "; msg += String(smsHandler.smsForwarded()); msg += "\n";
        msg += "  Failed: ";    msg += String(smsHandler.smsFailed()); msg += "\n";
        msg += "  Consec. failures: "; msg += String(smsHandler.consecutiveFailures()); msg += "\n";
        msg += "  Concat in-flight: "; msg += String((int)smsHandler.concatKeyCount()); msg += "\n";
        // RFC-0034: show outbound retry queue depth.
        msg += "  Outbound queue: "; msg += String(smsSender.queueSize()); msg += "/"; msg += String(SmsSender::kQueueSize); msg += "\n";

        msg += "\n\xF0\x9F\x92\xAC Telegram\n"; // 💬
        msg += "  Reply slots: ";
        msg += String((int)replyTargets.occupiedSlots()); msg += "/"; msg += String((int)ReplyTargetMap::kSlotCount); msg += "\n";
        if (telegramPoller)
        {
            msg += "  Polls: ";     msg += String(telegramPoller->pollAttempts()); msg += "\n";
            msg += "  update_id: "; msg += String((long)telegramPoller->lastUpdateId()); msg += "\n";
        }

        msg += "\n\xF0\x9F\x90\x9B Debug log: "; // 🐛
        msg += String((int)smsDebugLog.count()); msg += "/"; msg += String((int)SmsDebugLog::kMaxEntries); msg += "\n";

        msg += "\n\xE2\x9A\x99\xEF\xB8\x8F Config\n"; // ⚙️
        msg += "  Users: ";      msg += String(allowedIdCount); msg += "\n";
        msg += "  Block list: "; msg += String(sBlockListCount + sRuntimeBlockListCount); msg += "\n";
        return msg;
    };

    telegramPoller = new TelegramPoller(
        realBot, smsSender, replyTargets, realPersist,
        []() -> uint32_t { return (uint32_t)millis(); },
        [](int64_t fromId) -> bool {
            if (fromId == 0) return false;
            for (int i = 0; i < allowedIdCount; i++)
            {
                if (fromId == allowedIds[i]) return true;
            }
            return false;
        },
        statusFn,
        // ListMutatorFn — only handles /restart (RFC-0023). Admin check
        // (callerId in compile-time list) performed here.
        [](int64_t callerId, const String &cmd, int64_t /*targetId*/, String &reason) -> bool {
            bool isAdmin = false;
            for (int i = 0; i < allowedIdCount; i++)
            {
                if (callerId == allowedIds[i]) { isAdmin = true; break; }
            }
            if (cmd == "restart")
            {
                if (!isAdmin)
                {
                    reason = String("Admin access required.");
                    return false;
                }
                reason = String("\xF0\x9F\x94\x84 Restarting..."); // U+1F504
                s_pendingRestart = true;
                return true;
            }
            reason = String("Unknown command.");
            return false;
        },
        // RFC-0021: SmsBlockMutatorFn — handles /block, /unblock, /blocklist.
        // Admin check (callerId in compile-time list) performed here.
        // All state is file-scope statics with process lifetime.
        [](int64_t callerId, const String &cmd, const String &number, String &reason) -> bool {
            bool isAdmin = false;
            for (int i = 0; i < allowedIdCount; i++)
                if (callerId == allowedIds[i]) { isAdmin = true; break; }

            // /blocklist — any authorized user.
            if (cmd == "list")
            {
                String reply;
                reply += "Compile-time block list (" + String(sBlockListCount) + "):\n";
                for (int i = 0; i < sBlockListCount; i++)
                    reply += String("  ") + sBlockList[i] + "\n";
                reply += "\nRuntime block list (" + String(sRuntimeBlockListCount) + "):\n";
                for (int i = 0; i < sRuntimeBlockListCount; i++)
                    reply += String("  ") + sRuntimeBlockList[i] + "\n";
                if (sRuntimeBlockListCount == 0 && sBlockListCount == 0)
                    reply = "(No numbers blocked)";
                reason = reply;
                return true;
            }

            // Mutating commands require admin.
            if (!isAdmin)
            {
                reason = String("Admin access required.");
                return false;
            }

            if (cmd == "block")
            {
                if (isBlocked(number.c_str(), sBlockList, sBlockListCount))
                {
                    reason = number + " is already in the compile-time block list.";
                    return false;
                }
                if (isBlocked(number.c_str(), sRuntimeBlockList, sRuntimeBlockListCount))
                {
                    reason = number + " is already in the runtime block list.";
                    return false;
                }
                if (sRuntimeBlockListCount >= kSmsBlockListMaxEntries)
                {
                    reason = String("Runtime block list full (max ") +
                             String(kSmsBlockListMaxEntries) +
                             " entries). Use /unblock to remove one first.";
                    return false;
                }
                if ((int)number.length() > kSmsBlockListMaxNumberLen)
                {
                    reason = String("Number too long (max ") +
                             String(kSmsBlockListMaxNumberLen) + " characters).";
                    return false;
                }
                memcpy(sRuntimeBlockList[sRuntimeBlockListCount],
                       number.c_str(), number.length() + 1);
                sRuntimeBlockListCount++;
                smsHandler.setRuntimeBlockList(sRuntimeBlockList, sRuntimeBlockListCount);
                struct { int32_t count; char numbers[20][21]; } blob{};
                blob.count = sRuntimeBlockListCount;
                memcpy(blob.numbers, sRuntimeBlockList,
                       (size_t)sRuntimeBlockListCount * (kSmsBlockListMaxNumberLen + 1));
                realPersist.saveBlob("smsblist", &blob, sizeof(blob));
                Serial.printf("SMS block list: added %s (%d runtime entries)\n",
                              number.c_str(), sRuntimeBlockListCount);
                return true;
            }

            if (cmd == "unblock")
            {
                int found = -1;
                for (int i = 0; i < sRuntimeBlockListCount; i++)
                    if (strcmp(sRuntimeBlockList[i], number.c_str()) == 0) { found = i; break; }
                if (found < 0)
                {
                    if (isBlocked(number.c_str(), sBlockList, sBlockListCount))
                        reason = number + " is in the compile-time list and cannot be removed at runtime.";
                    else
                        reason = number + " is not in the runtime block list.";
                    return false;
                }
                for (int i = found; i < sRuntimeBlockListCount - 1; i++)
                    memcpy(sRuntimeBlockList[i], sRuntimeBlockList[i + 1],
                           kSmsBlockListMaxNumberLen + 1);
                memset(sRuntimeBlockList[sRuntimeBlockListCount - 1], 0,
                       kSmsBlockListMaxNumberLen + 1);
                sRuntimeBlockListCount--;
                smsHandler.setRuntimeBlockList(sRuntimeBlockList, sRuntimeBlockListCount);
                struct { int32_t count; char numbers[20][21]; } blob{};
                blob.count = sRuntimeBlockListCount;
                memcpy(blob.numbers, sRuntimeBlockList,
                       (size_t)sRuntimeBlockListCount * (kSmsBlockListMaxNumberLen + 1));
                realPersist.saveBlob("smsblist", &blob, sizeof(blob));
                Serial.printf("SMS block list: removed %s (%d runtime entries)\n",
                              number.c_str(), sRuntimeBlockListCount);
                return true;
            }

            reason = String("Unknown command.");
            return false;
        });

    // RFC-0003 persistence: open NVS, hydrate the reply-target ring
    // buffer and the last-seen update_id watermark, and wire the
    // ring buffer into the SmsHandler so future SMS forwards
    // populate it. If NVS init fails, log and continue without
    // bidirectional support — receive-only is still useful.
    if (!realPersist.begin())
    {
        Serial.println("RealPersist::begin failed; bidirectional TG->SMS disabled.");
        // RFC-0025: Alert via Telegram so remote operators know NVS is
        // unavailable. Best-effort — if transport isn't ready this is a no-op.
        realBot.sendMessage(String(
            "\xE2\x9A\xA0\xEF\xB8\x8F NVS init failed\n"  // U+26A0 ⚠️
            "Persistent state (reply targets, block list, SMS log) is unavailable "
            "for this session. Consider erasing NVS: pio run -t erase"));
    }
    else
    {
        // RFC-0021: Load runtime SMS block list from NVS.
        {
            struct { int32_t count; char numbers[20][21]; } blob{};
            size_t got = realPersist.loadBlob("smsblist", &blob, sizeof(blob));
            if (got >= sizeof(int32_t) && blob.count >= 0 && blob.count <= 20)
            {
                sRuntimeBlockListCount = blob.count;
                memcpy(sRuntimeBlockList, blob.numbers,
                       (size_t)blob.count * (kSmsBlockListMaxNumberLen + 1));
            }
            Serial.printf("Runtime SMS block list: %d entr%s\n",
                          sRuntimeBlockListCount,
                          sRuntimeBlockListCount == 1 ? "y" : "ies");
        }

        // RFC-0020: Restore the last 10 SMS debug log entries from NVS so
        // /debug shows history from before the reboot, then register the
        // sink so every subsequent push() persists to NVS.
        smsDebugLog.loadFrom(realPersist);
        smsDebugLog.setSink(realPersist);

        replyTargets.load();
        smsHandler.setReplyTargetMap(&replyTargets);
        smsHandler.setDebugLog(&smsDebugLog);
        telegramPoller->setDebugLog(&smsDebugLog);
        smsSender.setDebugLog(&smsDebugLog); // RFC-0035: log outbound failures
        telegramPoller->begin();
        Serial.print("TG->SMS poller online; reply-target slots in use: ");
        Serial.println((unsigned long)replyTargets.occupiedSlots());
    }

    // RFC-0018: Parse and apply the compile-time SMS sender block list.
#ifdef SMS_BLOCK_LIST
    sBlockListCount = parseBlockList(SMS_BLOCK_LIST, sBlockList, kSmsBlockListMaxEntries);
    Serial.print("SMS block list: ");
    Serial.print(sBlockListCount);
    Serial.println(" entries");
    if (sBlockListCount == kSmsBlockListMaxEntries)
        Serial.println("[WARN] Block list truncated at max entries — check SMS_BLOCK_LIST");
    smsHandler.setBlockList(sBlockList, sBlockListCount);
#endif

    // RFC-0021: Apply runtime SMS block list (loaded from NVS above, or empty on first boot).
    smsHandler.setRuntimeBlockList(sRuntimeBlockList, sRuntimeBlockListCount);

    // RFC-0022: Rich startup notification. Prime cachedCsq and operator name
    // so the boot banner shows real values (modem is idle here).
    cachedCsq = modem.getSignalQuality();
    // RFC-0027: Also prime operator name at boot.
    {
        String copsResp;
        modem.sendAT("+COPS?");
        modem.waitResponse(2000UL, copsResp);
        int q1 = copsResp.indexOf('"');
        int q2 = (q1 >= 0) ? copsResp.indexOf('"', q1 + 1) : -1;
        cachedOperatorName = (q1 >= 0 && q2 > q1)
                             ? copsResp.substring(q1 + 1, q2)
                             : String();
    }
    {
        String bootMsg = String("\xF0\x9F\x9A\x80 Bridge online\n"); // U+1F680 rocket
        if (statusFn)
            bootMsg += statusFn();
        else
            bootMsg += "(status not available)";
        realBot.sendMessage(bootMsg);
    }

    // Drain anything that arrived while we were offline.
    smsHandler.sweepExistingSms();

    // Hardware watchdog (RFC-0015). Initialized at the end of setup() so
    // unbounded startup sections (modem probe, network registration, NTP)
    // are not covered. loop() resets it at the top of every iteration.
    // IDF v5+ uses esp_task_wdt_reconfigure(); IDF v4 uses esp_task_wdt_init().
    {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        esp_task_wdt_config_t wdtCfg = {
            .timeout_ms = 120000,
            .idle_core_mask = 0,
            .trigger_panic = false, // soft reset → ESP_RST_WDT → "/status" shows "watchdog"
        };
        esp_task_wdt_reconfigure(&wdtCfg);
#else
        // IDF v4: timeout in seconds, panic flag
        esp_task_wdt_init(120, false);  // 120s, no panic
#endif
        esp_task_wdt_add(NULL);
        Serial.println("Hardware watchdog armed (120s timeout).");
    }
}

void loop()
{
    esp_task_wdt_reset();  // RFC-0015: keep the hardware watchdog alive

    // RFC-0023: Deferred restart from /restart bot command. The restart flag
    // is set by the ListMutatorFn lambda AFTER the "Restarting..." Telegram
    // reply was sent, so the message has already been delivered before we reset.
    if (s_pendingRestart)
    {
        Serial.println("/restart command received, rebooting...");
        delay(500); // let serial flush
        ESP.restart();
    }

    // NOTE: do NOT call modem.maintain() here. On TinyGSM/A76XX it internally
    // calls waitResponse() which eats unknown URCs (+CMTI included) and only
    // prints "### Unhandled: ..." in debug mode — meaning our +CMTI would be
    // consumed before we ever see it in SerialAT.available() below.
    // We drain the serial buffer ourselves and dispatch the URCs we care about.

    // Consume unsolicited lines and dispatch to the SMS / Call handlers.
    // +CDS state machine (RFC-0011): +CDS is a two-line URC.
    // The first line "+CDS: <length>" sets a flag; the very next line
    // (the PDU hex) is then routed to deliveryReportHandler.
#ifdef ENABLE_DELIVERY_REPORTS
    static bool waitingCdsPdu = false;
#endif
    while (SerialAT.available())
    {
        String line = SerialAT.readStringUntil('\n');
        line.trim();
        if (line.length() == 0)
            continue;

#ifdef ENABLE_DELIVERY_REPORTS
        // Second line of a +CDS URC: this is the PDU hex.
        if (waitingCdsPdu)
        {
            waitingCdsPdu = false;
            deliveryReportHandler.onStatusReport(line);
            continue;
        }

        // First line of a +CDS URC: "+CDS: <length>".
        if (line.startsWith("+CDS:"))
        {
            waitingCdsPdu = true;
            continue; // PDU arrives on the next line
        }
#endif

        if (line.startsWith("+CMTI:"))
        {
            int comma = line.indexOf(',');
            if (comma != -1)
            {
                int idx = line.substring(comma + 1).toInt();
                if (idx > 0)
                {
                    smsHandler.handleSmsIndex(idx);
                }
            }
        }

        // CallHandler gobbles RING / +CLIP. Feed it every line; it
        // does its own startsWith filtering and ignores anything else.
        callHandler.onUrcLine(line);
    }

    // Drive the CallHandler's unknown-number deadline + cooldown timer.
    // Cheap: constant-time and does no AT traffic unless a deadline fires.
    callHandler.tick();

    // Refresh cached modem CSQ + registration status every 30 s.
    // This block runs AFTER the URC drain and BEFORE the TelegramPoller
    // tick so the AT commands here cannot race with URC lines being read
    // from SerialAT. The StatusFn lambda (called from inside the poller)
    // reads these cached values instead of calling the modem directly,
    // avoiding the URC-eating hazard described in CLAUDE.md.
    static unsigned long lastStatusRefreshMs = 0;
    if (millis() - lastStatusRefreshMs > 30000UL)
    {
        lastStatusRefreshMs = millis();
        cachedCsq = modem.getSignalQuality();
        cachedRegStatus = modem.getRegistrationStatus();
        // RFC-0031: Record CSQ sample in rolling history.
        csqHistory[csqHistoryIdx] = cachedCsq;
        csqHistoryIdx = (csqHistoryIdx + 1) % 6;
        if (csqHistoryIdx == 0) csqHistoryFull = true;
        // RFC-0027: Cache operator name via AT+COPS?
        {
            String copsResp;
            modem.sendAT("+COPS?");
            modem.waitResponse(2000UL, copsResp);
            int q1 = copsResp.indexOf('"');
            int q2 = (q1 >= 0) ? copsResp.indexOf('"', q1 + 1) : -1;
            cachedOperatorName = (q1 >= 0 && q2 > q1)
                                 ? copsResp.substring(q1 + 1, q2)
                                 : String();
        }
#ifdef ENABLE_DELIVERY_REPORTS
        // Evict delivery report map entries older than 1 hour (RFC-0011).
        deliveryReportMap.evictExpired((uint32_t)millis());
#endif
    }

    // Drive the TG->SMS poller (RFC-0003). Rate-limited internally to
    // kPollIntervalMs; uses short polling so it doesn't block the URC
    // drain above. See telegram_poller.h "Implementation note" for
    // why we don't long-poll in the first cut.
    if (telegramPoller)
        telegramPoller->tick();

    // Drain one pending outbound SMS per loop iteration (RFC-0012).
    // Placed after the poller tick so AT commands are not issued while
    // the poller HTTP exchange is in flight. One send per loop keeps
    // the loop non-blocking from the perspective of URC processing.
    // NOTE: multi-PDU sends inside send() are atomic from the queue's
    // perspective — a +CMTI URC that arrives mid-PDU-sequence will sit
    // in the SerialAT RX buffer and be serviced on the next loop() call.
    smsSender.drainQueue((uint32_t)millis());

    // RFC-0017: Periodic heartbeat. Reuses statusFn output — same content as
    // the /status command. Fires once per HEARTBEAT_INTERVAL_SEC starting after
    // the first full interval post-boot (the boot banner already covers
    // "just started"). Uses unsigned subtraction so millis() wraparound at
    // ~49.7 days is handled correctly.
    // lastHeartbeatMs is advanced regardless of send result: a missed heartbeat
    // during a connectivity failure is itself a signal; no retry queue is needed.
#if HEARTBEAT_INTERVAL_SEC != 0
    {
        uint32_t nowMs = (uint32_t)millis();
        if ((uint32_t)(nowMs - lastHeartbeatMs) >= (uint32_t)HEARTBEAT_INTERVAL_SEC * 1000u)
        {
            lastHeartbeatMs = nowMs;  // advance regardless of send result
            if (statusFn)
            {
                esp_task_wdt_reset();  // heartbeat sendMessage can block; pet WDT first
                String msg = String("⏱ Heartbeat\n") + statusFn();
                if (!realBot.sendMessage(msg))
                    Serial.println("Heartbeat: sendMessage failed (connectivity issue)");
            }
        }
    }
#endif

    // Periodically verify the active transport is still viable.
    // - If WiFi was primary and drops: try WiFi reconnect. If it's still down
    //   on the second consecutive check (~60 s later), fall over to cellular
    //   (RFC-0004). We use a flag rather than a blocking delay so the URC
    //   drain keeps running uninterrupted.
    // - If cellular was primary: check whether WiFi has recovered; if so,
    //   switch back to the verified (CA-bundle) path.
    static unsigned long lastTransportCheck = 0;
    static bool wifiDownLastCheck = false;
    if (millis() - lastTransportCheck > 30000)
    {
        lastTransportCheck = millis();
        if (activeTransport == ActiveTransport::kWiFi)
        {
            if (WiFi.status() != WL_CONNECTED)
            {
                Serial.println("WiFi dropped, attempting reconnect...");
                WiFi.reconnect();
                if (wifiDownLastCheck)
                {
                    // Two consecutive checks with WiFi down — fall over.
#if defined(CELLULAR_APN) && defined(TINY_GSM_MODEM_A76XXSSL)
                    Serial.println("WiFi still down; falling over to cellular transport...");
                    if (modem.gprsConnect(CELLULAR_APN))
                    {
                        if (setupCellularClient(realBot))
                        {
                            activeTransport = ActiveTransport::kCellular;
                            wifiDownLastCheck = false;
                            Serial.println("Switched to cellular transport.");
                        }
                    }
#endif
                }
                else
                {
                    wifiDownLastCheck = true;
                }
            }
            else
            {
                wifiDownLastCheck = false;
            }
        }
        else if (activeTransport == ActiveTransport::kCellular)
        {
            // Non-blocking WiFi recovery check: on the first tick we kick off
            // WiFi.begin(); on the second tick (30s later) we check if it
            // connected. This avoids blocking the URC drain for up to 15s as
            // the old connectToWiFi() would. See rfc/0004-cellular-fallback.md
            // §Code Review (URC-eating blocker).
            static bool wifiBeginPending = false;
            if (strlen(ssid) > 0)
            {
                if (!wifiBeginPending)
                {
                    WiFi.mode(WIFI_STA);
                    WiFi.begin(ssid, password);
                    wifiBeginPending = true;
                    Serial.println("WiFi recovery: begin() issued, checking next tick.");
                }
                else
                {
                    wifiBeginPending = false; // reset for next cycle regardless
                    if (WiFi.status() == WL_CONNECTED)
                    {
                        Serial.println("WiFi recovered; switching from cellular to WiFi transport.");
                        syncTime();
                        if (setupTelegramClient(realBot))
                        {
                            activeTransport = ActiveTransport::kWiFi;
                            Serial.println("Transport switched to WiFi.");
                        }
                    }
                    else
                    {
                        Serial.println("WiFi recovery check: still not connected.");
                    }
                }
            }
        }
        else if (activeTransport == ActiveTransport::kNone)
        {
            // No transport yet — retry both paths on each check.
            // Non-blocking WiFi attempt: issue begin() one tick, check next.
            static bool wifiBeginPendingNone = false;
            if (strlen(ssid) > 0 && !wifiBeginPendingNone)
            {
                WiFi.mode(WIFI_STA);
                WiFi.begin(ssid, password);
                wifiBeginPendingNone = true;
            }
            else if (wifiBeginPendingNone)
            {
                wifiBeginPendingNone = false;
                if (WiFi.status() == WL_CONNECTED)
                {
                    syncTime();
                    if (setupTelegramClient(realBot))
                    {
                        activeTransport = ActiveTransport::kWiFi;
                        Serial.println("WiFi transport established (deferred).");
                    }
                }
            }
#if defined(CELLULAR_APN) && defined(TINY_GSM_MODEM_A76XXSSL)
            if (activeTransport == ActiveTransport::kNone)
            {
                if (modem.gprsConnect(CELLULAR_APN))
                {
                    if (setupCellularClient(realBot))
                    {
                        activeTransport = ActiveTransport::kCellular;
                        Serial.println("Cellular transport established (deferred).");
                    }
                }
            }
#endif
        }
    }

    delay(50);
}
