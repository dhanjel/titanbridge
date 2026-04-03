# TitanBridge

ESP32-C6 (Seeed XIAO ESP32-C6) firmware that bridges a Titan treadmill to a Garmin watch and Home Assistant via Zigbee.

## What it does

- Connects to the treadmill as a **BLE client** (FTMS, reads proprietary Training Status notifications)
- Exposes itself as a **BLE RSC sensor** (Running Speed & Cadence, 0x1814) so a Garmin watch can connect
- Reports speed, running state, and Garmin-connected state to **Home Assistant via Zigbee**

## Hardware

- **Board**: Seeed XIAO ESP32-C6
- **LED**: pin 15, active LOW (no RGB)
- **Treadmill BLE address**: `c8:1f:c2:2a:90:40` (public, hardcoded in `TREADMILL_ADDR`)

## Build & flash

```bash
bin/arduino-cli compile --fqbn "esp32:esp32:XIAO_ESP32C6:ZigbeeMode=ed,PartitionScheme=zigbee" titanbridge
bin/arduino-cli upload  --fqbn "esp32:esp32:XIAO_ESP32C6:ZigbeeMode=ed,PartitionScheme=zigbee" --port /dev/ttyACM0 titanbridge
```

**Both** FQBN options are required — `ZigbeeMode=ed` and `PartitionScheme=zigbee`. Using the default partition scheme causes a boot crash.

Monitor serial at 115200 baud:
```bash
stty -F /dev/ttyACM0 115200 raw && cat /dev/ttyACM0
```

## LED states

| Pattern | Meaning |
|---------|---------|
| Solid ON | Both Garmin and treadmill connected |
| Slow blink (500 ms) | Garmin connected, treadmill not |
| Fast blink (200 ms) | Neither connected |

## BLE architecture

The ESP32-C6 has one shared 2.4 GHz radio. Key constraint: **`pClient->connect()` is blocking and prevents Garmin from connecting simultaneously**. The treadmill task therefore:

1. Stops BLE advertising before calling `connect()`
2. Resumes advertising immediately after (success or failure)
3. Garmin can connect freely once treadmill is established

On treadmill disconnect the client is set to `nullptr` and recreated fresh — reusing a stale client after disconnect causes reconnect failures.

## Speed data source

The treadmill sends speed via **Training Status characteristic (0x2ACD)**, not via the standard FTMS Treadmill Data (0x2AD3). The 0x2AD3 notifications only arrive with the "More Data" flag set (speed absent) and do not carry real-time speed.

The 0x2ACD packet is proprietary, 19 bytes, sent every ~0.5 s:

```
[0]    0x8c  — proprietary flags
[1]    0x05  — training status code (e.g. Recovery Interval)
[2-3]  uint16 LE — actual belt speed, 0.01 km/h units
[4-5]  uint16 LE — segment target speed, 0.1 km/h units (workout program)
[6-10] varies — other workout parameters
[11]   segment/interval counter
[12]   0x00
[13-15] 0xff 0xff 0xff
[16]   0x00
[17-18] uint16 LE — elapsed time in seconds
```

Speed formula: `speed_kmh = (d[2] | d[3]<<8) / 100.0`

Note: bytes 4-5 is the *scheduled target* for the current workout interval, not the actual belt speed. These differ when the treadmill is running a pre-programmed workout.

## Zigbee endpoints

| EP | Type | Cluster | HA entity | Unit |
|----|------|---------|-----------|------|
| 10 | ZigbeeWindSpeedSensor | Wind Speed (0x040C) | `sensor.titanbridge_treadmillspeed` | m/s |
| 11 | ZigbeeOccupancySensor | Occupancy (0x0406) | `binary_sensor.titanbridge_treadmillrunning` | bool |
| 12 | ZigbeeOccupancySensor | Occupancy (0x0406) | `binary_sensor.titanbridge_garminconnected` | bool |

`ZigbeeWindSpeedSensor::setWindSpeed()` takes **m/s** (not 0.01 m/s) — it multiplies by 100 internally.

## Garmin RSC profile

The bridge advertises as a Running Speed & Cadence sensor (service 0x1814, appearance 0x0440).

RSC Measurement packet (8 bytes):
- Byte 0: flags — bit 1 = Total Distance Present, bit 2 = Running (vs Walking)
- Bytes 1-2: speed, 1/256 m/s units
- Byte 3: cadence (estimated as `140 + speed_kmh * 3`)
- Bytes 4-7: total distance, 1/10 m units

The SC Control Point (0x2A55) responds to opcode 0x04 (Supported Sensor Locations) which Garmin requires during pairing.

## First-time setup / re-flashing

If flashing a new partition scheme, erase flash first to clear stale NVS:
```bash
~/.arduino15/packages/esp32/tools/esptool_py/5.1.0/esptool --port /dev/ttyACM0 erase_flash
```

After erase, Zigbee needs re-pairing in Home Assistant (Settings → Devices → Add Device).
