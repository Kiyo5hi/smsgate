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
#include <nvs.h>

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
static SmsAliasStore    smsAliasStore(realPersist);    // RFC-0088
static SmsTemplateStore smsTemplateStore(realPersist); // RFC-0227
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

// RFC-0071: Deferred WiFi reconnect flag. Set by the /wifi bot command lambda;
// loop() reconnects WiFi and re-initialises the Telegram client on the next
// iteration AFTER the "WiFi reconnect initiated" Telegram reply has been sent.
static bool s_pendingWifiReconnect = false;

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
// RFC-0045: Modem firmware version (AT+CGMR), queried once at boot.
static String cachedModemFirmware;
// RFC-0076: Modem IMEI (AT+GSN), queried once at boot.
static String cachedImei;
// RFC-0077: SIM ICCID (AT+CICCID), queried once at boot.
static String cachedIccid;
// RFC-0031: CSQ trend — last 6 readings (one per 30s refresh = 3 min window).
static int csqHistory[6] = {0, 0, 0, 0, 0, 0};
static int csqHistoryIdx = 0;
static bool csqHistoryFull = false;
// RFC-0036: SIM slot usage from AT+CPMS?, refreshed every 30s.
static int cachedSimUsed  = -1; // -1 = not yet queried
static int cachedSimTotal = 0;
// RFC-0038: Boot timestamp — captured once on first successful NTP sync.
static time_t s_bootTimestamp = 0;
// RFC-0115: Timestamp of the last successful NTP sync. 0 = never synced.
static time_t s_lastNtpSyncTime = 0;
// RFC-0041: Wall-clock time of the most recently forwarded SMS (0 = none yet).
static time_t s_lastSmsTimestamp = 0;
// RFC-0059: Cumulative boot counter, incremented in setup() and persisted to NVS.
static uint32_t s_bootCount = 0;
// RFC-0060: Lifetime SMS forward count across all reboots, persisted to NVS.
static uint32_t s_lifetimeFwdCount = 0;
// RFC-0064: SIM slot full warning — set when usage crosses ≥80%; cleared when
// usage drops back below the threshold so the alert can re-fire if needed.
static bool s_simFullWarnSent = false;
// RFC-0066: Low heap warning. Hysteresis: alert at <15 KB, clear at >25 KB.
static bool s_lowHeapWarnSent = false;
// RFC-0081: CSQ low-signal alert. Hysteresis: alert at ≤5, clear at >10.
static bool s_lowCsqWarnSent = false;
// RFC-0082: Network registration loss alert.
static bool s_regLostAlertSent = false;
// RFC-0113: WiFi low-RSSI alert (< -80 dBm).
static bool s_lowWifiRssiAlertSent = false;
// RFC-0118: Custom device label (max 32 chars), loaded from NVS at boot.
static String s_deviceLabel;
// RFC-0131: Device note (max 120 chars), loaded from NVS at boot.
static String s_deviceNote;
// RFC-0075: Daily stats digest. Initialised to 0 so the first digest sends
// 24 h after boot (set on first 30-second tick, fire 24 h later).
static uint32_t s_lastDailyDigestMs = 0;
// RFC-0079: NTP retry. Retry every 5 minutes while clock is still invalid.
static uint32_t s_lastNtpRetryMs = 0;
static constexpr uint32_t kNtpRetryIntervalMs = 5UL * 60UL * 1000UL;
// RFC-0096: Stuck-queue alert. Fire if any entry is older than 5 minutes.
static bool     s_stuckQueueAlertSent = false;
static uint32_t s_lastStuckQueueCheckMs = 0;
static constexpr uint32_t kStuckQueueThresholdMs = 5UL * 60UL * 1000UL;  // 5 min
static constexpr uint32_t kStuckQueueCheckIntervalMs = 60UL * 1000UL;    // 1 min
// RFC-0098: Alert mute. 0 = not muted; else millis() timestamp until which alerts are silenced.
static uint32_t s_alertsMutedUntilMs = 0;
inline bool alertsMuted() { return (uint32_t)millis() < s_alertsMutedUntilMs; }
// RFC-0192: Forward pause. 0 = not paused; else millis() timestamp until which forwarding is off.
static uint32_t s_fwdPauseUntilMs = 0;
// RFC-0201: Scheduled SMS slots restored from NVS at boot (for boot banner).
static int s_schedLoadedAtBoot = 0;
// RFC-0208: Timestamp of last successful Telegram API contact (0 = never this boot).
static time_t s_lastTelegramOkTime = 0;
// RFC-0238: Deferred boot sweep flag. Set if the boot banner send failed
// (Telegram unreachable at boot). Cleared by the first successful pollUpdates.
static bool s_needBootSweep = false;
// RFC-0102: Boot time for uptime display in /status.
static uint32_t s_bootMs = 0;

// RFC-0017: StatusFn promoted to file scope so loop() can call it for
// the scheduled heartbeat. Assigned in setup() before TelegramPoller is
// constructed. Default-constructed to nullptr; guarded with `if (statusFn)`
// before use.
static std::function<String()> statusFn;

// RFC-0017: Heartbeat timer. Initialised to 0 so the first heartbeat fires
// one full interval after boot (the boot banner already covers "just started").
// Unconditionally declared so /hbnow (RFC-0177) can reset it from a lambda.
static uint32_t lastHeartbeatMs = 0;
// RFC-0137: Runtime-overridable heartbeat interval. Loaded from NVS "hb_interval"
// at boot; set via /setinterval. 0 = disabled. Defaults to HEARTBEAT_INTERVAL_SEC.
static uint32_t s_heartbeatIntervalSec = HEARTBEAT_INTERVAL_SEC;
// RFC-0150: Optional SMS auto-reply text. Loaded from NVS "autoreply" at boot.
// Empty string = disabled. Set via /setautoreply, cleared via /clearautoreply.
static String s_autoReplyText;

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
    // RFC-0249: 30-second deadline. If NTP is unreachable the loop would
    // otherwise spin forever (with WDT kicks), blocking all URC processing
    // when called from loop().  After the deadline, return and let RFC-0079
    // retry on the next iteration.
    unsigned long ntpDeadline = millis() + 30000UL;
    while (now < 8 * 3600 * 2 && millis() < ntpDeadline)
    {
        esp_task_wdt_reset();  // RFC-0015: syncTime() is also called from loop()
        delay(500);
        Serial.print(".");
        now = time(nullptr);
    }
    if (now < 8 * 3600 * 2)
    {
        Serial.println("\nsyncTime: NTP timed out (30 s deadline)");
        return; // leave s_bootTimestamp / s_lastNtpSyncTime unchanged
    }
    Serial.printf("\nCurrent time: %s\n", ctime(&now));
    // RFC-0038: Record boot timestamp on first NTP sync.
    if (s_bootTimestamp == 0)
        s_bootTimestamp = now;
    // RFC-0115: Track last successful NTP sync time.
    s_lastNtpSyncTime = now;
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

// RFC-0250: File-static helper that parses a "+CREG: ..." line, updates
// cachedRegStatus, and fires the RFC-0082 registration-lost/restored alert.
// Accepts both the unsolicited format "+CREG: <stat>" (AT+CREG=1 mode) and
// the solicited format "+CREG: <n>,<stat>" that can piggyback in raw AT
// response buffers. The alert logic is idempotent via s_regLostAlertSent,
// so calling this from multiple dispatch paths is harmless.
static void handleCregUrc(const String &line, const char *logTag)
{
    int colon = line.indexOf(':');
    String rest = line.substring(colon + 1);
    rest.trim();
    int comma = rest.indexOf(',');
    int stat = (comma >= 0) ? rest.substring(comma + 1).toInt() : rest.toInt();
    switch (stat) {
        case 1:  cachedRegStatus = REG_OK_HOME;      break;
        case 5:  cachedRegStatus = REG_OK_ROAMING;   break;
        case 6:  cachedRegStatus = REG_SMS_ONLY;     break;
        case 2:  cachedRegStatus = REG_SEARCHING;    break;
        case 3:  cachedRegStatus = REG_DENIED;       break;
        case 4:  cachedRegStatus = REG_UNKNOWN;      break;
        default: cachedRegStatus = REG_UNREGISTERED; break;
    }
    bool regOk = (cachedRegStatus == REG_OK_HOME    ||
                  cachedRegStatus == REG_OK_ROAMING ||
                  cachedRegStatus == REG_SMS_ONLY);
    const char *regTxt = (cachedRegStatus == REG_OK_HOME)      ? "home"
                       : (cachedRegStatus == REG_OK_ROAMING)   ? "roaming"
                       : (cachedRegStatus == REG_SMS_ONLY)     ? "sms-only"
                       : (cachedRegStatus == REG_SEARCHING)    ? "searching"
                       : (cachedRegStatus == REG_DENIED)       ? "denied"
                       : (cachedRegStatus == REG_UNREGISTERED) ? "unregistered"
                       : (cachedRegStatus == REG_UNKNOWN)      ? "unknown"
                       :                                         "unknown";
    Serial.printf("[%s] +CREG URC: stat=%d (%s)\n", logTag, stat, regTxt);
    if (!regOk && !s_regLostAlertSent && cachedCsq > 0) {
        if (!alertsMuted())
            realBot.sendMessage(
                String("\xF0\x9F\x93\xB5 Network registration lost (") + regTxt + ")"); // 📵
        s_regLostAlertSent = true;
    } else if (regOk && s_regLostAlertSent) {
        if (!alertsMuted())
            realBot.sendMessage(
                String("\xE2\x9C\x85 Network registration restored (") + regTxt + ")"); // ✅
        s_regLostAlertSent = false;
    }
}

