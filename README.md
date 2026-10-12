# HaloHealth Medical Kit Box

A two-unit IoT pulse oximeter that measures heart rate (BPM) and blood oxygen saturation (SpO2), shows the result on a touchscreen kit box, and stores every confirmed measurement as a timestamped history in Firebase Firestore for the HaloHealth mobile app. The firmware is written for two ESP32 boards (ESP32-C3 and ESP32-S3) with the Arduino framework and is built with PlatformIO.

> This is an academic prototype developed for the IoT Design & Application course (CPEN6254010) at Binus University. It is not a medical device and must not be used as a basis for diagnosis.

---

## Project Team

| Name | Student ID | Role |
|------|------------|------|
| Nathalie Arwan | 2802454820 | PCB design; casing design; component selection; breadboard testing; PCB fabrication; Firebase configuration from the mobile application side |
| Azalia Varsha Jeslyn Putri Legowo | 2802519902 | MAX30102 sensor configuration; PCB fabrication |
| Julius Lim Khan | 2802527381 | Firebase (Firestore) configuration; TFT display access; EEPROM configuration on the ESP32-S3; ESP-NOW protocol configuration |

---

## Background and Objectives

More people now check their basic health condition at home before deciding whether to see a doctor. Many telemedicine platforms only handle the consultation itself and do not keep a structured record of the patient's vital signs, so a doctor cannot see how the values change over time.

HaloHealth is a home telemedicine kit box concept that combines several basic health-checking tools into one portable box connected to a mobile application. Every measurement is stored automatically as part of the patient's history after the user confirms it. In this stage of development only the pulse oximeter is fully implemented. The other planned tools (blood pressure monitor, thermometer, and general heart rate monitor) are represented by dummy data in the mobile application.

The system follows the usual three-layer IoT structure. The MAX30102 sensor and the ESP32-C3 form the sensing layer, Firebase Firestore is the cloud layer, and the HaloHealth mobile application is the access layer.

The objectives are as follows.

1. Implement a pulse oximeter with the MAX30102 and the ESP32-C3 that calculates BPM and SpO2 from raw PPG signals, using a signal correlation of at least 0.78 as the validity threshold.
2. Relay the result from the sensing unit (ESP32-C3) to the display unit (ESP32-S3) with ESP-NOW, without requiring the sensing unit to join a WiFi network.
3. Store every confirmed measurement in Firestore under the correct user, as a continuous and timestamped history rather than a single overwritten value.
4. Let the user accept or reject a result on the kit box display before it is stored.

---

## Key Features

- Real-time BPM and SpO2 calculation on the ESP32-C3 from red and infrared PPG signals.
- Layered signal validation: finger detection, red/infrared correlation check, valid BPM range, and a SpO2 check that depends on the BPM check.
- Median-of-3 filtering followed by exponential smoothing for stable readings.
- Wireless relay between the two boards with ESP-NOW, with no wire and no WiFi connection at the sensor side.
- Touchscreen confirmation (YES / NO) before any data is written to the cloud.
- Dual-core firmware on the ESP32-S3 with FreeRTOS: the user interface runs on Core 1 and all Firebase work runs on Core 0.
- Per-device Kit Code stored in EEPROM, used to link the physical kit to a user account.
- NTP-synchronized timestamps, so the history can be sorted by real time.
- Automatic recovery from an I2C bus hang on the sensor side.

---

## System Overview

### Hardware Components

| Component | Function | Pin / Address |
|-----------|----------|---------------|
| ESP32-C3 Super Mini | Oximeter unit controller | SDA GPIO8, SCL GPIO9 |
| MAX30102 | PPG sensor (red and infrared LEDs) | I2C `0x57` |
| ESP32-S3 | Kit box controller (dual-core) | - |
| 2.8" TFT ILI9341 with touch controller | Result display and YES / NO input | SPI: MISO 13, MOSI 11, SCLK 12, CS 10, DC 7, RST 6, T_CS 16 |
| Li-ion battery (3500 mAh) | Power source of the kit box | - |
| TP4056 | Battery charging with basic protection | - |
| MT3608 step-up converter | Raises the battery voltage for the ESP32-S3 and the TFT | - |
| 2 x 1 kΩ resistors (R1, R3) | Voltage divider for battery monitoring | ADC on GPIO5 |
| Boot button | Generates a new Kit Code when held for 3 seconds | GPIO0 |

