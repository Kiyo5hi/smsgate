#pragma once

#include <Arduino.h>

// Initialize the TLS client and open the first connection to api.telegram.org.
// Returns true on success.
bool setupTelegramClient();

// POST a plain-text message to the configured chat. Returns true iff the
// HTTP request returned 200 and the API replied "ok":true.
bool sendBotMessage(const String &message);
