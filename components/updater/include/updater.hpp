#pragma once
#include "cJSON.h"
namespace sol {
bool updaterBegin();
// Synchronous: queries GitHub for the latest release and refreshes the shared state. Safe to call
// from an HTTP request handler (blocks for the network round trip, a second or two typically).
bool updaterCheckNow();
// Synchronous: downloads and flashes the release already found by the last check, then reboots on
// success. False if no update is known yet, or an OTA (this or the manual browser upload) is
// already running.
bool updaterInstall();
cJSON *updaterJson();
// Shared OTA re-entrancy guard between this component's auto-update and the manual browser
// upload in components/api, so only one firmware write can be in flight at a time.
bool otaBeginGuard();
void otaEndGuard();
bool otaBusy();
} // namespace sol
