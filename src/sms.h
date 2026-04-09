#pragma once

// SMS receive pipeline. All functions act on the global `modem` instance
// defined in main.cpp.

// Read the SMS at SIM index <idx>, forward it to Telegram, and delete it
// from the SIM on success. Leaves the SMS in place on failure so a later
// retry can pick it up. Reboots the ESP after too many consecutive failures
// to escape stuck TLS / WiFi states.
void handleSmsIndex(int idx);

// Drain every SMS currently on the SIM via AT+CMGL. Used at startup to
// catch up on messages that arrived while the bridge was offline.
void sweepExistingSms();