void setup()
{
    // RFC-0020: Capture reset reason once at startup (before any code path
    // can trigger another reset). Used in the /status lambda below.
    static esp_reset_reason_t s_resetReason = esp_reset_reason();

    // RFC-0059: Increment cumulative boot counter in NVS.
    {
        uint32_t bc = 0;
        realPersist.loadBlob("bootcnt", &bc, sizeof(bc));
        bc++;
        realPersist.saveBlob("bootcnt", &bc, sizeof(bc));
        s_bootCount = bc;
    }

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

    Serial1.setRxBufferSize(2048); // RFC-0232: enlarge RX buffer before begin() for URC burst tolerance
    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    SerialAT.setTimeout(100); // RFC-0229: prevent 1-second stalls on empty-stream reads

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

#ifdef SIM_PIN
    // RFC-0083: Unlock SIM if a PIN is configured and the SIM is locked.
    // getSimStatus() returns 3 when the SIM is ready (no PIN required or
    // already unlocked). Values 1/2 indicate PIN/PUK needed.
    {
        int simStatus = modem.getSimStatus();
        Serial.print("SIM status: "); Serial.println(simStatus);
        if (simStatus != 3) {
            Serial.println("SIM locked, sending PIN...");
            if (modem.simUnlock(SIM_PIN)) {
                Serial.println("SIM PIN accepted.");
            } else {
                Serial.println("SIM PIN rejected! Check SIM_PIN in secrets.h.");
            }
        }
    }
#endif

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

    // RFC-0247: Enable spontaneous +CREG: <stat> URCs so the drain loop
    // can update cachedRegStatus and fire RFC-0082 alerts in real-time
    // (within one loop iteration, ~50 ms) rather than waiting up to 30 s
    // for the next status-refresh block.
    modem.sendAT("+CREG=1");
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
        // --- uptime (RFC-0102: measured from end of setup(), not ESP power-on) ---
        unsigned long uptimeSec = ((uint32_t)millis() - s_bootMs) / 1000UL;
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
        case REG_SMS_ONLY:     regStr = "sms-only";      break;
        case REG_UNKNOWN:      regStr = "unknown";        break;
        default:               regStr = "unknown";       break;
        }

        // --- assemble message ---
        String msg;

        // RFC-0068: Compact one-liner at the top (same format as the heartbeat)
        // so the key numbers are visible without scrolling.
        {
            msg += String("\xE2\x8F\xB1 "); // ⏱
            msg += String((int)days); msg += "d ";
            msg += String((int)hours); msg += "h ";
            msg += String((int)mins); msg += "m";
            msg += String(" | CSQ "); msg += String(cachedCsq);
            if (cachedOperatorName.length() > 0) { msg += " "; msg += cachedOperatorName; }
            msg += String(" | WiFi "); msg += String(WiFi.RSSI()); msg += "dBm";
            msg += String(" | fwd "); msg += String(smsHandler.smsForwarded());
            msg += String(" | q "); msg += String(smsSender.queueSize());
            msg += String("/"); msg += String(SmsSender::kQueueSize);
            msg += "\n\n";
        }

        msg += "\xF0\x9F\x93\xA1 Device\n"; // 📡
        msg += "  Time: ";      msg += timeBuf; msg += " "; msg += tzLabel; msg += "\n";
        // RFC-0115: Last NTP sync time.
        msg += "  Last NTP: ";
        if (s_lastNtpSyncTime == 0)
        {
            msg += "(never synced)";
        }
        else
        {
            time_t ntpLocal = s_lastNtpSyncTime + TIMEZONE_OFFSET_SEC;
            struct tm *nt = gmtime(&ntpLocal);
            char ntpBuf[32];
            strftime(ntpBuf, sizeof(ntpBuf), "%Y-%m-%d %H:%M", nt);
            msg += ntpBuf; msg += " "; msg += tzLabel;
        }
        msg += "\n";
        // RFC-0208: Last successful Telegram contact.
        msg += "  Last TG: ";
        if (s_lastTelegramOkTime == 0)
        {
            msg += "(never this boot)";
        }
        else
        {
            time_t tgLocal = s_lastTelegramOkTime + TIMEZONE_OFFSET_SEC;
            struct tm *tg = gmtime(&tgLocal);
            char tgBuf[32];
            strftime(tgBuf, sizeof(tgBuf), "%Y-%m-%d %H:%M", tg);
            msg += tgBuf; msg += " "; msg += tzLabel;
        }
        msg += "\n";
        msg += "  Uptime: ";    msg += String((int)days); msg += "d "; msg += String((int)hours); msg += "h "; msg += String((int)mins); msg += "m\n";
        // RFC-0065: Show SSID, RSSI, and IP.
        msg += "  WiFi: ";
        if (WiFi.status() == WL_CONNECTED) {
            msg += WiFi.SSID(); msg += " ("; msg += String(WiFi.RSSI()); msg += " dBm)  ";
            msg += WiFi.localIP().toString();
        } else {
            msg += "disconnected";
        }
        msg += "\n";
        msg += "  Modem: CSQ "; msg += String(cachedCsq); msg += " ("; msg += csqLabel; msg += ")  "; msg += regStr;
        if (cachedOperatorName.length() > 0) { msg += " ("; msg += cachedOperatorName; msg += ")"; }
        if (cachedModemFirmware.length() > 0) { msg += "\n  FW: "; msg += cachedModemFirmware; } // RFC-0045
        if (cachedImei.length() > 0) { msg += "\n  IMEI: "; msg += cachedImei; }                 // RFC-0076
        if (cachedIccid.length() > 0) { msg += "\n  ICCID: "; msg += cachedIccid; }              // RFC-0077
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
        // RFC-0059: Cumulative boot count.
        msg += "  Boots: "; msg += String((int)s_bootCount); msg += " (lifetime)\n";
        // RFC-0038: Show absolute boot timestamp when NTP was available.
        if (s_bootTimestamp > 0) {
            time_t bt = s_bootTimestamp + TIMEZONE_OFFSET_SEC;
            struct tm *bt2 = gmtime(&bt);
            char bootBuf[32];
            strftime(bootBuf, sizeof(bootBuf), "%Y-%m-%d %H:%M", bt2);
            msg += "  Booted: "; msg += bootBuf; msg += " "; msg += tzLabel; msg += "\n";
        }

        msg += "\n\xF0\x9F\x93\xA8 SMS\n"; // 📨
        msg += "  Forwarded: "; msg += String(smsHandler.smsForwarded()); msg += " (session), ";
        msg += String((int)s_lifetimeFwdCount); msg += " (lifetime)\n";
        // RFC-0062: blocked and dedup counters.
        if (smsHandler.smsBlocked() > 0)
            { msg += "  Blocked: "; msg += String(smsHandler.smsBlocked()); msg += "\n"; }
        if (smsHandler.smsDeduplicated() > 0)
            { msg += "  Deduped: "; msg += String(smsHandler.smsDeduplicated()); msg += "\n"; }
        // RFC-0041: Last SMS received timestamp.
        if (s_lastSmsTimestamp > 0) {
            time_t lt = s_lastSmsTimestamp + TIMEZONE_OFFSET_SEC;
            struct tm *lt2 = gmtime(&lt);
            char lastSmsBuf[32];
            strftime(lastSmsBuf, sizeof(lastSmsBuf), "%Y-%m-%d %H:%M", lt2);
            msg += "  Last rcvd: "; msg += lastSmsBuf; msg += " "; msg += tzLabel; msg += "\n";
        }
        msg += "  Failed: ";    msg += String(smsHandler.smsFailed()); msg += "\n";
        msg += "  Consec. failures: "; msg += String(smsHandler.consecutiveFailures()); msg += "\n";
        msg += "  Concat in-flight: "; msg += String((int)smsHandler.concatKeyCount()); msg += "\n";
        // RFC-0034: show outbound retry queue depth.
        msg += "  Outbound queue: "; msg += String(smsSender.queueSize()); msg += "/"; msg += String(SmsSender::kQueueSize); msg += "\n";
        msg += "  Outbound: "; msg += String(smsSender.sentCount()); msg += " sent, "; msg += String(smsSender.failedCount()); msg += " failed\n"; // RFC-0091
        // RFC-0206: Scheduled SMS count (show only when non-zero).
        if (telegramPoller) {
            int schedPending = 0;
            const auto& sq = telegramPoller->getSchedQueue();
            for (size_t si = 0; si < sq.size(); si++)
                if (sq[si].sendAtMs != 0) schedPending++;
            if (schedPending > 0) {
                msg += "  Sched: "; msg += String(schedPending);
                msg += "/"; msg += String((int)TelegramPoller::kScheduledQueueSize);
                msg += " pending\n";
            }
        }
        // RFC-0036: SIM slot usage (reflects SMS still on SIM — fragments + sweep backlog).
        if (cachedSimUsed >= 0) {
            msg += "  SIM: "; msg += String(cachedSimUsed); msg += "/"; msg += String(cachedSimTotal); msg += " slots\n";
        }

        // RFC-0043: Calls received since boot.
        msg += "  Calls rcvd: "; msg += String(callHandler.callsReceived()); msg += "\n";

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
        msg += "  Aliases: "; msg += String(smsAliasStore.count()); msg += "/"; msg += String(SmsAliasStore::kMaxAliases); msg += "\n"; // RFC-0090
        // RFC-0099: Show mute state when active.
        if (s_alertsMutedUntilMs > (uint32_t)millis()) {
            uint32_t remainSec = (s_alertsMutedUntilMs - (uint32_t)millis()) / 1000u;
            msg += "  Alerts: muted (";
            if (remainSec >= 60) { msg += String((int)(remainSec / 60)); msg += "m"; }
            else                  { msg += String((int)remainSec); msg += "s"; }
            msg += " remaining)\n";
        }
        // RFC-0048: Build timestamp (compile-time).
        msg += "  Build: "; msg += String(__DATE__); msg += " "; msg += String(__TIME__); msg += "\n";
        // RFC-0135: Show device note when set.
        if (s_deviceNote.length() > 0) {
            msg += "  Note: "; msg += s_deviceNote; msg += "\n";
        }
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
                // RFC-0084: Normalize number before storing (preserve trailing '*').
                String normedNumber;
                if (number.endsWith("*")) {
                    normedNumber = sms_codec::normalizePhoneNumber(number.substring(0, number.length() - 1)) + "*";
                } else {
                    normedNumber = sms_codec::normalizePhoneNumber(number);
                }
                if (isBlocked(normedNumber.c_str(), sBlockList, sBlockListCount))
                {
                    reason = normedNumber + " is already in the compile-time block list.";
                    return false;
                }
                if (isBlocked(normedNumber.c_str(), sRuntimeBlockList, sRuntimeBlockListCount))
                {
                    reason = normedNumber + " is already in the runtime block list.";
                    return false;
                }
                if (sRuntimeBlockListCount >= kSmsBlockListMaxEntries)
                {
                    reason = String("Runtime block list full (max ") +
                             String(kSmsBlockListMaxEntries) +
                             " entries). Use /unblock to remove one first.";
                    return false;
                }
                if ((int)normedNumber.length() > kSmsBlockListMaxNumberLen)
                {
                    reason = String("Number too long (max ") +
                             String(kSmsBlockListMaxNumberLen) + " characters).";
                    return false;
                }
                memcpy(sRuntimeBlockList[sRuntimeBlockListCount],
                       normedNumber.c_str(), normedNumber.length() + 1);
                sRuntimeBlockListCount++;
                smsHandler.setRuntimeBlockList(sRuntimeBlockList, sRuntimeBlockListCount);
                struct { int32_t count; char numbers[20][21]; } blob{};
                blob.count = sRuntimeBlockListCount;
                memcpy(blob.numbers, sRuntimeBlockList,
                       (size_t)sRuntimeBlockListCount * (kSmsBlockListMaxNumberLen + 1));
                realPersist.saveBlob("smsblist", &blob, sizeof(blob));
                Serial.printf("SMS block list: added %s (%d runtime entries)\n",
                              normedNumber.c_str(), sRuntimeBlockListCount);
                return true;
            }

            if (cmd == "unblock")
            {
                // RFC-0084: Normalize before lookup (preserve trailing '*').
                String normedUnblock;
                if (number.endsWith("*")) {
                    normedUnblock = sms_codec::normalizePhoneNumber(number.substring(0, number.length() - 1)) + "*";
                } else {
                    normedUnblock = sms_codec::normalizePhoneNumber(number);
                }
                int found = -1;
                for (int i = 0; i < sRuntimeBlockListCount; i++)
                    if (strcmp(sRuntimeBlockList[i], normedUnblock.c_str()) == 0) { found = i; break; }
                if (found < 0)
                {
                    if (isBlocked(normedUnblock.c_str(), sBlockList, sBlockListCount))
                        reason = normedUnblock + " is in the compile-time list and cannot be removed at runtime.";
                    else
                        reason = normedUnblock + " is not in the runtime block list.";
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
                              normedUnblock.c_str(), sRuntimeBlockListCount);
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
        // RFC-0118: Load device label from NVS.
        {
            char labelBuf[33] = {};
            size_t n = realPersist.loadBlob("device_label", labelBuf, 32);
            if (n > 0)
            {
                labelBuf[n] = '\0';
                s_deviceLabel = String(labelBuf);
                Serial.print("Device label loaded: ");
                Serial.println(s_deviceLabel);
            }
        }

        // RFC-0131: Load device note from NVS.
        {
            char noteBuf[121] = {};
            size_t n = realPersist.loadBlob("device_note", noteBuf, 120);
            if (n > 0)
            {
                noteBuf[n] = '\0';
                s_deviceNote = String(noteBuf);
            }
        }

        // RFC-0137: Load heartbeat interval override from NVS.
        {
            uint32_t hbInterval = 0;
            if (realPersist.loadBlob("hb_interval", &hbInterval, sizeof(hbInterval)) == sizeof(hbInterval))
                s_heartbeatIntervalSec = hbInterval;
        }

        // RFC-0182: Restore timezone offset and forward tag from NVS.
        {
            int32_t gmtOffMin = 480;
            if (realPersist.loadBlob("gmt_off_min", &gmtOffMin, sizeof(gmtOffMin)) == sizeof(gmtOffMin))
                smsHandler.setGmtOffsetMinutes(gmtOffMin);
        }
        {
            char fwdTagBuf[21] = {};
            if (realPersist.loadBlob("fwd_tag", fwdTagBuf, sizeof(fwdTagBuf)) > 0)
                smsHandler.setFwdTag(String(fwdTagBuf));
        }

        // RFC-0183: Restore forwarding and block-mode flags from NVS.
        {
            uint8_t v = 1;
            if (realPersist.loadBlob("fwd_enabled", &v, sizeof(v)) == sizeof(v))
                smsHandler.setForwardingEnabled(v != 0);
        }
        {
            uint8_t v = 1;
            if (realPersist.loadBlob("blk_enabled", &v, sizeof(v)) == sizeof(v))
                smsHandler.setBlockingEnabled(v != 0);
        }

        // RFC-0185: Restore call notify flag and poll interval from NVS.
        {
            uint8_t v = 1;
            if (realPersist.loadBlob("call_notify", &v, sizeof(v)) == sizeof(v))
                callHandler.setCallNotifyEnabled(v != 0);
        }
        {
            uint32_t v = 0;
            if (realPersist.loadBlob("poll_ms", &v, sizeof(v)) == sizeof(v)
                && v >= 1000 && v <= 30000)
                telegramPoller->setPollIntervalMs(v);
        }

        // RFC-0189: Restore maxParts, concatTtl, maxFail, dedupWindow from NVS.
        {
            int32_t v = 10;
            if (realPersist.loadBlob("max_parts", &v, sizeof(v)) == sizeof(v)
                && v >= 1 && v <= 10)
                smsSender.setMaxParts((int)v);
        }
        {
            uint32_t v = 0;
            if (realPersist.loadBlob("concat_ttl", &v, sizeof(v)) == sizeof(v) && v > 0)
                smsHandler.setConcatTtlMs((unsigned long)v * 1000UL);
        }
        {
            int32_t v = 8;
            if (realPersist.loadBlob("max_fail", &v, sizeof(v)) == sizeof(v)
                && v >= 0 && v <= 100)
                smsHandler.setMaxConsecutiveFailures((int)v);
        }
        {
            uint32_t v = 0;
            if (realPersist.loadBlob("dedup_secs", &v, sizeof(v)) == sizeof(v))
                smsHandler.setDedupWindowMs((unsigned long)v * 1000UL);
        }
        // RFC-0190: Restore SMS age filter from NVS.
        {
            int32_t v = 0;
            if (realPersist.loadBlob("sms_age_h", &v, sizeof(v)) == sizeof(v) && v >= 0)
                smsHandler.setMaxSmsAgeHours((int)v);
        }

        // RFC-0211: Load quiet hours from NVS.
        {
            uint8_t qh[2] = {0xFF, 0xFF};
            if (realPersist.loadBlob("quiet_hrs", qh, sizeof(qh)) == sizeof(qh)
                && qh[0] <= 23 && qh[1] <= 23 && qh[0] != qh[1])
            {
                telegramPoller->setQuietHours((int)qh[0], (int)qh[1]);
                Serial.printf("[RFC-0211] Quiet hours loaded: %02d:00-%02d:00 UTC\n",
                              (int)qh[0], (int)qh[1]);
            }
        }

        // RFC-0150: Load auto-reply text from NVS.
        {
            char buf[161] = {};
            size_t got = realPersist.loadBlob("autoreply", buf, sizeof(buf) - 1);
            if (got > 0) s_autoReplyText = String(buf);
        }

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

        // RFC-0044: Restore last-SMS timestamp from NVS.
        {
            uint32_t ts = 0;
            if (realPersist.loadBlob("lastsmsts", &ts, sizeof(ts)) == sizeof(ts) && ts > 0)
                s_lastSmsTimestamp = (time_t)ts;
        }

        // RFC-0060: Restore lifetime forward count from NVS.
        {
            uint32_t lf = 0;
            if (realPersist.loadBlob("lifetimefwd", &lf, sizeof(lf)) == sizeof(lf))
                s_lifetimeFwdCount = lf;
        }

        replyTargets.load();
        smsAliasStore.load();    // RFC-0088
        smsTemplateStore.load(); // RFC-0227
        smsHandler.setReplyTargetMap(&replyTargets);
        smsHandler.setDebugLog(&smsDebugLog);
        // RFC-0070: Forward SMS to all allow-list users; admin (index 0) already
        // receives via sendMessageReturningId, so only pass indices 1..n-1.
        if (allowedIdCount > 1)
            smsHandler.setExtraRecipients(allowedIds + 1, allowedIdCount - 1);
        smsHandler.setOnForwarded([]() {                                        // RFC-0041/0044/0060
            s_lastSmsTimestamp = time(nullptr);
            uint32_t ts = (uint32_t)s_lastSmsTimestamp;                         // RFC-0044: persist
            realPersist.saveBlob("lastsmsts", &ts, sizeof(ts));
            s_lifetimeFwdCount++;                                                // RFC-0060: persist
            realPersist.saveBlob("lifetimefwd", &s_lifetimeFwdCount, sizeof(s_lifetimeFwdCount));
        });
        telegramPoller->setForwardingEnabledFn([&smsHandler](bool enabled) { // RFC-0153/0183
            smsHandler.setForwardingEnabled(enabled);
            if (enabled) s_fwdPauseUntilMs = 0; // RFC-0192: manual /setforward on cancels pause
            uint8_t v = enabled ? 1 : 0;
            realPersist.saveBlob("fwd_enabled", &v, sizeof(v));
        });
        telegramPoller->setPauseFwdFn([&smsHandler](uint32_t durMs) -> String { // RFC-0192
            smsHandler.setForwardingEnabled(false);
            s_fwdPauseUntilMs = (uint32_t)millis() + durMs;
            uint32_t mins = (durMs + 59999U) / 60000U;
            return String("Forwarding paused for ") + String(mins) + String(" min.");
        });
        smsHandler.setOnSenderFn([&smsSender](const String &phone) { // RFC-0150
            if (s_autoReplyText.length() > 0)
                smsSender.enqueue(phone, s_autoReplyText);
        });
        telegramPoller->setAutoReplyGetFn([]() -> String { return s_autoReplyText; }); // RFC-0151
        telegramPoller->setAutoReplySetFn([](const String &text) { // RFC-0151
            s_autoReplyText = text;
            realPersist.saveBlob("autoreply", text.c_str(), text.length());
        });
        telegramPoller->setDebugLog(&smsDebugLog);
        telegramPoller->setNtpSyncFn([]() { syncTime(); }); // RFC-0055
        telegramPoller->setConcatSummaryFn([]() { return smsHandler.concatGroupsSummary(); }); // RFC-0069
        telegramPoller->setWifiReconnectFn([]() { s_pendingWifiReconnect = true; });            // RFC-0071
        telegramPoller->setVersionStr(String("Built: ") + __DATE__ + " " + __TIME__);            // RFC-0074
        telegramPoller->setHeapFn([]() -> String {                                              // RFC-0072
            String s;
            s += "Free: ";      s += String(ESP.getFreeHeap());    s += " B\n";
            s += "Min ever: ";  s += String(ESP.getMinFreeHeap()); s += " B\n";
            s += "Max block: "; s += String(ESP.getMaxAllocHeap()); s += " B";
            return s;
        });
        smsSender.setDebugLog(&smsDebugLog); // RFC-0035: log outbound failures
        // RFC-0100 / RFC-0108: Register call notification in ReplyTargetMap so the
        // user can reply to the call message to send an SMS back to the caller.
        callHandler.setOnCallFn([](const String &callerNumber, int32_t msgId) {
            // RFC-0108: Register (msgId, callerNumber) so a Telegram reply to
            // this notification routes back to the caller as an SMS.
            if (callerNumber.length() > 0 && msgId > 0)
                replyTargets.put(msgId, callerNumber);
#ifdef CALL_AUTO_REPLY_TEXT
            // RFC-0100: Auto-reply SMS when a call is auto-rejected.
            if (callerNumber.length() > 0) // can't SMS unknown number
                smsSender.enqueue(callerNumber, String(CALL_AUTO_REPLY_TEXT),
                    nullptr, nullptr);
#endif
        });
        telegramPoller->setAliasStore(&smsAliasStore);       // RFC-0088
        telegramPoller->setTemplateStore(&smsTemplateStore); // RFC-0227
        smsHandler.setAliasFn([](const String &phone) { // RFC-0176
            return smsAliasStore.lookupByPhone(phone);
        });
        // RFC-0219: Wire snooze filter — suppress forwarding snoozed numbers.
        smsHandler.setSenderFilterFn([&](const String &phone) -> bool {
            return telegramPoller->isSnoozed(phone);
        });
        telegramPoller->setMuteFn([](uint32_t minutes) { // RFC-0098
            s_alertsMutedUntilMs = (uint32_t)millis() + minutes * 60000UL;
        });
        telegramPoller->setUnmuteFn([]() {              // RFC-0098
            s_alertsMutedUntilMs = 0;
        });
        telegramPoller->setCsqFn([]() -> String {      // RFC-0092
            const char *csqLabel;
            if (cachedCsq == 99)      csqLabel = "none";
            else if (cachedCsq <= 9)  csqLabel = "marginal";
            else if (cachedCsq <= 14) csqLabel = "ok";
            else if (cachedCsq <= 19) csqLabel = "good";
            else                      csqLabel = "excellent";
            const char *regStr;
            switch (cachedRegStatus) {
                case REG_OK_HOME:      regStr = "home";         break;
                case REG_OK_ROAMING:   regStr = "roaming";      break;
                case REG_SEARCHING:    regStr = "searching";    break;
                case REG_DENIED:       regStr = "denied";       break;
                case REG_UNREGISTERED: regStr = "unregistered"; break;
                case REG_SMS_ONLY:     regStr = "sms-only";     break;
                case REG_UNKNOWN:      regStr = "unknown";       break;
                default:               regStr = "unknown";      break;
            }
            String s = String("\xF0\x9F\x93\xB6 CSQ ") + String(cachedCsq) // 📶
                + String(" (") + csqLabel + String(") | ") + regStr;
            if (cachedOperatorName.length() > 0)
                s += String(" (") + cachedOperatorName + String(")");
            s += String(" | WiFi ") + String(WiFi.RSSI()) + String(" dBm");
            return s;
        });
        telegramPoller->setUssdFn([](const String &code) -> String { // RFC-0103
            return realModem.ussdQuery(code, 10000UL);
        });
        telegramPoller->setBalanceCodeFn([]() -> String {            // RFC-0114
#ifdef USSD_BALANCE_CODE
            return String(USSD_BALANCE_CODE);
#else
            return String();
#endif
        });
        telegramPoller->setResetStatsFn([]() {                      // RFC-0110
            smsHandler.resetStats();
            callHandler.resetStats();
            smsSender.resetStats();
        });
        telegramPoller->setRebootFn([](int64_t /*fromId*/) {        // RFC-0112
            // All users in allow list may reboot; admin-only gating
            // can be added here if needed.
            s_pendingRestart = true;
        });
        telegramPoller->setClearNvsFn([]() { realPersist.clearAll(); }); // RFC-0184
        telegramPoller->setOnPollIntervalChangedFn([](uint32_t ms) { // RFC-0185
            realPersist.saveBlob("poll_ms", &ms, sizeof(ms));
        });
        telegramPoller->setLabelGetFn([]() -> String {              // RFC-0118
            return s_deviceLabel;
        });
        telegramPoller->setLabelSetFn([](const String &lbl) {       // RFC-0118
            s_deviceLabel = lbl;
            realPersist.saveBlob("device_label", lbl.c_str(), lbl.length());
        });
        telegramPoller->setPingSummaryFn([]() -> String {           // RFC-0119
            // "🏓 Pong [label] | ⏱ 2d 3h 15m | CSQ 18 (good)"
            String msg = String("\xF0\x9F\x8F\x93 Pong"); // 🏓
            if (s_deviceLabel.length() > 0) {
                msg += String(" ["); msg += s_deviceLabel; msg += String("]");
            }
            unsigned long uptimeSec = ((uint32_t)millis() - s_bootMs) / 1000UL;
            unsigned long days  = uptimeSec / 86400UL;
            unsigned long hours = (uptimeSec % 86400UL) / 3600UL;
            unsigned long mins  = (uptimeSec % 3600UL) / 60UL;
            msg += String(" | \xE2\x8F\xB1 "); // ⏱
            if (days > 0)  { msg += String(days);  msg += String("d "); }
            if (hours > 0) { msg += String(hours); msg += String("h "); }
            msg += String(mins); msg += String("m");
            const char *csqLabel;
            if (cachedCsq == 99)      csqLabel = "none";
            else if (cachedCsq <= 9)  csqLabel = "marginal";
            else if (cachedCsq <= 14) csqLabel = "ok";
            else if (cachedCsq <= 19) csqLabel = "good";
            else                      csqLabel = "excellent";
            msg += String(" | CSQ "); msg += String(cachedCsq);
            msg += String(" ("); msg += String(csqLabel); msg += String(")");
            return msg;
        });
        telegramPoller->setUptimeFn([]() -> String {                 // RFC-0120
            // "⏱ 2d 3h 15m 42s"
            unsigned long uptimeSec = ((uint32_t)millis() - s_bootMs) / 1000UL;
            unsigned long days  = uptimeSec / 86400UL;
            unsigned long hours = (uptimeSec % 86400UL) / 3600UL;
            unsigned long mins  = (uptimeSec % 3600UL) / 60UL;
            unsigned long secs  = uptimeSec % 60UL;
            String msg = String("\xE2\x8F\xB1 "); // ⏱
            if (days > 0)  { msg += String(days);  msg += String("d "); }
            if (hours > 0) { msg += String(hours); msg += String("h "); }
            if (mins > 0 || days > 0 || hours > 0) { msg += String(mins); msg += String("m "); }
            msg += String(secs); msg += String("s");
            return msg;
        });
        telegramPoller->setNetworkFn([]() -> String {                // RFC-0121
            // "📶 Operator: T-Mobile | Reg: home | CSQ 18 (good)"
            const char *regStr;
            switch (cachedRegStatus) {
                case REG_OK_HOME:      regStr = "home";         break;
                case REG_OK_ROAMING:   regStr = "roaming";      break;
                case REG_SEARCHING:    regStr = "searching";    break;
                case REG_DENIED:       regStr = "denied";       break;
                case REG_UNREGISTERED: regStr = "unregistered"; break;
                case REG_SMS_ONLY:     regStr = "sms-only";     break;
                case REG_UNKNOWN:      regStr = "unknown";       break;
                default:               regStr = "unknown";      break;
            }
            const char *csqLabel;
            if (cachedCsq == 99)      csqLabel = "none";
            else if (cachedCsq <= 9)  csqLabel = "marginal";
            else if (cachedCsq <= 14) csqLabel = "ok";
            else if (cachedCsq <= 19) csqLabel = "good";
            else                      csqLabel = "excellent";
            String msg = String("\xF0\x9F\x93\xB6 Operator: "); // 📶
            msg += (cachedOperatorName.length() > 0 ? cachedOperatorName : String("(unknown)"));
            msg += String(" | Reg: "); msg += String(regStr);
            msg += String(" | CSQ "); msg += String(cachedCsq);
            msg += String(" ("); msg += String(csqLabel); msg += String(")");
            return msg;
        });
        telegramPoller->setBootInfoFn([]() -> String {              // RFC-0123
            // "🔄 Boot #42 | Reason: Power-on | 2026-04-10 14:32 UTC"
            String msg = String("\xF0\x9F\x94\x84 Boot #"); // 🔄
            msg += String((int)s_bootCount);
            msg += String(" | Reason: ");
            msg += String(resetReasonStr(s_resetReason));
            if (s_bootTimestamp > 0) {
                time_t bt = s_bootTimestamp + TIMEZONE_OFFSET_SEC;
                struct tm *tm_info = gmtime(&bt);
                char buf[32];
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", tm_info);
                msg += String(" | "); msg += String(buf);
            }
            return msg;
        });
        telegramPoller->setCountFn([&smsHandler, &smsSender, &callHandler]() -> String { // RFC-0124
            // "📊 SMS rcvd: 12 | fwd: 11 | fail: 1 | Outbound: 5 sent, 0 fail | Calls: 3"
            String msg = String("\xF0\x9F\x93\x8A SMS rcvd: "); // 📊
            msg += String(smsHandler.smsForwarded() + smsHandler.smsFailed());
            msg += String(" | fwd: "); msg += String(smsHandler.smsForwarded());
            msg += String(" | fail: "); msg += String(smsHandler.smsFailed());
            msg += String(" | Outbound: "); msg += String(smsSender.sentCount());
            msg += String(" sent, "); msg += String(smsSender.failedCount()); msg += String(" fail");
            msg += String(" | Calls: "); msg += String(callHandler.callsReceived());
            return msg;
        });
        telegramPoller->setIpFn([]() -> String {                    // RFC-0126
            // "🌐 192.168.1.42 | SSID: MyWiFi | RSSI: -65 dBm"
            String msg = String("\xF0\x9F\x8C\x90 "); // 🌐
            if (WiFi.status() == WL_CONNECTED) {
                msg += WiFi.localIP().toString();
                msg += String(" | SSID: "); msg += String(WiFi.SSID());
                msg += String(" | RSSI: "); msg += String(WiFi.RSSI()); msg += String(" dBm");
            } else {
                msg += String("(not connected)");
            }
            return msg;
        });
        telegramPoller->setSmsSlotssFn([]() -> String {             // RFC-0127
            // "📨 SIM slots: 5/30 used"
            if (cachedSimUsed < 0)
                return String("\xF0\x9F\x93\xA8 SIM slots: (not yet queried)"); // 📨
            String msg = String("\xF0\x9F\x93\xA8 SIM slots: "); // 📨
            msg += String(cachedSimUsed); msg += String("/");
            msg += String(cachedSimTotal); msg += String(" used");
            if (cachedSimTotal > 0) {
                int pct = (cachedSimUsed * 100) / cachedSimTotal;
                msg += String(" ("); msg += String(pct); msg += String("%)");
            }
            return msg;
        });
        telegramPoller->setLifetimeFn([]() -> String {              // RFC-0128
            // "📈 Lifetime: 1234 SMS forwarded | Boot #42"
            String msg = String("\xF0\x9F\x93\x88 Lifetime: "); // 📈
            msg += String((int)s_lifetimeFwdCount); msg += String(" SMS forwarded");
            msg += String(" | Boot #"); msg += String((int)s_bootCount);
            return msg;
        });
        telegramPoller->setAnnounceFn([](const String &text) -> int { // RFC-0129
            int count = 0;
            for (int i = 0; i < allowedIdCount; i++) {
                if (realBot.sendMessageTo(allowedIds[i], text))
                    count++;
            }
            return count;
        });
        telegramPoller->setDigestFn([&smsHandler, &smsSender, &callHandler]() -> String { // RFC-0130
            // Same format as the daily digest.
            String digest;
            digest += "\xF0\x9F\x93\x8A On-demand digest | "; // 📊
            digest += "fwd "; digest += String(smsHandler.smsForwarded());
            digest += " (session) "; digest += String(s_lifetimeFwdCount); digest += " (lifetime)";
            if (smsHandler.smsBlocked() > 0) {
                digest += " | blocked "; digest += String(smsHandler.smsBlocked());
            }
            if (smsHandler.smsDeduplicated() > 0) {
                digest += " | deduped "; digest += String(smsHandler.smsDeduplicated());
            }
            digest += " | out "; digest += String(smsSender.sentCount());
            digest += " | calls "; digest += String(callHandler.callsReceived());
            unsigned long uptimeSec = ((uint32_t)millis() - s_bootMs) / 1000UL;
            digest += " | up "; digest += String((int)(uptimeSec / 3600)); digest += "h";
            return digest;
        });
        telegramPoller->setNoteGetFn([]() -> String {               // RFC-0131
            return s_deviceNote;
        });
        telegramPoller->setNoteSetFn([](const String &note) {       // RFC-0131
            s_deviceNote = note;
            realPersist.saveBlob("device_note", note.c_str(), note.length());
        });
        telegramPoller->setIntervalFn([](uint32_t secs) {           // RFC-0137
            s_heartbeatIntervalSec = secs;
            realPersist.saveBlob("hb_interval", &secs, sizeof(secs));
        });
        telegramPoller->setHeartbeatNowFn([]() -> bool { // RFC-0177
            if (s_heartbeatIntervalSec == 0) return false;
            // Back-date the timer so the next loop() tick fires the heartbeat.
            lastHeartbeatMs = (uint32_t)millis() - s_heartbeatIntervalSec * 1000UL - 1;
            return true;
        });
        telegramPoller->setSweepFn([&smsHandler]() -> int { // RFC-0148
            return smsHandler.sweepExistingSms();
        });
        telegramPoller->setHealthFn([]() -> String { // RFC-0149
            // "✅ OK | WiFi: -65dBm | CSQ: 18 | Heap: 48KB | Up: 2d3h"
            bool wifiOk = (WiFi.status() == WL_CONNECTED);
            String msg = wifiOk
                ? String("\xe2\x9c\x85 OK") // ✅
                : String("\xe2\x9a\xa0\xef\xb8\x8f WiFi down"); // ⚠️
            if (wifiOk) {
                msg += String(" | WiFi: "); msg += String(WiFi.RSSI()); msg += String(" dBm");
            }
            msg += String(" | CSQ: "); msg += String(cachedCsq);
            msg += String(" | Heap: "); msg += String(ESP.getFreeHeap() / 1024); msg += String("KB");
            if (s_heartbeatIntervalSec > 0) {
                unsigned long upSec = millis() / 1000;
                unsigned long d = upSec / 86400; upSec %= 86400;
                unsigned long h = upSec / 3600;  upSec %= 3600;
                unsigned long m = upSec / 60;
                msg += String(" | Up: ");
                if (d > 0) { msg += String((int)d); msg += String("d"); }
                if (h > 0 || d > 0) { msg += String((int)h); msg += String("h"); }
                msg += String((int)m); msg += String("m");
            }
            return msg;
        });
        telegramPoller->setSmsForwardFn([&smsHandler](int idx) -> bool { // RFC-0146
            smsHandler.handleSmsIndex(idx);
            return true; // handleSmsIndex is void; assume success
        });
        telegramPoller->setDedupWindowFn([&smsHandler](uint32_t secs) { // RFC-0144/0189
            smsHandler.setDedupWindowMs((unsigned long)secs * 1000UL);
            realPersist.saveBlob("dedup_secs", &secs, sizeof(secs));
        });
        telegramPoller->setClearDedupFn([&smsHandler]() { // RFC-0145
            smsHandler.clearDedupBuffer();
        });
        telegramPoller->setConcatTtlFn([&smsHandler](uint32_t secs) { // RFC-0142/0189
            smsHandler.setConcatTtlMs((unsigned long)secs * 1000UL);
            realPersist.saveBlob("concat_ttl", &secs, sizeof(secs));
        });
        telegramPoller->setModemInfoFn([]() -> String { // RFC-0143
            String info = String("\xF0\x9F\x93\xB1 Modem Info\n"); // 📱
            info += String("IMEI: ") + String(modem.getIMEI()) + String("\n");
            info += String("Model: ") + String(modem.getModemName()) + String("\n");
            info += String("FW: ") + String(modem.getModemInfo());
            return info;
        });
        telegramPoller->setMaxFailFn([&smsHandler](int n) { // RFC-0138/0189
            smsHandler.setMaxConsecutiveFailures(n);
            int32_t v = n;
            realPersist.saveBlob("max_fail", &v, sizeof(v));
        });
        telegramPoller->setMaxPartsFn([&smsSender](int n) { // RFC-0160/0189
            smsSender.setMaxParts(n);
            int32_t v = n;
            realPersist.saveBlob("max_parts", &v, sizeof(v));
        });
        telegramPoller->setSmsAgeFilterFn([&smsHandler](int h) { // RFC-0190
            smsHandler.setMaxSmsAgeHours(h);
            int32_t v = h;
            realPersist.saveBlob("sms_age_h", &v, sizeof(v));
        });
        telegramPoller->setTestPduFn([&smsHandler](const String &hexArg) -> String { // RFC-0191
            // Strip any whitespace from the input.
            String hex;
            for (unsigned i = 0; i < hexArg.length(); i++)
            {
                char c = hexArg[i];
                if (c != ' ' && c != '\t' && c != '\r' && c != '\n') hex += c;
            }
            sms_codec::SmsPdu pdu;
            if (!sms_codec::parseSmsPdu(hex, pdu))
                return String("\xe2\x9d\x8c Failed to parse PDU."); // ❌
            String out = String("\xF0\x9F\x93\xA8 PDU decoded:\n"); // 📨
            out += String("Sender: ") + sms_codec::humanReadablePhoneNumber(pdu.sender) + String("\n");
            if (pdu.timestamp.length() > 0)
                out += String("Time: ")
                       + sms_codec::timestampToRFC3339(pdu.timestamp, smsHandler.gmtOffsetMinutes())
                       + String("\n");
            if (pdu.isConcatenated)
                out += String("Concat: ref=") + String((int)pdu.concatRefNumber)
                       + String(" part=") + String((int)pdu.concatPartNumber)
                       + String("/") + String((int)pdu.concatTotalParts) + String("\n");
            out += String("Body (") + String(pdu.content.length()) + String(" chars): ");
            // Truncate very long bodies for readability.
            if (pdu.content.length() <= 300)
                out += pdu.content;
            else
                out += pdu.content.substring(0, 297) + String("...");
            return out;
        });
        telegramPoller->setBlockingEnabledFn([&smsHandler](bool enable) { // RFC-0162/0183
            smsHandler.setBlockingEnabled(enable);
            uint8_t v = enable ? 1 : 0;
            realPersist.saveBlob("blk_enabled", &v, sizeof(v));
        });
        telegramPoller->setCallNotifyFn([&callHandler](bool enable) { // RFC-0164/0185
            callHandler.setCallNotifyEnabled(enable);
            uint8_t v = enable ? 1 : 0;
            realPersist.saveBlob("call_notify", &v, sizeof(v));
        });
        telegramPoller->setCallDedupFn([&callHandler](uint32_t ms) { // RFC-0165
            callHandler.setDedupeWindowMs(ms);
        });
        telegramPoller->setCallUnknownDeadlineFn([&callHandler](uint32_t ms) { // RFC-0166
            callHandler.setUnknownDeadlineMs(ms);
        });
        telegramPoller->setSettingsFn([&smsHandler, &smsSender, &callHandler]() -> String { // RFC-0167,0169,0172
            String s;
            s += "\xe2\x9a\x99\xef\xb8\x8f Runtime settings:\n"; // ⚙️
            s += "  SMS forwarding: ";
            s += (smsHandler.forwardingEnabled() ? "ON" : "OFF"); s += "\n";
            s += "  SMS block enforcement: ";
            s += (smsHandler.blockingEnabled() ? "ON" : "OFF"); s += "\n";
            s += "  Max SMS parts: ";
            s += String(smsSender.maxParts()); s += "\n";
            s += "  Call notify: ";
            s += (callHandler.callNotifyEnabled() ? "ON" : "OFF"); s += "\n";
            s += "  Call dedup window: ";
            s += String(callHandler.dedupeWindowMs() / 1000); s += "s\n";
            s += "  Unknown-number deadline: ";
            s += String(callHandler.unknownDeadlineMs()); s += "ms\n";
            s += "  Heartbeat interval: ";
            s += String(s_heartbeatIntervalSec);
            s += (s_heartbeatIntervalSec == 0 ? "s (disabled)" : "s"); s += "\n";
            s += "  Poll interval: ";
            s += String(telegramPoller->pollIntervalMs() / 1000); s += "s\n";
            s += "  SMS timestamp GMT offset: UTC";
            int gmtM = smsHandler.gmtOffsetMinutes();
            { int absM = gmtM < 0 ? -gmtM : gmtM;
              int h = absM / 60, m = absM % 60;
              if (gmtM >= 0) s += "+";
              s += String(h);
              if (m) { s += ":"; if (m < 10) s += "0"; s += String(m); }
            }
            s += "\n";
            s += "  Forward tag: ";
            s += (smsHandler.fwdTag().length() > 0 ? ("\"" + smsHandler.fwdTag() + "\"") : "(none)");
            s += "\n";
            s += "  SMS age filter: "; // RFC-0190
            int ageh = smsHandler.maxSmsAgeHours();
            s += (ageh > 0 ? (String(ageh) + "h") : "disabled");
            s += "\n";
            s += "  Fwd pause: "; // RFC-0192/0194
            {
                uint32_t nowMs4 = (uint32_t)millis();
                if (s_fwdPauseUntilMs != 0 && s_fwdPauseUntilMs > nowMs4)
                {
                    uint32_t remMin = (s_fwdPauseUntilMs - nowMs4 + 59999U) / 60000U;
                    s += String("active (~") + String(remMin) + String(" min remain)");
                }
                else
                {
                    s += "none";
                }
            }
            return s;
        });
        telegramPoller->setGmtOffsetFn([&smsHandler](int m) { // RFC-0169/0175/0182
            smsHandler.setGmtOffsetMinutes(m);
            int32_t v = m;
            realPersist.saveBlob("gmt_off_min", &v, sizeof(v));
        });
        telegramPoller->setFwdTagFn([&smsHandler](const String &tag) { // RFC-0172/0182
            smsHandler.setFwdTag(tag);
            char buf[21] = {};
            tag.toCharArray(buf, sizeof(buf));
            realPersist.saveBlob("fwd_tag", buf, sizeof(buf));
        });
        telegramPoller->setFwdTestFn([&smsHandler]() -> String { // RFC-0181
            // Build a PDU-style timestamp "YY/MM/DD,HH:MM:SS+32" from now.
            time_t now = time(nullptr);
            struct tm *t = gmtime(&now);
            char tsBuf[22];
            snprintf(tsBuf, sizeof(tsBuf), "%02d/%02d/%02d,%02d:%02d:%02d+32",
                     t->tm_year % 100, t->tm_mon + 1, t->tm_mday,
                     t->tm_hour, t->tm_min, t->tm_sec);
            return smsHandler.previewFormat(
                String("+10000000000"),
                String(tsBuf),
                String("This is a test message. / \xe8\xbf\x99\xe6\x98\xaf\xe6\xb5\x8b\xe8\xaf\x95\xe6\xb6\x88\xe6\x81\xaf\xe3\x80\x82")
            );
        });
        telegramPoller->setFwdTestPhoneBodyFn([&smsHandler](const String &phone, const String &body) -> String { // RFC-0187
            time_t now = time(nullptr);
            struct tm *t = gmtime(&now);
            char tsBuf[22];
            snprintf(tsBuf, sizeof(tsBuf), "%02d/%02d/%02d,%02d:%02d:%02d+32",
                     t->tm_year % 100, t->tm_mon + 1, t->tm_mday,
                     t->tm_hour, t->tm_min, t->tm_sec);
            return smsHandler.previewFormat(phone, String(tsBuf), body);
        });
        telegramPoller->setSmsHandlerInfoFn([&smsHandler, &smsSender]() -> String { // RFC-0174
            String s;
            s += "\xF0\x9F\x93\xB1 SMS handler info:\n"; // 📱
            s += "  Forwarding: ";
            s += (smsHandler.forwardingEnabled() ? "ON" : "OFF"); s += "\n";
            s += "  Block enforcement: ";
            s += (smsHandler.blockingEnabled() ? "ON" : "OFF"); s += "\n";
            s += "  Max fail count: ";
            s += String(smsHandler.maxConsecutiveFailures()); s += "\n";
            s += "  Concat TTL: ";
            s += String(smsHandler.concatTtlMs() / 1000); s += "s\n";
            s += "  Dedup window: ";
            s += String(smsHandler.dedupWindowMs() / 1000); s += "s\n";
            s += "  Max SMS parts: ";
            s += String(smsSender.maxParts()); s += "\n";
            s += "  GMT offset: UTC";
            int gmtM = smsHandler.gmtOffsetMinutes();
            { int absM = gmtM < 0 ? -gmtM : gmtM;
              int h = absM / 60, m = absM % 60;
              if (gmtM >= 0) s += "+";
              s += String(h);
              if (m) { s += ":"; if (m < 10) s += "0"; s += String(m); }
            }
            s += "\n";
            s += "  Fwd tag: ";
            s += (smsHandler.fwdTag().length() > 0 ? ("\"" + smsHandler.fwdTag() + "\"") : "(none)"); s += "\n";
            s += "  Age filter: "; // RFC-0190
            { int a = smsHandler.maxSmsAgeHours(); s += (a > 0 ? (String(a) + "h") : "off"); s += "\n"; }
            s += "  Session: fwd=";
            s += String(smsHandler.smsForwarded());
            s += " blk="; s += String(smsHandler.smsBlocked());
            s += " dup="; s += String(smsHandler.smsDeduplicated());
            return s;
        });
        telegramPoller->setCallStatusFn([&callHandler]() -> String { // RFC-0173
            String s;
            s += "\xF0\x9F\x93\x9E Call handler status:\n"; // 📞
            s += "  State: ";
            switch (callHandler.state())
            {
                case CallHandler::State::Idle:     s += "Idle\n"; break;
                case CallHandler::State::Ringing:  s += "Ringing\n"; break;
                case CallHandler::State::Cooldown: s += "Cooldown\n"; break;
                default:                           s += "Unknown\n"; break;
            }
            s += "  Notify: ";
            s += (callHandler.callNotifyEnabled() ? "ON" : "OFF"); s += "\n";
            s += "  Dedup window: ";
            s += String(callHandler.dedupeWindowMs() / 1000); s += "s\n";
            s += "  Unknown deadline: ";
            s += String(callHandler.unknownDeadlineMs()); s += "ms\n";
            s += "  Calls received: ";
            s += String(callHandler.callsReceived()); s += "\n";
            s += "  Last caller: ";
            s += (callHandler.lastCallerNumber().length() > 0
                  ? callHandler.lastCallerNumber()
                  : String("(none yet)"));
            if (callHandler.lastCallTimeMs() > 0)
            {
                uint32_t agoMs = (uint32_t)millis() - callHandler.lastCallTimeMs();
                uint32_t agoSec = agoMs / 1000;
                s += " (";
                if (agoSec < 60)       { s += String(agoSec); s += "s ago)"; }
                else if (agoSec < 3600){ s += String(agoSec/60); s += "m ago)"; }
                else                   { s += String(agoSec/3600); s += "h ago)"; }
            }
            return s;
        });
        telegramPoller->setNvsInfoFn([]() -> String { // RFC-0168
            nvs_stats_t stats;
            esp_err_t err = nvs_get_stats(NULL, &stats);
            if (err != ESP_OK)
            {
                return String("NVS stats unavailable (err=") + String(err) + String(")");
            }
            String s;
            s += "\xF0\x9F\x97\x84 NVS storage:\n"; // 🗄
            s += "  Used entries:  "; s += String(stats.used_entries); s += "\n";
            s += "  Free entries:  "; s += String(stats.free_entries); s += "\n";
            s += "  Total entries: "; s += String(stats.total_entries); s += "\n";
            if (stats.total_entries > 0)
            {
                int pct = (int)((stats.used_entries * 100u) / stats.total_entries);
                s += "  Usage: "; s += String(pct); s += "%";
            }
            return s;
        });
        telegramPoller->setBlockCheckFn([&smsHandler](const String &phone) -> String { // RFC-0163
            bool enfEnabled = smsHandler.blockingEnabled();
            bool hitCompile = (sBlockListCount > 0 &&
                               isBlocked(phone.c_str(), sBlockList, sBlockListCount));
            bool hitRuntime = (sRuntimeBlockListCount > 0 &&
                               isBlocked(phone.c_str(), sRuntimeBlockList, sRuntimeBlockListCount));
            if (!enfEnabled)
            {
                String r = String("\xe2\x9a\xa0\xef\xb8\x8f Enforcement SUSPENDED. "); // ⚠️
                if (hitCompile || hitRuntime)
                    r += String("Would be blocked (") + (hitCompile ? "compile" : "runtime") + String("-list) if enabled.");
                else
                    r += String("Not in any block list.");
                return r;
            }
            if (hitCompile)
                return String("\xF0\x9F\x9A\xAB BLOCKED by compile-time list."); // 🚫
            if (hitRuntime)
                return String("\xF0\x9F\x9A\xAB BLOCKED by runtime list."); // 🚫
            return String("\xe2\x9c\x85 NOT blocked — would be forwarded."); // ✅
        });
        telegramPoller->setSmsCntFn([]() -> String { // RFC-0161
            String raw;
            realModem.sendAT(String("+CPMS?"));
            realModem.waitResponse(3000UL, raw);
            // +CPMS: "SM",<used>,<total>,"SM",<used2>,<total2>,...
            int pos = raw.indexOf("+CPMS:");
            if (pos < 0)
                return String("(CPMS query failed)");
            String line = raw.substring(pos + 7);
            line.trim();
            // Parse up to 3 store triplets: "name",used,total
            String out = String("\xF0\x9F\x93\xB1 SMS storage:\n"); // 📱
            int p = 0;
            for (int store = 0; store < 3 && p < (int)line.length(); store++)
            {
                int qo = line.indexOf('"', p);
                if (qo < 0) break;
                int qc = line.indexOf('"', qo + 1);
                if (qc < 0) break;
                String name = line.substring(qo + 1, qc);
                p = qc + 1;
                // skip comma
                if (p < (int)line.length() && line[p] == ',') ++p;
                int c1 = line.indexOf(',', p);
                if (c1 < 0) break;
                String usedStr = line.substring(p, c1);
                usedStr.trim();
                p = c1 + 1;
                int c2 = line.indexOf(',', p);
                String totalStr = c2 > 0 ? line.substring(p, c2) : line.substring(p);
                // totalStr may end at newline / CR
                {
                    int nl = totalStr.indexOf('\r'); if (nl >= 0) totalStr = totalStr.substring(0, nl);
                    nl = totalStr.indexOf('\n'); if (nl >= 0) totalStr = totalStr.substring(0, nl);
                }
                totalStr.trim();
                out += String("  ") + name + String(": ") + usedStr + String("/") + totalStr + String("\n");
                p = c2 > 0 ? c2 + 1 : (int)line.length();
            }
            return out;
        });
        telegramPoller->setFlushSimFn([]() -> int { // RFC-0139
            // AT+CMGDA="DEL ALL" — A76xx/SIM7xxx extension to delete all SMS.
            realModem.sendAT(String("+CMGDA=\"DEL ALL\""));
            bool ok = realModem.waitResponseOk(10000);
            if (!ok) {
                Serial.println("[flushsim] CMGDA failed, trying CMGDA=6");
                realModem.sendAT(String("+CMGDA=6")); // numeric variant
                realModem.waitResponseOk(10000);
            }
            return -1; // count unknown
        });
        telegramPoller->setSimListFn([]() -> String { // RFC-0140
            // Issue AT+CMGL="ALL" (PDU mode) and parse index lines.
            String data;
            realModem.sendAT(String("+CMGL=\"ALL\""));
            realModem.waitResponse(10000UL, data);
            String out;
            int count = 0;
            int search = 0;
            while (true) {
                int start = data.indexOf("+CMGL:", search);
                if (start == -1) break;
                int eol = data.indexOf('\n', start);
                String line = eol > 0 ? data.substring(start, eol) : data.substring(start);
                // +CMGL: <idx>,<stat>,,[<len>]
                int c1 = line.indexOf(',');
                String idxStr = c1 > 6 ? line.substring(7, c1) : String("?");
                idxStr.trim();
                out += String("  [") + idxStr + String("]\n");
                count++;
                search = (eol > 0) ? eol + 1 : data.length();
            }
            if (count == 0) return String("(no SMS in SIM)");
            return String("\xF0\x9F\x93\x8B SIM: ") + String(count) + String(" stored\n") + out; // 📋
        });
        telegramPoller->setSimReadFn([](int idx) -> String { // RFC-0141
            String raw;
            realModem.sendAT(String("+CMGR=") + String(idx));
            realModem.waitResponse(5000UL, raw);
            // Extract PDU hex from "+CMGR: ...\r\n<hex>\r\nOK" response.
            int header = raw.indexOf("+CMGR:");
            if (header < 0)
                return String("(slot ") + String(idx) + String(" empty or error)");
            int headerEnd = raw.indexOf("\r\n", header);
            if (headerEnd < 0) headerEnd = raw.length();
            int hexStart = headerEnd + 2;
            int hexEnd = raw.indexOf("\r\n", hexStart);
            String pduHex = raw.substring(hexStart, hexEnd < 0 ? raw.length() : hexEnd);
            pduHex.trim();
            if (pduHex.isEmpty())
                return String("(slot ") + String(idx) + String(": no PDU data)");
            // Strip SMSC prefix (first octet = SMSC info length in bytes).
            int smscLen = 0;
            if (pduHex.length() >= 2)
                smscLen = (int)strtol(pduHex.substring(0, 2).c_str(), nullptr, 16);
            String tpdu = pduHex.substring(2 + smscLen * 2);
            sms_codec::SmsPdu pdu;
            if (!sms_codec::parseSmsPdu(tpdu, pdu))
                return String("(idx ") + String(idx) + String(": PDU parse failed)");
            String result = String("\xF0\x9F\x93\xA8 [") + String(idx) + String("]\n"); // 📨
            result += String("From: ") + pdu.sender + String("\n");
            result += String("Time: ") + pdu.timestamp + String("\n");
            result += String("Body: ") + pdu.content;
            return result;
        });
        telegramPoller->setSimStatusFn([]() -> String { // RFC-0156
            // Live AT queries for network/SIM registration state.
            String out = String("\xF0\x9F\x93\xB6 Live SIM status\n"); // 📶
            // Registration state: AT+CREG?
            String creg;
            realModem.sendAT(String("+CREG?"));
            realModem.waitResponse(3000UL, creg);
            {
                // +CREG: <n>,<stat>[,<lac>,<ci>]
                int pos = creg.indexOf("+CREG:");
                if (pos >= 0) {
                    int comma = creg.indexOf(',', pos + 7);
                    if (comma > 0) {
                        int stat = creg.substring(comma + 1, comma + 2).toInt();
                        const char *regDesc = "unknown";
                        switch (stat) {
                            case 0: regDesc = "not registered"; break;
                            case 1: regDesc = "registered (home)"; break;
                            case 2: regDesc = "searching"; break;
                            case 3: regDesc = "denied"; break;
                            case 4: regDesc = "unknown (not determined)"; break; // RFC-0251
                            case 5: regDesc = "registered (roaming)"; break;
                            case 6: regDesc = "sms-only service"; break; // RFC-0251
                        }
                        out += String("  Reg: "); out += regDesc; out += String("\n");
                    }
                }
            }
            // Live signal: AT+CSQ
            String csqRaw;
            realModem.sendAT(String("+CSQ"));
            realModem.waitResponse(2000UL, csqRaw);
            {
                int pos = csqRaw.indexOf("+CSQ:");
                if (pos >= 0) {
                    int comma = csqRaw.indexOf(',', pos + 5);
                    String rssi = comma > 0 ? csqRaw.substring(pos + 6, comma) : csqRaw.substring(pos + 6);
                    rssi.trim();
                    out += String("  CSQ: "); out += rssi; out += String("\n");
                }
            }
            // Operator: AT+COPS?
            String cops;
            realModem.sendAT(String("+COPS?"));
            realModem.waitResponse(5000UL, cops);
            {
                int pos = cops.indexOf("+COPS:");
                if (pos >= 0) {
                    // +COPS: <mode>,<format>,<oper>[,<Act>]
                    // operator string is between second and third commas, quoted
                    int c2 = cops.indexOf(',', pos + 6);
                    if (c2 > 0) {
                        int qOpen = cops.indexOf('"', c2);
                        int qClose = qOpen >= 0 ? cops.indexOf('"', qOpen + 1) : -1;
                        if (qOpen >= 0 && qClose > qOpen) {
                            out += String("  Operator: ");
                            out += cops.substring(qOpen + 1, qClose);
                            out += String("\n");
                        }
                    }
                }
            }
            // ICCID (live)
            String iccidRaw;
            realModem.sendAT(String("+CCID"));
            realModem.waitResponse(3000UL, iccidRaw);
            {
                int pos = iccidRaw.indexOf("+CCID:");
                if (pos >= 0) {
                    String icc = iccidRaw.substring(pos + 7);
                    icc.trim();
                    // remove trailing OK or extra data
                    int nl = icc.indexOf('\r'); if (nl > 0) icc = icc.substring(0, nl);
                    nl = icc.indexOf('\n'); if (nl > 0) icc = icc.substring(0, nl);
                    icc.trim();
                    out += String("  ICCID: "); out += icc;
                }
            }
            return out;
        });
        telegramPoller->setWifiScanFn([]() -> String { // RFC-0158
            int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
            if (n <= 0)
                return String("(no networks found)");
            // Cap at 10 results; WiFi.scanNetworks returns sorted by RSSI desc.
            if (n > 10) n = 10;
            String out = String("\xF0\x9F\x93\xA1 WiFi scan ("); // 📡
            out += String(n); out += String(" networks)\n");
            for (int i = 0; i < n; ++i)
            {
                out += String("  ");
                String ssid = WiFi.SSID(i);
                if (ssid.length() == 0) ssid = String("(hidden)");
                out += ssid;
                out += String(" ch"); out += String(WiFi.channel(i));
                out += String(" "); out += String(WiFi.RSSI(i)); out += String("dBm");
                if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN)
                    out += String(" [open]");
                out += String("\n");
            }
            WiFi.scanDelete();
            return out;
        });
        telegramPoller->setAtCmdFn([](int64_t fromId, const String &cmd) -> String { // RFC-0107
            // Admin-only: first user in TELEGRAM_CHAT_IDS.
            if (allowedIdCount == 0 || fromId != allowedIds[0])
                return String("\xE2\x9D\x8C Admin access required."); // ❌
            String out;
            realModem.sendAT(cmd);
            realModem.waitResponse(5000UL, out);
            out.trim();
            if (out.length() == 0) out = String("(no response)");
            return out;
        });
        telegramPoller->setSimInfoFn([]() -> String {               // RFC-0105
            const char *csqLabel;
            if (cachedCsq == 99)      csqLabel = "none";
            else if (cachedCsq <= 9)  csqLabel = "marginal";
            else if (cachedCsq <= 14) csqLabel = "ok";
            else if (cachedCsq <= 19) csqLabel = "good";
            else                      csqLabel = "excellent";
            String s = String("\xF0\x9F\x93\xB6 SIM info\n"); // 📶
            if (cachedIccid.length() > 0)        { s += "  ICCID: ";    s += cachedIccid;        s += "\n"; }
            if (cachedImei.length() > 0)          { s += "  IMEI: ";     s += cachedImei;          s += "\n"; }
            if (cachedOperatorName.length() > 0)  { s += "  Operator: "; s += cachedOperatorName; s += "\n"; }
            s += "  CSQ: "; s += String(cachedCsq); s += " ("; s += csqLabel; s += ")";
            return s;
        });
        telegramPoller->setWallTimeFn([]() -> long { return (long)time(nullptr); }); // RFC-0202
        telegramPoller->setOnPollSuccessFn([]() { // RFC-0208 / RFC-0238
            s_lastTelegramOkTime = time(nullptr);
            // RFC-0238: If the boot sweep was deferred (Telegram was down at boot),
            // run it now on the first successful poll — Telegram is confirmed reachable.
            if (s_needBootSweep)
            {
                s_needBootSweep = false;
                Serial.println("[RFC-0238] Telegram reachable — running deferred boot sweep.");
                esp_task_wdt_reset();
                int swept = smsHandler.sweepExistingSms();
                esp_task_wdt_reset();
                if (swept > 0)
                {
                    String msg = String("\xF0\x9F\x93\xA8 Deferred sweep: ") + String(swept) // 📨
                               + String(" offline SMS drained.");
                    realBot.sendMessage(msg);
                }
            }
        }); // RFC-0208 / RFC-0238
        // RFC-0211: Persist quiet hours to NVS on change.
        telegramPoller->setPersistQuietFn([&]() {
            uint8_t qh[2] = { (uint8_t)telegramPoller->quietStart(),
                               (uint8_t)telegramPoller->quietEnd() };
            realPersist.saveBlob("quiet_hrs", qh, sizeof(qh));
        });
        // RFC-0209: /pending — terse snapshot of all pending work.
        telegramPoller->setPendingFn([&]() -> String {
            int q = smsSender.queueSize();
            int s = 0;
            const auto& sq = telegramPoller->getSchedQueue();
            for (const auto& slot : sq)
                if (slot.sendAtMs != 0) s++;
            int c = (int)smsHandler.concatKeyCount();
            if (q == 0 && s == 0 && c == 0)
                return String("\xf0\x9f\x93\x8b All clear (nothing pending)"); // 📋
            String out = String("\xf0\x9f\x93\x8b Queue: ") // 📋
                       + String(q) + String("/") + String(SmsSender::kQueueSize)
                       + String(" | Sched: ") + String(s) + String("/")
                       + String((int)TelegramPoller::kScheduledQueueSize)
                       + String(" | Concat: ") + String(c);
            return out;
        });
        // RFC-0200/RFC-0221: Serialize scheduled SMS queue to NVS after any mutation.
        // Blob layout v0x02: 1 version byte + 5 × 168-byte slots.
        // Slot: uint32_t sendAtUnix (4), char phone[32], char body[128],
        //       uint32_t repeatIntervalSec (4) [RFC-0221].
        telegramPoller->setPersistSchedFn([&]() {
            static const size_t kSlotSize = sizeof(uint32_t) + 32 + 128 + sizeof(uint32_t); // 168
            static const size_t kNumSlots = 5;
            uint8_t blob[1 + kNumSlots * kSlotSize] = {};
            blob[0] = 0x02; // version byte (bumped for RFC-0221)
            const auto& q = telegramPoller->getSchedQueue();
            long nowSec = (long)time(nullptr);
            long nowMs  = (long)(uint32_t)millis();
            for (size_t i = 0; i < q.size() && i < kNumSlots; i++)
            {
                uint8_t *slotPtr = blob + 1 + i * kSlotSize;
                if (q[i].sendAtMs == 0) continue; // free slot → sendAtUnix stays 0
                long deltaMs = (long)q[i].sendAtMs - nowMs;
                uint32_t sendAtUnix = (uint32_t)(nowSec + deltaMs / 1000L);
                memcpy(slotPtr, &sendAtUnix, 4);
                q[i].phone.toCharArray((char*)(slotPtr + 4), 32);
                q[i].body.toCharArray((char*)(slotPtr + 4 + 32), 128);
                uint32_t repeatSec = q[i].repeatIntervalMs / 1000U; // RFC-0221
                memcpy(slotPtr + 4 + 32 + 128, &repeatSec, 4);
            }
            realPersist.saveBlob("sched_queue", blob, sizeof(blob));
        });

        telegramPoller->begin();

        // RFC-0200/RFC-0221: Load persisted scheduled SMS queue. NTP has already synced
        // above (syncTime()), so time(nullptr) is valid here.
        {
            static const size_t kSlotSize = sizeof(uint32_t) + 32 + 128 + sizeof(uint32_t); // 168
            static const size_t kNumSlots = 5;
            static const size_t kExpectedSize = 1 + kNumSlots * kSlotSize;
            uint8_t blob[kExpectedSize] = {};
            if (realPersist.loadBlob("sched_queue", blob, kExpectedSize) == kExpectedSize
                && blob[0] == 0x02)
            {
                long nowSec = (long)time(nullptr);
                long nowMs  = (long)(uint32_t)millis();
                auto q = telegramPoller->getSchedQueue(); // copy
                int loaded = 0;
                for (size_t i = 0; i < kNumSlots; i++)
                {
                    const uint8_t *slotPtr = blob + 1 + i * kSlotSize;
                    uint32_t sendAtUnix = 0;
                    memcpy(&sendAtUnix, slotPtr, 4);
                    if (sendAtUnix == 0) continue; // free slot
                    long ageSeconds = nowSec - (long)sendAtUnix;
                    // RFC-0221: repeating slots may be past-due; only stale
                    // one-shots are dropped. For repeating slots (repeatSec > 0)
                    // we accept any past-due time and fire on next tick.
                    uint32_t repeatSec = 0;
                    memcpy(&repeatSec, slotPtr + 4 + 32 + 128, 4);
                    if (ageSeconds >= 2L * 3600L && repeatSec == 0) continue; // stale one-shot: drop
                    char phone[33] = {};
                    char body[129] = {};
                    memcpy(phone, slotPtr + 4,      32);
                    memcpy(body,  slotPtr + 4 + 32, 128);
                    q[i].phone = String(phone);
                    q[i].body  = String(body);
                    q[i].repeatIntervalMs = repeatSec * 1000U; // RFC-0221
                    if (ageSeconds > 0L) {
                        q[i].sendAtMs = 1; // past due: fire on next tick
                    } else {
                        long deltaMs = ((long)sendAtUnix - nowSec) * 1000L;
                        q[i].sendAtMs = (uint32_t)(nowMs + deltaMs);
                    }
                    loaded++;
                }
                telegramPoller->setSchedQueue(q);
                if (loaded > 0)
                {
                    Serial.printf("[RFC-0200] Loaded %d scheduled SMS slot(s) from NVS.\n", loaded);
                    s_schedLoadedAtBoot = loaded; // RFC-0201: include in boot banner
                }
            }
        }

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
    // RFC-0045: Query modem firmware version once at boot (doesn't change).
    {
        modem.sendAT("+CGMR");
        modem.waitResponse(1000UL, cachedModemFirmware);
        cachedModemFirmware.trim();
        // Strip leading "+CGMR: " prefix if present.
        if (cachedModemFirmware.startsWith("+CGMR: "))
            cachedModemFirmware = cachedModemFirmware.substring(7);
        cachedModemFirmware.trim();
    }
    // RFC-0076: Query IMEI once at boot (doesn't change).
    {
        cachedImei = modem.getIMEI();
        cachedImei.trim();
    }
    // RFC-0077: Query SIM ICCID once at boot (changes only on SIM swap).
    {
        cachedIccid = modem.getSimCCID();
        cachedIccid.trim();
    }
    // RFC-0036: Prime SIM slot usage at boot.
    {
        String cpmsResp;
        modem.sendAT("+CPMS?");
        modem.waitResponse(2000UL, cpmsResp);
        int smPos = cpmsResp.indexOf('"');
        if (smPos >= 0) {
            int c1 = cpmsResp.indexOf(',', smPos);
            int c2 = (c1 >= 0) ? cpmsResp.indexOf(',', c1 + 1) : -1;
            if (c1 >= 0 && c2 > c1) {
                cachedSimUsed  = cpmsResp.substring(c1 + 1, c2).toInt();
                int c3 = cpmsResp.indexOf(',', c2 + 1);
                if (c3 < 0) c3 = cpmsResp.length();
                cachedSimTotal = cpmsResp.substring(c2 + 1, c3).toInt();
            }
        }
    }
    {
        // RFC-0116: Differentiate boot banner by reset reason so unexpected
        // reboots (watchdog, panic, brownout) stand out from normal restarts.
        String bootHeader;
        switch (s_resetReason)
        {
        case ESP_RST_POWERON:
            bootHeader = String("\xF0\x9F\x9A\x80 Bridge online\n");       // 🚀
            break;
        case ESP_RST_WDT:
            bootHeader = String("\xE2\x9A\xA0\xEF\xB8\x8F Bridge restarted (watchdog timeout!)\n"); // ⚠️
            break;
        case ESP_RST_PANIC:
            bootHeader = String("\xF0\x9F\x9A\xA8 Bridge restarted (panic/exception!)\n"); // 🚨
            break;
        case ESP_RST_BROWNOUT:
            bootHeader = String("\xE2\x9A\xA1 Bridge restarted (brownout!)\n"); // ⚡
            break;
        default:
            bootHeader = String("\xF0\x9F\x94\x84 Bridge restarted\n");    // 🔄
            break;
        }
        // RFC-0118: Append device label to the header line if set.
        if (s_deviceLabel.length() > 0)
        {
            // Insert label before the trailing newline.
            if (bootHeader.endsWith("\n"))
                bootHeader = bootHeader.substring(0, bootHeader.length() - 1)
                             + String(" [") + s_deviceLabel + String("]\n");
        }
        String bootMsg = bootHeader;
        if (statusFn)
            bootMsg += statusFn();
        else
            bootMsg += "(status not available)";
        // RFC-0201: Append restored-schedule notice if any slots were loaded.
        if (s_schedLoadedAtBoot > 0)
        {
            bootMsg += String("\n\xe2\x8f\xb0 Restored ") // ⏰
                     + String(s_schedLoadedAtBoot)
                     + String(" scheduled SMS from NVS. Use /schedqueue to review.");
        }
        bool bootBannerOk = realBot.sendMessage(bootMsg);
        // RFC-0238: If the boot banner failed, Telegram is unreachable.
        // Defer the boot sweep to the first successful pollUpdates so
        // we don't trigger the consecutive-failure reboot loop before
        // the device even starts serving traffic.
        if (!bootBannerOk)
        {
            s_needBootSweep = true;
            Serial.println("[RFC-0238] Boot banner failed — boot sweep deferred to first successful poll.");
        }
    }

    // Drain anything that arrived while we were offline.
    // RFC-0238: Skip if Telegram unreachable at boot (deferred to first poll).
    if (!s_needBootSweep) {
        int swept = smsHandler.sweepExistingSms(); // RFC-0097
        if (swept > 0) {
            String drainMsg = String("\xF0\x9F\x93\xA8 Drained ") + String(swept) // 📨
                + String(" offline SMS");
            if (swept == 1) drainMsg += ".";
            else            drainMsg += ".";
            realBot.sendMessage(drainMsg);
        }
    }

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

    // RFC-0102: Record boot time for uptime display in /status.
    s_bootMs = (uint32_t)millis();
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

    // RFC-0071: Deferred WiFi reconnect from /wifi bot command.
    if (s_pendingWifiReconnect)
    {
        s_pendingWifiReconnect = false;
        Serial.println("/wifi command received, reconnecting WiFi...");
        WiFi.disconnect(true);
        delay(500);
        if (connectToWiFi())
        {
            if (setupTelegramClient(realBot))
            {
                realBot.sendMessage("\xF0\x9F\x9F\xA2 WiFi reconnected."); // 🟢
                Serial.println("WiFi reconnected successfully.");
            }
            else
            {
                realBot.sendMessage("\xE2\x9A\xA0\xEF\xB8\x8F WiFi up but Telegram TLS failed."); // ⚠️
                Serial.println("WiFi up but Telegram TLS init failed.");
            }
        }
        else
        {
            Serial.println("WiFi reconnect failed.");
            // No Telegram message possible — we're offline.
        }
    }

    // RFC-0192: Forward-pause auto-resume.
    if (s_fwdPauseUntilMs != 0 && (uint32_t)millis() >= s_fwdPauseUntilMs)
    {
        s_fwdPauseUntilMs = 0;
        smsHandler.setForwardingEnabled(true);
        Serial.println("Forward pause expired, forwarding re-enabled.");
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
    // RFC-0234: track last modem serial activity for health check heuristic.
    static unsigned long s_lastSerialActivityMs = 0;
    while (SerialAT.available())
    {
        String line = SerialAT.readStringUntil('\n');
        line.trim();
        if (line.length() == 0)
            continue;
        s_lastSerialActivityMs = millis();

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
                    // RFC-0244: skip forwarding when there is no transport.
                    // handleSmsIndex calls doSendMessage which would fail and
                    // increment noteTelegramFailure(). With no transport, a
                    // burst of 8+ SMS would exhaust the 8-failure limit and
                    // trigger a reboot loop. Leave the SMS on the SIM — it
                    // will be swept when transport becomes available (RFC-0243).
                    if (activeTransport == ActiveTransport::kNone)
                    {
                        Serial.printf("[RFC-0244] +CMTI idx=%d skipped (no transport) — will sweep on connect\n", idx);
                    }
                    else
                    {
                        // RFC-0241: kick WDT — handleSmsIndex can take ~10 s
                        // per SMS (Telegram roundtrip). If multiple +CMTI URCs
                        // pile up in the buffer the loop would exceed 120 s
                        // without a reset (e.g. burst on module wake-up).
                        esp_task_wdt_reset();
                        smsHandler.handleSmsIndex(idx);
                    }
                }
            }
        }

        // RFC-0245: Modem soft-reset detection. The A76xx sends one or
        // more of "RDY", "APP READY", "SMS Ready", "PB DONE" after an
        // internal reset. These indicate that all AT settings (including
        // +CNMI and +CLIP) have been cleared. Re-arm them immediately so
        // SMS notifications resume without waiting for the RFC-0242
        // periodic re-arm (up to 30 min).
        if (line == "RDY" || line == "APP READY" || line == "SMS Ready" ||
            line == "PB DONE")
        {
            Serial.printf("[RFC-0245] Modem ready indicator: \"%s\" — re-arming +CNMI/+CLIP\n",
                          line.c_str());
            realModem.sendAT("+CMGF=0");          // restore PDU mode
            realModem.waitResponseOk(2000UL);
            realModem.sendAT("+CSDH=1");          // restore text header display
            realModem.waitResponseOk(2000UL);
            realModem.sendAT("+CNMI=2,1,0,0,0"); // restore SMS notification
            realModem.waitResponseOk(2000UL);
            realModem.sendAT("+CLIP=1");           // restore caller-ID presentation
            realModem.waitResponseOk(2000UL);
            realModem.sendAT("+CREG=1");           // RFC-0247: restore CREG URC subscription
            realModem.waitResponseOk(2000UL);
            // Sweep SIM — SMS may have arrived during the reset window.
            if (activeTransport != ActiveTransport::kNone)
            {
                esp_task_wdt_reset();
                smsHandler.sweepExistingSms();
                esp_task_wdt_reset();
            }
        }

        // RFC-0247: Real-time registration status update.
        // AT+CREG=1 causes the modem to emit "+CREG: <stat>" whenever
        // registration changes. Update cachedRegStatus immediately and fire
        // the RFC-0082 lost/restored alert without waiting for the 30 s poll.
        // Also handles "+CREG: <n>,<stat>" (solicited-response format) which
        // can appear via the RFC-0236 piggybacked-URC dispatch path — safe
        // because the alert logic is idempotent via s_regLostAlertSent.
        // RFC-0250: shared logic factored into handleCregUrc() helper.
        if (line.startsWith("+CREG:"))
        {
            handleCregUrc(line, "RFC-0247");
        }

        // CallHandler gobbles RING / +CLIP. Feed it every line; it
        // does its own startsWith filtering and ignores anything else.
        callHandler.onUrcLine(line);
    }

    // Drive the CallHandler's unknown-number deadline + cooldown timer.
    // Cheap: constant-time and does no AT traffic unless a deadline fires.
    callHandler.tick();

    // Refresh cached modem CSQ + registration status every 30 s.
    // This block runs AFTER the URC drain and BEFORE the TelegramPoller tick.
    // URCs that arrive during the AT exchanges are captured in raw response
    // buffers and dispatched at the end of the block (RFC-0236) — they cannot
    // be processed by the drain loop above because TinyGSM's waitResponse()
    // consumes them from SerialAT before the loop can see them.
    // RFC-0236: all raw AT response bytes are accumulated in s236raw so
    // piggybacked +CMTI / RING / +CLIP URCs can be dispatched at the end.
    static unsigned long lastStatusRefreshMs = 0;
    if (millis() - lastStatusRefreshMs > 30000UL)
    {
        lastStatusRefreshMs = millis();
        esp_task_wdt_reset(); // RFC-0252: block can call sendMessage() up to 8× (~23 s each)
        String s236raw; // RFC-0236: accumulates raw bytes from all 4 AT exchanges

        // RFC-0236: Use raw AT for CSQ so the captured buffer can be
        // scanned for piggybacked URCs. modem.getSignalQuality() uses
        // waitResponse() internally which swallows +CMTI silently.
        {
            String csqRaw;
            realModem.sendAT("+CSQ");
            realModem.waitResponse(2000UL, csqRaw);
            s236raw += csqRaw;
            int p = csqRaw.indexOf("+CSQ:");
            if (p >= 0) {
                int comma = csqRaw.indexOf(',', p);
                String rssiStr = (comma > p) ? csqRaw.substring(p + 5, comma)
                                             : csqRaw.substring(p + 5);
                rssiStr.trim();
                int v = rssiStr.toInt();
                if (v >= 0 && v <= 31) cachedCsq = v;
                else if (v == 99)      cachedCsq = 99;
            }
        }

        // RFC-0236: Use raw AT for CREG. +CREG stat values map 1-to-1
        // to TinyGSM's RegStatus enum so we cast directly.
        {
            String cregRaw;
            realModem.sendAT("+CREG?");
            realModem.waitResponse(2000UL, cregRaw);
            s236raw += cregRaw;
            int p = cregRaw.indexOf("+CREG:");
            if (p >= 0) {
                int comma = cregRaw.indexOf(',', p);
                if (comma > p) {
                    int stat = cregRaw.substring(comma + 1).toInt();
                    switch (stat) {
                        case 1:  cachedRegStatus = REG_OK_HOME;       break;
                        case 5:  cachedRegStatus = REG_OK_ROAMING;    break;
                        case 6:  cachedRegStatus = REG_SMS_ONLY;      break;
                        case 2:  cachedRegStatus = REG_SEARCHING;     break;
                        case 3:  cachedRegStatus = REG_DENIED;        break;
                        case 4:  cachedRegStatus = REG_UNKNOWN;       break;
                        default: cachedRegStatus = REG_UNREGISTERED;  break;
                    }
                }
            }
        }
        // RFC-0082: Network registration loss / recovery alert.
        {
            bool regOk = (cachedRegStatus == REG_OK_HOME    ||
                          cachedRegStatus == REG_OK_ROAMING ||
                          cachedRegStatus == REG_SMS_ONLY);
            const char *regTxt = (cachedRegStatus == REG_OK_HOME)      ? "home"
                               : (cachedRegStatus == REG_OK_ROAMING)   ? "roaming"
                               : (cachedRegStatus == REG_SMS_ONLY)     ? "sms-only"
                               : (cachedRegStatus == REG_SEARCHING)    ? "searching"
                               : (cachedRegStatus == REG_DENIED)       ? "denied"
                               : (cachedRegStatus == REG_UNREGISTERED) ? "unregistered"
                               : (cachedRegStatus == REG_UNKNOWN)      ? "unknown"
                               :                                         "unknown";
            if (!regOk && !s_regLostAlertSent && cachedCsq > 0) {
                if (!alertsMuted()) { // RFC-0098
                    esp_task_wdt_reset(); // RFC-0252
                    realBot.sendMessage(
                        String("\xF0\x9F\x93\xB5 Network registration lost (") + regTxt + ")"); // 📵
                }
                s_regLostAlertSent = true;
            } else if (regOk && s_regLostAlertSent) {
                if (!alertsMuted()) { // RFC-0098
                    esp_task_wdt_reset(); // RFC-0252
                    realBot.sendMessage(
                        String("\xE2\x9C\x85 Network registration restored (") + regTxt + ")"); // ✅
                }
                s_regLostAlertSent = false;
            }
        }
        // RFC-0113: WiFi low-RSSI alert. Hysteresis: alert at < -80 dBm, clear at >= -70 dBm.
        {
            if (WiFi.status() == WL_CONNECTED)
            {
                int rssi = WiFi.RSSI();
                if (rssi < -80 && !s_lowWifiRssiAlertSent && !alertsMuted())
                {
                    String msg = String("\xE2\x9A\xA0\xEF\xB8\x8F Weak WiFi: "); // ⚠️
                    msg += String(rssi); msg += " dBm — Telegram delivery may be unreliable.";
                    esp_task_wdt_reset(); // RFC-0252
                    realBot.sendMessage(msg);
                    s_lowWifiRssiAlertSent = true;
                }
                else if (rssi >= -70)
                {
                    s_lowWifiRssiAlertSent = false;
                }
            }
            else
            {
                s_lowWifiRssiAlertSent = false; // reset so alert re-fires on reconnect
            }
        }
        // RFC-0031: Record CSQ sample in rolling history.
        csqHistory[csqHistoryIdx] = cachedCsq;
        csqHistoryIdx = (csqHistoryIdx + 1) % 6;
        if (csqHistoryIdx == 0) csqHistoryFull = true;
        // RFC-0027: Cache operator name via AT+COPS?
        {
            String copsResp;
            modem.sendAT("+COPS?");
            modem.waitResponse(2000UL, copsResp);
            s236raw += copsResp; // RFC-0236: accumulate for piggybacked URC scan
            int q1 = copsResp.indexOf('"');
            int q2 = (q1 >= 0) ? copsResp.indexOf('"', q1 + 1) : -1;
            cachedOperatorName = (q1 >= 0 && q2 > q1)
                                 ? copsResp.substring(q1 + 1, q2)
                                 : String();
        }
        // RFC-0036: Cache SIM slot usage via AT+CPMS?
        // Response: +CPMS: "SM",used,total,...
        {
            String cpmsResp;
            modem.sendAT("+CPMS?");
            modem.waitResponse(2000UL, cpmsResp);
            s236raw += cpmsResp; // RFC-0236: accumulate for piggybacked URC scan
            // Find first "SM" or "ME" entry; take the numbers after it.
            int smPos = cpmsResp.indexOf('"');
            if (smPos >= 0) {
                int c1 = cpmsResp.indexOf(',', smPos);
                int c2 = (c1 >= 0) ? cpmsResp.indexOf(',', c1 + 1) : -1;
                if (c1 >= 0 && c2 > c1) {
                    cachedSimUsed  = cpmsResp.substring(c1 + 1, c2).toInt();
                    int c3 = cpmsResp.indexOf(',', c2 + 1);
                    if (c3 < 0) c3 = cpmsResp.length();
                    cachedSimTotal = cpmsResp.substring(c2 + 1, c3).toInt();
                }
            }
        }
        // RFC-0064: SIM slot full warning. Alert once when usage crosses ≥80%;
        // reset when usage drops below threshold so re-fills re-alert.
        if (cachedSimTotal > 0) {
            bool nearFull = (cachedSimUsed * 5 >= cachedSimTotal * 4); // ≥80%
            if (nearFull && !s_simFullWarnSent) {
                String simMsg = String("\xE2\x9A\xA0\xEF\xB8\x8F SIM storage "); // ⚠️
                simMsg += String(cachedSimUsed); simMsg += "/"; simMsg += String(cachedSimTotal);
                simMsg += " slots (\xe2\x89\xa580%). Delete old SMS or SIM may reject new ones."; // ≥
                esp_task_wdt_reset(); // RFC-0252
                realBot.sendMessage(simMsg);
                s_simFullWarnSent = true;
            } else if (!nearFull) {
                s_simFullWarnSent = false;
            }
        }
        // RFC-0081: CSQ low-signal alert. Hysteresis: alert at ≤5, clear at >10.
        // CSQ==0 means "unknown" (modem not yet polled) — skip.
        if (cachedCsq > 0 && cachedCsq <= 5 && !s_lowCsqWarnSent) {
            if (!alertsMuted()) { // RFC-0098
                String csqMsg = String("\xF0\x9F\x93\xB6 Low signal: CSQ "); // 📶
                csqMsg += String(cachedCsq); csqMsg += ". SMS delivery may be unreliable.";
                esp_task_wdt_reset(); // RFC-0252
                realBot.sendMessage(csqMsg);
            }
            s_lowCsqWarnSent = true;
        } else if (cachedCsq > 10) {
            s_lowCsqWarnSent = false;
        }

        // RFC-0066: Low heap warning. Hysteresis: alert at <15 KB, clear at >25 KB.
        // RFC-0073: Critical threshold at <8 KB — send final message and reboot.
        {
            uint32_t freeHeap = ESP.getFreeHeap();
            if (freeHeap < 8u * 1024u) {
                // RFC-0073: Too close to the edge — reboot before we crash silently.
                String critMsg = String("\xF0\x9F\x92\x80 Critical heap: "); // 💀
                critMsg += String((int)freeHeap); critMsg += " B free — rebooting now.";
                esp_task_wdt_reset(); // RFC-0252: ensure WDT alive for this critical send
                realBot.sendMessage(critMsg);
                delay(500);
                ESP.restart();
            } else if (freeHeap < 15u * 1024u && !s_lowHeapWarnSent) {
                String heapMsg = String("\xE2\x9A\xA0\xEF\xB8\x8F Low heap: "); // ⚠️
                heapMsg += String((int)freeHeap); heapMsg += " B free. Device may become unstable.";
                esp_task_wdt_reset(); // RFC-0252
                realBot.sendMessage(heapMsg);
                s_lowHeapWarnSent = true;
            } else if (freeHeap > 25u * 1024u) {
                s_lowHeapWarnSent = false;
            }
        }
        // RFC-0079: Periodic NTP retry when clock is still invalid (every 5 min).
        if (activeTransport == ActiveTransport::kWiFi && time(nullptr) <= 8 * 3600 * 2)
        {
            if (millis() - s_lastNtpRetryMs >= kNtpRetryIntervalMs) {
                s_lastNtpRetryMs = millis();
                syncTime();
                if (time(nullptr) > 8 * 3600 * 2) {
                    esp_task_wdt_reset(); // RFC-0252
                    realBot.sendMessage(
                        String("\xF0\x9F\x95\x90 Clock synced via NTP.")); // 🕐
                }
            }
        }

        // RFC-0096: Stuck-queue alert. Check every minute; fire if any entry
        // has been waiting > kStuckQueueThresholdMs without delivering.
        if (millis() - s_lastStuckQueueCheckMs >= kStuckQueueCheckIntervalMs)
        {
            s_lastStuckQueueCheckMs = millis();
            auto snapshot = smsSender.getQueueSnapshot();
            bool hasStuck = false;
            uint32_t nowMs2 = (uint32_t)millis();
            String oldestPhone;
            uint32_t oldestAgeSec = 0;
            int stuckCount = 0;
            for (const auto &e : snapshot) {
                if (e.queuedAtMs > 0 && nowMs2 >= e.queuedAtMs &&
                    (nowMs2 - e.queuedAtMs) >= kStuckQueueThresholdMs)
                {
                    hasStuck = true;
                    stuckCount++;
                    uint32_t ageSec = (nowMs2 - e.queuedAtMs) / 1000;
                    if (ageSec > oldestAgeSec) {
                        oldestAgeSec = ageSec;
                        oldestPhone  = e.phone;
                    }
                }
            }
            if (hasStuck && !s_stuckQueueAlertSent) {
                if (!alertsMuted()) { // RFC-0098
                    String alert = String("\xE2\x9A\xA0\xEF\xB8\x8F Queue stuck: "); // ⚠️
                    alert += String(stuckCount);
                    alert += String(" entr"); alert += (stuckCount == 1 ? "y" : "ies");
                    alert += String(" waiting >5m. Oldest: "); alert += oldestPhone;
                    alert += String(" ("); alert += String((int)(oldestAgeSec / 60)); alert += "m)\n";
                    alert += String("Use /queue to inspect, /flushqueue to retry, /clearqueue to discard.");
                    esp_task_wdt_reset(); // RFC-0252
                    realBot.sendMessage(alert);
                }
                s_stuckQueueAlertSent = true;
            } else if (!hasStuck) {
                s_stuckQueueAlertSent = false; // reset when queue clears
            }
        }

        // RFC-0075: Daily stats digest. First tick initialises the timer;
        // subsequent ticks check whether 24 hours have elapsed.
        {
            if (s_lastDailyDigestMs == 0) {
                s_lastDailyDigestMs = millis(); // arm the timer without sending
            } else if (millis() - s_lastDailyDigestMs >= 24UL * 3600UL * 1000UL) {
                s_lastDailyDigestMs = millis();
                String digest;
                digest += "\xF0\x9F\x93\x8A 24 h digest | "; // 📊
                digest += "fwd "; digest += String(smsHandler.smsForwarded());
                digest += " (session) "; digest += String(s_lifetimeFwdCount); digest += " (lifetime)";
                if (smsHandler.smsBlocked() > 0) {
                    digest += " | blocked "; digest += String(smsHandler.smsBlocked());
                }
                if (smsHandler.smsDeduplicated() > 0) {
                    digest += " | deduped "; digest += String(smsHandler.smsDeduplicated());
                }
                digest += " | heap "; digest += String((int)(ESP.getFreeHeap() / 1024)); digest += " KB free";
                esp_task_wdt_reset(); // RFC-0252
                realBot.sendMessage(digest);
            }
        }

#ifdef ENABLE_DELIVERY_REPORTS
        // Evict delivery report map entries older than 1 hour (RFC-0011).
        deliveryReportMap.evictExpired((uint32_t)millis());
#endif

        // RFC-0236 + RFC-0234: Any modem response means the modem is alive.
        // The AT exchanges consume bytes from SerialAT via TinyGSM's internal
        // waitResponse(), so the URC drain loop never sees them and
        // s_lastSerialActivityMs is not updated. Update it here to prevent
        // the RFC-0234 silence heuristic from falsely declaring the modem hung.
        if (s236raw.length() > 0)
            s_lastSerialActivityMs = millis();

        // RFC-0236: Scan all captured AT response buffers for piggybacked
        // +CMTI / RING / +CLIP URCs. TinyGSM's waitResponse() reads every
        // byte arriving on SerialAT into the capture string — URCs that race
        // the AT exchange land here instead of in our drain loop above. Dispatch
        // them now so no SMS is silently dropped during the 30 s status refresh.
        {
            int pos = 0;
            while (pos < (int)s236raw.length())
            {
                int eol = s236raw.indexOf('\n', pos);
                String line;
                if (eol < 0) {
                    line = s236raw.substring(pos);
                    pos  = (int)s236raw.length();
                } else {
                    line = s236raw.substring(pos, eol);
                    pos  = eol + 1;
                }
                line.trim();
                if (line.length() == 0) continue;
                if (line.startsWith("+CMTI:")) {
                    int comma = line.indexOf(',');
                    if (comma > 0) {
                        int idx = line.substring(comma + 1).toInt();
                        if (idx > 0) {
                            Serial.printf("[RFC-0236] Piggybacked +CMTI idx=%d — dispatching\n", idx);
                            esp_task_wdt_reset();
                            smsHandler.handleSmsIndex(idx);
                        }
                    }
                } else if (line.startsWith("RING") || line.startsWith("+CLIP:")) {
                    callHandler.onUrcLine(line);
                } else if (line.startsWith("+CREG:")) {
                    // RFC-0247: real-time registration update via piggybacked URC.
                    // Idempotent via s_regLostAlertSent; the 30 s block also checks
                    // AT+CREG? so duplicate processing is harmless.
                    // RFC-0250: shared logic factored into handleCregUrc() helper.
                    handleCregUrc(line, "RFC-0247/236");
                }
            }
        }
    }

    // RFC-0235: Periodic SIM sweep — recover SMS that failed to forward
    // during a transient Telegram outage (WiFi was up, Telegram was down).
    // sweepExistingSms() at boot handles the common case; this handles the
    // case where Telegram recovers without a WiFi drop/reconnect event.
    {
        static unsigned long lastPeriodicSweepMs = 0;
        const unsigned long kSweepIntervalMs = 30UL * 60UL * 1000UL; // 30 min
        if (activeTransport != ActiveTransport::kNone &&
            millis() - lastPeriodicSweepMs >= kSweepIntervalMs &&
            lastPeriodicSweepMs != 0) // skip first iteration (boot sweep already done)
        {
            lastPeriodicSweepMs = millis();
            // RFC-0242: Re-arm AT settings before each sweep. The A76xx
            // firmware can silently revert PDU mode, header display, or URC
            // subscriptions during normal operation — not just on explicit
            // soft-resets. Re-arm the full set every 30 min to prevent a
            // silently-reverted +CMGF from breaking the PDU receive pipeline.
            realModem.sendAT("+CMGF=0");          // ensure PDU mode
            realModem.waitResponseOk(2000UL);
            realModem.sendAT("+CSDH=1");          // ensure text header display
            realModem.waitResponseOk(2000UL);
            realModem.sendAT("+CNMI=2,1,0,0,0"); // ensure SMS notification
            realModem.waitResponseOk(2000UL);
            realModem.sendAT("+CLIP=1");           // ensure caller-ID presentation
            realModem.waitResponseOk(2000UL);
            realModem.sendAT("+CREG=1");           // RFC-0247: ensure CREG URC subscription
            realModem.waitResponseOk(2000UL);
            esp_task_wdt_reset();
            smsHandler.sweepExistingSms();
            esp_task_wdt_reset();
        }
        else if (lastPeriodicSweepMs == 0)
        {
            lastPeriodicSweepMs = millis(); // initialise without sweeping
        }
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
        if (s_heartbeatIntervalSec > 0 &&
            (uint32_t)(nowMs - lastHeartbeatMs) >= s_heartbeatIntervalSec * 1000u)
        {
            lastHeartbeatMs = nowMs;  // advance regardless of send result
            esp_task_wdt_reset();  // heartbeat sendMessage can block; pet WDT first
            // RFC-0051: compact one-line heartbeat (uptime | signal | fwd | queue).
            {
                // RFC-0102: uptime from end of setup(), not ESP power-on.
                unsigned long uptimeSec2 = ((uint32_t)millis() - s_bootMs) / 1000UL;
                unsigned long days2  = uptimeSec2 / 86400UL;
                unsigned long hours2 = (uptimeSec2 % 86400UL) / 3600UL;
                unsigned long mins2  = (uptimeSec2 % 3600UL) / 60UL;
                String hb = String("\xE2\x8F\xB1 "); // ⏱
                hb += String((int)days2); hb += "d ";
                hb += String((int)hours2); hb += "h ";
                hb += String((int)mins2); hb += "m";
                hb += String(" | CSQ "); hb += String(cachedCsq);
                if (cachedOperatorName.length() > 0) { hb += " "; hb += cachedOperatorName; }
                hb += String(" | WiFi "); hb += String(WiFi.RSSI()); hb += "dBm";
                hb += String(" | fwd "); hb += String(smsHandler.smsForwarded());
                hb += String(" | calls "); hb += String(callHandler.callsReceived()); // RFC-0109
                hb += String(" | q "); hb += String(smsSender.queueSize());
                hb += String("/"); hb += String(SmsSender::kQueueSize);
                // RFC-0210: append sched slot count when > 0.
                {
                    int schedCount = 0;
                    const auto& sq = telegramPoller->getSchedQueue();
                    for (const auto& slot : sq)
                        if (slot.sendAtMs != 0) schedCount++;
                    if (schedCount > 0) {
                        hb += String(" | sched "); hb += String(schedCount);
                    }
                }
#ifdef USSD_BALANCE_CODE
                // RFC-0106: Append USSD balance check to heartbeat.
                {
                    String bal = realModem.ussdQuery(String(USSD_BALANCE_CODE), 10000UL);
                    if (bal.length() > 0)
                    {
                        if (bal.length() > 40) bal = bal.substring(0, 40) + "\xE2\x80\xA6"; // …
                        hb += String(" | Bal: "); hb += bal;
                    }
                }
#endif
                if (!realBot.sendMessage(hb))
                    Serial.println("Heartbeat: sendMessage failed (connectivity issue)");
            }
        }
    }
