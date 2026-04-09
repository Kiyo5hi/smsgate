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
#include "telegram.h"
#include "sms_handler.h"
#include "call_handler.h"
#include "real_modem.h"

#ifdef TINY_GSM_MODEM_SIM7080
#error "This modem has no SMS function"
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

// Composition root state. These objects are singletons for the
// lifetime of the process; the handlers borrow references to them.
static RealModem realModem(modem);
static RealBotClient realBot;
static SmsHandler smsHandler(realModem, realBot, []() {
    // Production reboot callback. Short delay gives the last Serial
    // line a chance to flush before the chip resets.
    delay(1000);
    ESP.restart();
});
static CallHandler callHandler(realModem, realBot, []() -> uint32_t {
    return (uint32_t)millis();
});

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

    // Enable Caller Line Identification Presentation so incoming RINGs
    // are followed by a +CLIP: "<number>",<type>,... URC carrying the
    // caller's number. See RFC-0005 / call_handler.{h,cpp}.
    modem.sendAT("+CLIP=1");
    modem.waitResponse(2000);

    connectToWiFi();
    syncTime();
    if (!setupTelegramClient())
    {
        Serial.println("Telegram client setup failed, aborting SMS bridge.");
        return;
    }

    realBot.sendMessage("🚀 Modem SMS to Telegram Bridge is now online!");

    // Drain anything that arrived while we were offline.
    smsHandler.sweepExistingSms();
}

void loop()
{
    // NOTE: do NOT call modem.maintain() here. On TinyGSM/A76XX it internally
    // calls waitResponse() which eats unknown URCs (+CMTI included) and only
    // prints "### Unhandled: ..." in debug mode — meaning our +CMTI would be
    // consumed before we ever see it in SerialAT.available() below.
    // We drain the serial buffer ourselves and dispatch the URCs we care about.

    // Consume unsolicited lines and dispatch to the SMS / Call handlers.
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