The oximeter unit is powered directly from its own battery through a two-pin connector, with no extra regulation circuit, to keep the probe as small and light as possible. The INT, IRD, and RD pins of the MAX30102 are left unconnected, because the firmware polls the sensor over I2C instead of using its interrupt.

### Block Diagram

```
 OXIMETER UNIT                         KIT BOX                              CLOUD
┌────────────────────┐  ESP-NOW  ┌──────────────────────┐  WiFi   ┌──────────────────┐
│ MAX30102           │           │ ESP32-S3             │         │ Firebase         │
│   ▲ │ I2C          │ one-way   │   ▲ │ SPI            │ one-way │ Firestore        │──▶ Mobile app
│   │ ▼              │──────────▶│   │ ▼                │────────▶│                  │
│ ESP32-C3           │           │ TFT 2.8" + touch     │         └──────────────────┘
└─────────▲──────────┘           └─────────▲────────────┘
          │                                │
       Battery                  Battery (TP4056 + MT3608)
```

### Data Flow

A measurement passes through five stages, and each stage only forwards data that has been accepted by the stage before it.

1. The MAX30102 captures red and infrared light reflected from the fingertip.
2. The ESP32-C3 buffers, filters, and validates the signal, then calculates BPM and SpO2.
3. A valid and stable result is sent to the ESP32-S3 with ESP-NOW.
4. The ESP32-S3 shows the result and asks the user whether it should be saved.
5. Only after the user taps YES is the result written to Firestore, where the mobile application reads it in real time.

---

## Repository Structure

```
halohealth-oximeter/
├── .gitignore
├── README.md
├── ESP32C3-COMMS/                  # Oximeter unit (sensing)
│   ├── platformio.ini
│   └── src/
│       ├── main.cpp                # main flow, filtering, ESP-NOW sending
│       ├── max30102.cpp / .h       # low-level I2C driver for the sensor
│       ├── algorithm_by_RF.cpp / .h# BPM and SpO2 algorithm
│       ├── secrets.example.h       # template for local settings
│       └── secrets.h               # your own settings (not committed)
└── ESP32S3_Coms/                   # Kit box (display and cloud)
    ├── platformio.ini
    └── src/
        ├── main.cpp                # UI, RTOS tasks, Firebase, ESP-NOW receiver
        ├── NTPTime.cpp / .h        # NTP time and Firestore timestamp
        ├── RandomID.cpp / .h       # Kit Code stored in EEPROM
        ├── secrets.example.h       # template for local settings
        └── secrets.h               # your own settings (not committed)
```

### Module Descriptions

**ESP32C3-COMMS (oximeter unit)**

| Module | Description |
|--------|-------------|
| `main.cpp` | Sets up WiFi channel and ESP-NOW, initializes the sensor, reads the FIFO, applies validation and smoothing, and sends the result every 10 seconds when the data is stable. |
| `max30102.cpp / .h` | Single-register read and write over I2C, and reading one FIFO sample (6 bytes: 3 for red and 3 for infrared, combined into two 18-bit values). The interrupt status registers are cleared before every read. |
| `algorithm_by_RF.cpp / .h` | Calculates BPM and SpO2 from the 100-sample buffers (see the algorithm section below). |

**ESP32S3_Coms (kit box)**

| Module | Description |
|--------|-------------|
| `main.cpp` | Draws the interface, runs the state machine, receives ESP-NOW data, looks up the user, and writes to Firestore from a dedicated FreeRTOS task. |
| `NTPTime` | Synchronizes time with `pool.ntp.org` (fallback `time.nist.gov`) for the WIB time zone (UTC+7), and produces the on-screen time and the UTC timestamp for Firestore. |
| `RandomID` | Generates a 4-digit Kit Code (1000 to 9999) and keeps it in EEPROM using a magic flag, so the code survives a restart. |

---

## Software Design

### Oximeter Unit (ESP32-C3)

**Initialization (`setup`)**

1. The CPU frequency is lowered to 80 MHz and Bluetooth is turned off to save power.
2. WiFi is set to station mode, the router channel is found by scanning for the configured SSID, and the radio is locked to that channel.
3. The radio transmit power is set to its maximum, and ESP-NOW is initialized with a send callback.
4. The ESP32-S3 is registered as an ESP-NOW peer by its MAC address (no encryption).
5. The I2C bus is started at 400 kHz and the MAX30102 is configured for SpO2 mode.

