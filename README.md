# XIAO ESP32-C3 BMP280 temperature sender

This Seeed Studio XIAO ESP32-C3 firmware supports both Kitchen and Living Room.
It reads a BMP280 and advertises the Fahrenheit value over BLE. Sensor readings
are not posted over Wi-Fi. The signed Wi-Fi OTA path remains as a recovery
mechanism, and the device enters deep sleep between readings.

Each timer wake is a fresh boot: the firmware reads the sensor once, advertises
the reading over BLE, keeps a connection window open for the Pi, checks the
signed Wi-Fi OTA recovery path, turns off Wi-Fi and I2C, and sleeps again. The
default interval is 30 seconds.

## Wiring

| BMP280 | XIAO ESP32-C3 |
| --- | --- |
| VIN/VCC | 3V3 |
| GND | GND |
| SDA/SDI | D4 / GPIO6 |
| SCL/SCK | D5 / GPIO7 |
| CSB | 3V3 |
| SDO | GND |

SDO connected to ground selects I2C address `0x76`.

## Build and upload

Open this directory as its own PlatformIO project. Build the room explicitly:

```sh
cp include/secrets.example.h include/secrets.h
# Edit include/secrets.h with the local Wi-Fi and Pi settings.

pio run -e kitchen
pio run -e living_room
pio run -e kitchen --target upload
pio device monitor
```

### Phase 1 BLE advertisement diagnostic

The isolated `ble_phase1_kitchen` environment compiles a name-only BLE radio
test advertising `XIAO-BLE-TEST` for 60 seconds at a time. It does not initialize
the BMP280, submit sensor readings, or enter deep sleep. It retains signed Wi-Fi
OTA after the first 60-second window so it can be installed and recovered
remotely. Build and sign it for publishing as version `ble-radio-test-1`, target
`Kitchen`:

```sh
~/.platformio/penv/bin/platformio run -e ble_phase1_kitchen
```

On the Pi, stop the dashboard scanner and start the probe before resetting the
XIAO:

```sh
sudo systemctl stop esp-temp-display
sudo -u pi .venv/bin/python ble_probe.py --seconds 90
sudo systemctl start esp-temp-display
```

The diagnostic restarts the name-only advertisement in 60-second intervals and
polls signed Wi-Fi OTA every 30 seconds after the first window. Restore a signed
`temperature-*` Kitchen release after the probe succeeds.

USB serial disconnects while the XIAO is in deep sleep and reconnects when it
wakes. A serial monitor may therefore need to be restarted after each wake;
the receiving server is the primary deployment health check.

Edit the ignored `include/secrets.h` to change Wi-Fi, server, or authentication
settings. Never commit that file.
The `kitchen` and `living_room` PlatformIO environments set the room at build
time so a binary cannot be mislabeled by changing only the publish target.

## Signed OTA releases

The firmware retains the full temperature workflow and checks for a signed
Wi-Fi OTA recovery release after the BLE window, immediately before deep sleep.
Change `FIRMWARE_VERSION` in `include/ota_update.h` for every release.

Build, sign, and verify each room separately on the development computer:

```sh
cd ~/Documents/PlatformIO/Projects/XIAO_ESP32C3_Temp

~/.platformio/penv/bin/platformio run -e kitchen -e living_room

mkdir -p releases/temperature-7/kitchen releases/temperature-7/living_room
cp .pio/build/kitchen/firmware.bin releases/temperature-7/kitchen/firmware.bin
cp .pio/build/living_room/firmware.bin \
  releases/temperature-7/living_room/firmware.bin

openssl dgst -sha256 \
  -sign ../XIAO_ESP32C3_OTA_Bootstrap/keys/ota_private.pem \
  -out releases/temperature-7/kitchen/firmware.sig \
  releases/temperature-7/kitchen/firmware.bin
openssl dgst -sha256 \
  -sign ../XIAO_ESP32C3_OTA_Bootstrap/keys/ota_private.pem \
  -out releases/temperature-7/living_room/firmware.sig \
  releases/temperature-7/living_room/firmware.bin
```

Verify each signature before publishing. Each binary must contain
`temperature-7` and exactly its intended room. Publish the two room directories
one at a time using the matching target; never reuse one room's binary for the
other room.


If plugged into the serial monitor, then run
```
~/.platformio/penv/bin/platformio device monitor --baud 115200
```


To manually upload
```
~/.platformio/penv/bin/platformio run -e kitchen --target upload --upload-port /dev/ttyACM0
```