#endif

    // RFC-0056: Periodic NTP resync (every 24 hours) to keep the clock
    // accurate over long uptimes. Uses the same syncTime() as boot.
    // Runs only on the WiFi path (syncTime needs network; cellular path
    // uses NTP only when transitioning back to WiFi anyway).
    {
        static uint32_t lastNtpResyncMs = 0;
        constexpr uint32_t kNtpResyncIntervalMs = 24UL * 60UL * 60UL * 1000UL; // 24h
        uint32_t nowMs2 = (uint32_t)millis();
        if (activeTransport == ActiveTransport::kWiFi &&
            (uint32_t)(nowMs2 - lastNtpResyncMs) >= kNtpResyncIntervalMs &&
            lastNtpResyncMs != 0) // skip first iteration (boot already synced)
        {
            lastNtpResyncMs = nowMs2;
            Serial.println("RFC-0056: Periodic NTP resync...");
            esp_task_wdt_reset();
            syncTime();
        }
        else if (lastNtpResyncMs == 0)
        {
            lastNtpResyncMs = nowMs2; // initialise to now (first boot sync already done)
        }
    }

    // RFC-0229/0234: Periodic modem AT health check.
    // Only fire if there has been no serial activity for at least
    // kModemSilenceThresholdMs (2 min). Normal modems emit periodic URCs
    // (registration updates, CSQ etc.) so extended silence is suspicious.
    // After 2 consecutive silent+failed checks (~4 min silence) → reboot.
    static unsigned long lastModemCheck   = 0;
    static int           modemFailStreak  = 0;
    static const unsigned long kModemCheckIntervalMs      = 120000UL; // 2 min
    static const unsigned long kModemSilenceThresholdMs   = 120000UL; // 2 min silence needed
    bool modemSilent = (s_lastSerialActivityMs > 0) &&
                       (millis() - s_lastSerialActivityMs > kModemSilenceThresholdMs);
    if (modemSilent && millis() - lastModemCheck > kModemCheckIntervalMs)
    {
        lastModemCheck = millis();
        esp_task_wdt_reset();
        // RFC-0239: Use raw AT instead of modem.testAT() so the response
        // buffer can be scanned for piggybacked +CMTI / RING / +CLIP URCs.
        // modem.testAT() discards all data it reads; raw waitResponse()
        // captures it. Same 3 s timeout as before.
        String atHealthRaw;
        realModem.sendAT(""); // sends AT\r\n → modem responds OK
        bool atHealthOk = (realModem.waitResponse(3000UL, atHealthRaw) == 1);
        // Update silence timer — modem just responded.
        if (atHealthRaw.length() > 0)
            s_lastSerialActivityMs = millis();
        // Dispatch any piggybacked URCs captured during the health check.
        {
            int p239 = 0;
            while (p239 < (int)atHealthRaw.length()) {
                int e239 = atHealthRaw.indexOf('\n', p239);
                String l239 = (e239 < 0) ? atHealthRaw.substring(p239) : atHealthRaw.substring(p239, e239);
                p239 = (e239 < 0) ? (int)atHealthRaw.length() : e239 + 1;
                l239.trim();
                if (l239.length() == 0) continue;
                if (l239.startsWith("+CMTI:")) {
                    int cm239 = l239.indexOf(',');
                    if (cm239 > 0) {
                        int idx239 = l239.substring(cm239 + 1).toInt();
                        if (idx239 > 0) {
                            Serial.printf("[RFC-0239] Piggybacked +CMTI idx=%d — dispatching\n", idx239);
                            esp_task_wdt_reset();
                            smsHandler.handleSmsIndex(idx239);
                        }
                    }
                } else if (l239.startsWith("RING") || l239.startsWith("+CLIP:")) {
                    callHandler.onUrcLine(l239);
                } else if (l239.startsWith("+CREG:")) {
                    // RFC-0247: real-time registration update via piggybacked URC.
                    // RFC-0250: shared logic factored into handleCregUrc() helper.
                    handleCregUrc(l239, "RFC-0247/239");
                }
            }
        }
        if (!atHealthOk)
        {
            modemFailStreak++;
            Serial.print("Modem health check failed (streak=");
            Serial.print(modemFailStreak);
            Serial.println(")");
            if (modemFailStreak >= 2)
            {
                realBot.sendMessage(String("\xe2\x9a\xa0\xef\xb8\x8f Modem unresponsive (2 checks) — rebooting.")); // ⚠️
                delay(1000);
                ESP.restart();
            }
        }
        else
        {
            modemFailStreak = 0;
            // RFC-0242: Re-arm AT settings after a silence period.
            // Extended silence can follow an internal modem state change
            // that clears settings without triggering a ready indicator.
            // Re-arm PDU mode and header display in addition to URC
            // subscriptions — a silent reset reverts all of these.
            realModem.sendAT("+CMGF=0");          // restore PDU mode
            realModem.waitResponseOk(2000UL);
            realModem.sendAT("+CSDH=1");          // restore text header display
            realModem.waitResponseOk(2000UL);
            realModem.sendAT("+CNMI=2,1,0,0,0"); // restore SMS notification
            realModem.waitResponseOk(2000UL);
            realModem.sendAT("+CLIP=1");           // restore caller-ID presentation
            realModem.waitResponseOk(2000UL);
            realModem.sendAT("+CREG=1");           // RFC-0247: restore CREG URC subscription
            realModem.waitResponseOk(2000UL);
        }
        esp_task_wdt_reset();
    }
    else if (!modemSilent)
    {
        modemFailStreak = 0; // reset streak if modem is actively sending
    }

    // Periodically verify the active transport is still viable.
    // - If WiFi was primary and drops: try WiFi reconnect. If it's still down
    //   on the second consecutive check (~60 s later), fall over to cellular
    //   (RFC-0004). We use a flag rather than a blocking delay so the URC
    //   drain keeps running uninterrupted.
    // - If cellular was primary: check whether WiFi has recovered; if so,
    //   switch back to the verified (CA-bundle) path.
    static unsigned long lastTransportCheck = 0;
    static bool wifiDownLastCheck = false;
    static unsigned long wifiDownSinceMs = 0; // RFC-0039: when WiFi first dropped
    if (millis() - lastTransportCheck > 30000)
    {
        lastTransportCheck = millis();
        if (activeTransport == ActiveTransport::kWiFi)
        {
            if (WiFi.status() != WL_CONNECTED)
            {
                if (wifiDownLastCheck)
                {
                    // Two consecutive checks with WiFi down — fall over.
#if defined(CELLULAR_APN) && defined(TINY_GSM_MODEM_A76XXSSL)
                    Serial.println("WiFi still down; falling over to cellular transport...");
                    esp_task_wdt_reset(); // gprsConnect can block 60+ s
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
                    // RFC-0237: After 5 min down, escalate from WiFi.reconnect()
                    // to a full disconnect + begin cycle. WiFi.reconnect() can get
                    // stuck on some ESP32 WiFi driver versions and will not recover
                    // without re-running begin() with credentials.
                    const unsigned long kWifiHardResetMs = 5UL * 60UL * 1000UL;
                    if (wifiDownSinceMs > 0 &&
                        (millis() - wifiDownSinceMs) >= kWifiHardResetMs)
                    {
                        Serial.println("[RFC-0237] WiFi down >5 min — full reconnect cycle");
                        WiFi.disconnect(false); // disconnect, keep radio on
                        delay(100);
                        WiFi.begin(ssid, password); // non-blocking, re-applies credentials
                        wifiDownSinceMs = millis(); // reset so escalation re-arms 5 min later
                    }
                    else
                    {
                        Serial.println("WiFi dropped, attempting reconnect...");
                        WiFi.reconnect();
                    }
                }
                else
                {
                    Serial.println("WiFi dropped, attempting reconnect...");
                    WiFi.reconnect();
                    wifiDownLastCheck = true;
                    wifiDownSinceMs   = millis(); // RFC-0039: record drop time
                }
            }
            else
            {
                // RFC-0039: WiFi was down last check but is now back up.
                if (wifiDownLastCheck)
                {
                    unsigned long downSec = (millis() - wifiDownSinceMs) / 1000UL;
                    String notif = String("\xF0\x9F\x94\x97 WiFi reconnected"); // 🔗
                    notif += " (was down ";
                    if (downSec >= 60) { notif += String((unsigned long)(downSec / 60)); notif += "m "; }
                    notif += String((unsigned long)(downSec % 60)); notif += "s)";
                    realBot.sendMessage(notif);
                    Serial.println(notif);
                    // RFC-0047: sweep SIM for any SMS that arrived while
                    // Telegram was unreachable — they'll be on the SIM
                    // already but handleSmsIndex never succeeded.
                    esp_task_wdt_reset();
                    smsHandler.sweepExistingSms();
                    esp_task_wdt_reset(); // RFC-0241: sweep may process many SMS
                }
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
                            // RFC-0243: sweep SIM after cellular→WiFi switch —
                            // any SMS that failed to forward on the cellular
                            // path are still on the SIM.
                            esp_task_wdt_reset();
                            smsHandler.sweepExistingSms();
                            esp_task_wdt_reset();
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
                        // RFC-0243: sweep SIM now that transport is available —
                        // any SMS that arrived before transport was ready
                        // would have been deferred (s_needBootSweep or failures).
                        // The RFC-0238 onPollSuccessFn handles the boot-sweep flag,
                        // but a direct sweep here ensures fast recovery on the
                        // first successful transport establishment.
                        // Note: if s_needBootSweep is set, the onPollSuccessFn will
                        // also sweep — that's harmless (CMGL finds nothing if empty).
                        esp_task_wdt_reset();
                        smsHandler.sweepExistingSms();
                        esp_task_wdt_reset();
                    }
                }
            }