**Main loop**

1. **Watchdog.** Every second the sensor address is checked on the I2C bus. If it does not respond, the bus, the sensor, and all buffered state are reinitialized.
2. **FIFO reading.** The write and read pointers are compared and all waiting samples are read.
3. **Finger detection.** A sample with an infrared value below 50000 counts as "no finger". After more than 50 such samples in a row, the buffers and filters are reset.
4. **Buffering.** Valid samples go into 100-sample rolling buffers (about 4 seconds at 25 Hz).
5. **Calculation.** When the buffers are full and 10 new samples have arrived, BPM and SpO2 are recalculated.
6. **Validation and smoothing.** Results are accepted or rejected as described in the next section.
7. **Sending.** When BPM and SpO2 are both valid and stable and at least 10 seconds have passed since the last transmission, the result is sent to the ESP32-S3.

Sensor configuration used in the firmware: mode SpO2 (red and infrared), 100 samples per second with 4-sample averaging (effective rate of 25 samples per second), pulse width 411 µs, ADC range 16384 nA, with FIFO rollover enabled.

### Kit Box (ESP32-S3)

**Initialization (`setup`)**

1. The Kit Code is read from EEPROM, or a new one is generated if the EEPROM has never been written.
2. The TFT is initialized, and the FreeRTOS semaphore (`xSemKirim`) and mutex (`xMutexUID`) are created.
3. The board connects to WiFi (restarts after 20 seconds without a connection), then starts ESP-NOW.
4. Time is synchronized with NTP.
5. The board signs in to Firebase (restarts after 20 seconds if Firebase is not ready).
6. The first user lookup is performed with the Kit Code.
7. The Firebase task is started on Core 0 and the dashboard is drawn.

**Core split**

| Core | Task | Responsibility |
|------|------|----------------|
| Core 1 | `loop()` | Touchscreen, display, battery reading, Boot button, serial commands, state machine |
| Core 0 | `taskFirebase()` | User lookup (retry every 5 seconds while the user is unknown) and Firestore writes |

The two cores are connected by a binary semaphore, which tells Core 0 that the user approved the data, and by a mutex that protects the shared `userID` string. The payload is copied into a shared structure before the semaphore is released, so Core 0 never reads a half-written payload. Core 0 yields for 10 ms at the end of every pass so that the WiFi stack running on the same core keeps working.

### User Interface State Machine

| State | Trigger | Action |
|-------|---------|--------|
| `S_IDLE` | New data arrives from the oximeter | Show BPM, SpO2, and status, then move to `S_KONFIRMASI` |
| `S_KONFIRMASI` | User taps the send button | Copy the payload, release the semaphore, move to `S_SENDING` |
| `S_KONFIRMASI` | User taps the cancel button | Discard the result and return to `S_IDLE` |
| `S_SENDING` | Core 0 reports that it is done | Show the result screen and move to `S_RESULT` |
| `S_RESULT` | 2 seconds have passed | Return to `S_IDLE` |

### Resource Optimization

- The oximeter runs at 80 MHz with Bluetooth disabled, since it only polls I2C and occasionally transmits.
- Transmission is limited to once every 10 seconds, so ESP-NOW and the confirmation screen are not flooded.
- The Firebase work is isolated on Core 0, so the touchscreen never waits for a network call.

---

## BPM and SpO2 Algorithm

The calculation runs on the ESP32-C3 using `algorithm_by_RF` and the validation logic in `main.cpp`.

1. **Preprocessing.** The slow drift (DC component and linear trend) is removed from the red and infrared buffers.
2. **Correlation check.** The correlation between the red and infrared signals confirms that both come from the same pulse. A reading is rejected when the correlation is below 0.78.
3. **BPM.** The heartbeat interval is estimated with autocorrelation and refined with parabolic interpolation. A BPM value is accepted only when it is marked valid and lies between 30 and 220.
4. **SpO2.** The saturation is computed from the AC/DC amplitude ratio of the red and infrared signals with an empirical polynomial. A value is accepted only when the BPM was accepted and the value lies between 70 and 100.
5. **Smoothing.** Accepted BPM and SpO2 values pass through a median-of-3 filter and then an exponential moving average with a factor of 0.3.
6. **Status.** The SpO2 value is classified before sending.

