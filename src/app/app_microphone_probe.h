#pragma once

/* NULL lists inputs only. An exact device name requests a one-second local
 * capture test. No audio is saved, played, or transmitted. Run before streaming.
 * Device/PCM availability does not establish physical headset identity. */
int app_microphone_probe(const char *device_name);