#if defined(CELLULAR_APN) && defined(TINY_GSM_MODEM_A76XXSSL)
            if (activeTransport == ActiveTransport::kNone)
            {
                esp_task_wdt_reset(); // gprsConnect can block 60+ s
                if (modem.gprsConnect(CELLULAR_APN))
                {
                    if (setupCellularClient(realBot))
                    {
                        activeTransport = ActiveTransport::kCellular;
                        Serial.println("Cellular transport established (deferred).");
                        // RFC-0243: sweep SIM now that cellular transport is available.
                        esp_task_wdt_reset();
                        smsHandler.sweepExistingSms();
                        esp_task_wdt_reset();
                    }
                }
            }
#endif
        }
    }

    // RFC-0246: Recovery sweep.  When consecutiveFailures_ drops from >0 to 0
    // it means Telegram just became reachable again after a failure window.
    // SMS that accumulated on the SIM during the outage are not automatically
    // forwarded — only the one +CMTI that happened to succeed is processed.
    // Sweep immediately so the backlog is drained without waiting for the
    // 30-min periodic sweep (RFC-0235).
    {
        static int s_prevConsecFailures = 0;
        int cur = smsHandler.consecutiveFailures();
        if (s_prevConsecFailures > 0 && cur == 0 &&
            activeTransport != ActiveTransport::kNone)
        {
            Serial.println("[RFC-0246] Telegram recovered — sweeping SIM for pending SMS");
            esp_task_wdt_reset();
            smsHandler.sweepExistingSms();
            esp_task_wdt_reset();
        }
        // Update AFTER the sweep so the transition is not re-triggered.
        s_prevConsecFailures = smsHandler.consecutiveFailures();
    }

    delay(50);
}