| SpO2 | Status sent (as stored in the database) |
|------|-----------------------------------------|
| 95 % or higher | `Normal` |
| 90 % to below 95 % | `Rendah` (low) |
| Below 90 % | `Kritis` (critical) |

### ESP-NOW Payload

| Field | Type | Description |
|-------|------|-------------|
| `bpm` | `int` | Smoothed heart rate |
| `saturasi` | `float` | Smoothed SpO2 |
| `status` | `char[20]` | Status text |
| `dataReady` | `bool` | Marks the packet as a valid result |

---

## Kit Code and User Pairing

Each kit box has a 4-digit Kit Code that is shown at the top of the dashboard.

1. The user types the Kit Code into the HaloHealth mobile application during registration, and the application stores it in the `kodeKit` field of the user document.
2. The ESP32-S3 queries the `users` collection for the document whose `kodeKit` equals its own Kit Code and keeps the document ID as the user ID. The dashboard shows `Terhubung` once a user has been found.
3. To generate a new Kit Code, hold the Boot button for 3 seconds or send `NEWID` over the serial monitor. The stored user ID is cleared and a new lookup begins.
4. Send `STATUS` over the serial monitor to print the Kit Code, the user ID, the time, the NTP state, the free heap, and the remaining stack of the Firebase task.

---

## Firestore Data Structure

Each confirmed measurement becomes a new document, so the history keeps growing instead of being overwritten.

```
users/{uid}
    kodeKit        : string      (created by the mobile application)
    lastRiwayatID  : string      (updated by the kit after every new reading)

users/{uid}/alat/Oximeter/riwayat/{autoId}
    bpm            : integer
    saturasi       : integer
    statusSaturasi : string
    waktu          : timestamp   (UTC, from the NTP-synchronized clock)
```

The `users/{uid}` document also holds the profile fields created by the mobile application (name, email, gender, phone, age, and creation time).

---

## Display

The dashboard of the kit box shows the following information.

```
Connected      HaloHealth      84%
Kode Kit  7030          Terhubung
DETAK JANTUNG   |   SATURASI O2
     82         |       99
bpm             |        %
Status: Normal
2026-7-4 12:23
MAC: xx:xx:xx:xx:xx:xx | CH: 1 | UID: xxxxxx
```

| Area | Content |
|------|---------|
| Top bar | Connection label, title, and battery percentage |
| Kit bubble | Kit Code and user lookup status |
| Card | BPM and SpO2 values |
| Info bar | Status text (green for Normal, yellow for Rendah, red for Kritis) and measurement time |
| Footer | MAC address, WiFi channel, and the first 6 characters of the user ID |

The battery percentage is estimated from the divided battery voltage on GPIO5, mapped from 2.5 V (0 %) to 4.4 V (100 %).

---

## Configuration

### Local Settings (`secrets.h`)

Credentials are kept out of the repository. Each project has a `secrets.example.h` template that you copy to `secrets.h` and fill in.

| Project | Macro | Description |
|---------|-------|-------------|
| `ESP32C3-COMMS` | `WIFI_SSID` | Name of the WiFi network. It must be the same network used by the ESP32-S3, because the C3 follows its channel. |
| `ESP32S3_Coms` | `WIFI_SSID`, `WIFI_PASSWORD` | WiFi network used by the kit box |
| `ESP32S3_Coms` | `API_KEY`, `DATABASE_URL`, `PROJECT_ID` | Firebase project settings |
| `ESP32S3_Coms` | `USER_EMAIL`, `USER_PASSWORD` | Firebase Authentication account used by the kit box |

The ESP32-C3 also needs the MAC address of the ESP32-S3. It is shown in the footer of the dashboard and is set in `s3Address` at the top of `ESP32C3-COMMS/src/main.cpp`.

### Main Parameters

| Parameter | File | Default | Description |
|-----------|------|---------|-------------|
| `FINGER_THRESHOLD` | `ESP32C3-COMMS/src/main.cpp` | 50000 | Minimum infrared value for a valid finger contact |
| `SLIDE_STEP` | `ESP32C3-COMMS/src/main.cpp` | 10 | New samples needed before each recalculation |
| `SMOOTH_ALPHA` | `ESP32C3-COMMS/src/main.cpp` | 0.3 | Smoothing factor |
| `MIN_CORREL_QUALITY` | `ESP32C3-COMMS/src/main.cpp` | 0.78 | Minimum red/infrared correlation |
| `SPO2_MIN_VALID`, `SPO2_MAX_VALID` | `ESP32C3-COMMS/src/main.cpp` | 70 and 100 | Accepted SpO2 range |
| `KIRIM_INTERVAL` | `ESP32C3-COMMS/src/main.cpp` | 10000 | Minimum time between ESP-NOW transmissions (ms) |
| `ALAT` | `ESP32S3_Coms/src/main.cpp` | `Oximeter` | Device name used in the Firestore path |
| `USERID_RETRY_INTERVAL` | `ESP32S3_Coms/src/main.cpp` | 5000 | Interval of the user lookup retry (ms) |
| `WIFI_CONNECT_TIMEOUT` | `ESP32S3_Coms/src/main.cpp` | 20000 | WiFi timeout before restart (ms) |
| `FIREBASE_READY_TIMEOUT` | `ESP32S3_Coms/src/main.cpp` | 20000 | Firebase timeout before restart (ms) |
| `HOLD_DURATION` | `ESP32S3_Coms/src/main.cpp` | 3000 | Hold time of the Boot button for a new Kit Code (ms) |
| `TOUCH_DEBOUNCE_MS` | `ESP32S3_Coms/src/main.cpp` | 300 | Touch debounce time (ms) |

`BUFFER_SIZE` (100) in the oximeter firmware matches the window used by the algorithm. Change it only together with the algorithm constants.

---

## Build and Upload

The project uses [PlatformIO](https://platformio.org/), best used through the PlatformIO IDE extension for VS Code. Run the commands from the repository root.

**1. Prepare the local settings**

```powershell
Copy-Item ESP32S3_Coms/src/secrets.example.h ESP32S3_Coms/src/secrets.h
Copy-Item ESP32C3-COMMS/src/secrets.example.h ESP32C3-COMMS/src/secrets.h
```

Then open both `secrets.h` files and fill in your own values.

**2. Prepare Firebase**

1. Create a Firebase project and enable Cloud Firestore.
2. Enable Email/Password in Firebase Authentication and create one account for the kit box.
3. Copy the web API key, the database URL, and the project ID into `ESP32S3_Coms/src/secrets.h`.
4. Set the Firestore security rules so the kit account can only read the `users` collection and write the `riwayat` entries it needs.

**3. Build and upload**

```bash
# Kit box (ESP32-S3)
pio run -d ESP32S3_Coms -t upload

# Oximeter unit (ESP32-C3)
pio run -d ESP32C3-COMMS -t upload

# Serial monitor (115200 baud)
pio device monitor -d ESP32S3_Coms
```

Upload the kit box first, read its MAC address from the dashboard, put it in `s3Address` of the oximeter firmware, and then upload the oximeter.

**PlatformIO environments**

| Project | Environment | Board |
|---------|-------------|-------|
| `ESP32C3-COMMS` | `esp32-c3-devkitm-1` | ESP32-C3 (USB CDC on boot enabled) |
| `ESP32S3_Coms` | `esp32s3box` | ESP32-S3 (pioarduino platform 54.03.20) |

Libraries used by the kit box: `TFT_eSPI` (2.5.43 or newer), `Firebase Arduino Client Library for ESP8266 and ESP32` (4.4.17 or newer), and `Adafruit NeoPixel`. The TFT pin assignment is defined in the build flags of `ESP32S3_Coms/platformio.ini`.

---

## Test Results

### Pipeline Test

The Serial Monitor was used to confirm each part in isolation: sensor detection and reasonable BPM and SpO2 values on the oximeter, and WiFi, ESP-NOW reception, and NTP synchronization on the kit box. The Firebase Console was used to confirm that every confirmed measurement appeared under `users/{uid}/alat/Oximeter/riwayat` with the correct fields, and that the mobile application listed it in its oximeter history.

### Comparison with a Smartwatch

The oximeter was compared with a commercial smartwatch in 7 readings.

<!-- TODO: confirm the unit of this table (BPM or SpO2) against the original measurement notes before publishing. -->

| No | Smartwatch | HaloHealth Oximeter |
|----|------------|---------------------|
| 1 | 95 | 99 |
| 2 | 96 | 99 |
| 3 | 95 | 99 |
| 4 | 94 | 99 |
| 5 | 95 | 99 |
| 6 | 93 | 99 |
| 7 | 92 | 99 |

The smartwatch values changed between 92 and 96, while the oximeter stayed at 99. The most likely cause is the casing. A large gap between the finger and the photodetector lets ambient light and direct LED reflection reach the sensor, so the signal is not modulated by the blood pulse and looks like a flat baseline. The signal then passes the validation checks while being inaccurate. This shows that a stable reading is not automatically a correct reading, and that the physical placement of the sensor matters as much as the firmware.

---

## Limitations and Future Work

- ESP-NOW only works when both boards are on the same WiFi channel as the router used by the ESP32-S3. If the router changes its channel, the link can break silently until both boards are restarted.
- The ESP-NOW link uses a fixed MAC address and no encryption. Pairing between a kit and an account relies only on the Kit Code and on Firebase Authentication.
- Neither board has a battery-backed real-time clock, so timestamps depend on a successful NTP synchronization at boot.
- Finger presence is judged only from the infrared threshold, so a loosely placed finger can reset the buffer in the middle of a measurement.
- A reading that fails to reach Firestore is not queued or retried. The result screen shows the error and the measurement must be repeated.
- The measurement accuracy under real conditions is not yet reliable because of the optical gap in the casing described above.
- The flag `sudahKirim` in the oximeter firmware is reset but never used and can be removed or integrated in a later revision.
- Only the oximeter is implemented. The system is meant for personal and indicative monitoring and does not replace a clinically calibrated pulse oximeter.

Suggested improvements are as follows.

1. Redesign the casing so that the finger sits firmly against the sensor and ambient light is blocked, then repeat the comparison with a reference device.
2. Read the MAX30102 through its INT pin instead of continuous polling, which opens the way to sleep modes on the ESP32-C3.
3. Merge sensing, display, and Firebase connectivity into one ESP32-S3 and remove ESP-NOW, if a single board has enough resources.
4. Add the other health tools, starting with a digital thermometer and then a blood pressure monitor.

---

## Troubleshooting

**The serial monitor shows `[MAX30102] ERROR: Sensor tidak ditemukan!`.** The sensor does not answer at address `0x57`. Check the SDA (GPIO8) and SCL (GPIO9) wiring, the power supply of the sensor, and any loose connector.

**The oximeter keeps printing `Mengumpulkan data...` or `Sinyal tidak stabil...`.** The buffer is still filling or the signal is rejected. Place the finger firmly on the sensor, keep it still, and block ambient light.

**The kit box never receives data (`ESP-NOW: Gagal kirim ke S3!`).** Check that `s3Address` matches the MAC address shown on the kit box, and that `WIFI_SSID` is identical in both projects. The oximeter prints `Router tidak ditemukan` when it cannot find the network, and in that case it keeps the default channel.

**The kit box restarts at boot.** It restarts when WiFi is not connected within 20 seconds or Firebase is not ready within 20 seconds. Check the WiFi credentials, the internet connection, and the Firebase settings in `secrets.h`.

**The dashboard stays at `Mencari user...`.** No user document has a `kodeKit` equal to the Kit Code on the screen. Register the Kit Code in the mobile application, or generate a new code by holding the Boot button for 3 seconds.

**The result screen shows `Gagal Kirim!`.** Read the error message on the screen and in the serial monitor. Typical causes are an unknown user ID, a Firebase connection that is not ready, or Firestore security rules that reject the write.

**The time is not shown correctly.** The NTP synchronization failed. Check the internet connection and restart the kit box.

**The screen is blank or the touch does not respond.** Check the SPI wiring and the pins in the build flags of `ESP32S3_Coms/platformio.ini`, including the separate `T_CS` line of the touch controller.

---

## Credits and References

- The BPM and SpO2 algorithm is based on R. Fraczkiewicz, [MAX30102_by_RF](https://github.com/aromring/MAX30102_by_RF).
- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) by Bodmer, and [Firebase-ESP-Client](https://github.com/mobizt/Firebase-ESP-Client) by Mobizt.
- Espressif Systems documentation for ESP-NOW and FreeRTOS on the ESP32.
- Nitzan, M., Romem, A., and Koppel, R. (2014). Pulse oximetry: Fundamentals and technology update. *Medical Devices: Evidence and Research*, 7, 231 to 239.
- Abdulmalek, S., et al. (2022). IoT-Based Healthcare-Monitoring System towards Improving Quality of Life: A Review. *Healthcare*, 10(10), 1993.
