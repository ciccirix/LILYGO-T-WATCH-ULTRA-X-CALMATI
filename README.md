# LILYGO T-Watch Ultra × CALMATI

**Custom firmware for the LilyGo T-Watch Ultra (ESP32-S3).** A smartwatch plus a full suite of RF, WiFi, BLE and NFC tools — reached from swipe gestures, the side buttons, and the on-screen **Tools** grid.

[![build](https://github.com/ciccirix/LILYGO-T-WATCH-ULTRA-X-CALMATI/actions/workflows/build.yml/badge.svg)](https://github.com/ciccirix/LILYGO-T-WATCH-ULTRA-X-CALMATI/actions/workflows/build.yml)

⬇️ **[Download the latest flashable build](https://github.com/ciccirix/LILYGO-T-WATCH-ULTRA-X-CALMATI/releases/latest)** — flash the merged `.bin` at offset `0x0` (esptool or any ESP Web Tools / esptool-js web flasher). Rebuilt automatically on every push.

---

## Previews

| Threat Radar | pwnpet | pwnpet (live) |
| :---: | :---: | :---: |
| <img src="img/threat_radar.png" width="240" alt="Threat Radar"> | <img src="img/pwnpet.png" width="240" alt="pwnpet"> | <img src="img/pwnpet_loop.gif" width="240" alt="pwnpet swimming"> |

**Watch face & system**

| Clock | Time | Settings | Settings |
| :---: | :---: | :---: | :---: |
| ![Clock](img/Clock.bmp) | ![Time](img/Time.bmp) | ![Settings](img/Settings1.bmp) | ![Settings](img/Settings2.bmp) |

**Radios**

| WiFi | Bluetooth | LoRa | GPS | NFC |
| :---: | :---: | :---: | :---: | :---: |
| ![WiFi](img/WiFi_Radio.bmp) | ![Bluetooth](img/Bluetooth_Radio.bmp) | ![LoRa](img/LORA_Radio.bmp) | ![GPS](img/GPS_Radio.bmp) | ![NFC](img/NFC_Radio.bmp) |

**Tools grid**

| Tools | Tools | Tools | Tools |
| :---: | :---: | :---: | :---: |
| ![Tools](img/Tools1.bmp) | ![Tools](img/Tools2.bmp) | ![Tools](img/Tools3.bmp) | ![Tools](img/Tools4.bmp) |

**Meshtastic & wardriving**

| Messages | Nodes | Send | Map |
| :---: | :---: | :---: | :---: |
| ![Messages](img/Meshtastic_Messages.bmp) | ![Nodes](img/Meshtastic_Nodes.bmp) | ![Send](img/Meshtastic_Send.bmp) | ![Map](img/Meshtastic_Map.bmp) |

| Configuration | Configuration | Wardriver |
| :---: | :---: | :---: |
| ![Configuration](img/Meshtastic_configuration1.bmp) | ![Configuration](img/Meshtastic_Configuration2.bmp) | ![Wardriver](img/Wardriver.bmp) |

---

## Features

**Smartwatch**
- Clock — analog / digital face, battery, live LoRa · BT · WiFi · SD · NFC status, detector badges
- Matrix animated wallpaper
- Settings — brightness, faces, 12/24h, dim, haptics, motion-wake, manual time
- TIME hub — Alarm · Stopwatch · Timer · Calendar
- Screenshot — long-press capture to SD

**Meshtastic & LoRa**
- Meshtastic — RX/decrypt, messages, DMs, per-packet RSSI/SNR/hops
- Nodes — heard-node list, traceroute, position request
- Send Message — presets, compose, DM with ACK
- Map — SD-tile slippy map with peer nodes
- LoRa radio status · LoRa APRS · ESP-NOW

**Navigation & Radios**
- GPS — fix, satellites, auto timezone (DST-aware)
- Per-radio status screens — WiFi · Bluetooth · LoRa · GPS · NFC

**NFC**
- Read (ISO 14443 / 15693) · Write NDEF (text / URL / phone)
- Mifare · Credito

**WiFi tools**
- Scanner (unified) · WiFi survey + ping sweep · Port Scanner
- Wardriver (WiGLE CSV + GPS) · Evil Twin detector
- Deauth · ARP MitM · Phantom · Panel (web) · Auto

**BLE tools**
- AirTag · Flipper · Skimmers detectors
- BLE Audit · GATT explorer · Drone ID · RID Spoof
- Meta (smart-glasses) · Mouse (BLE HID)

**Sub-GHz / RF**
- TPMS (433 MHz) · Pager (POCSAG / FLEX) · Tesla CP
- Waterfall · Analyze (WiFi/BT/LoRa spectrum) · Sentinel · ADS-B

**Threat Radar — anti-stalking**
- Correlates AirTag / Flipper / Skimmer / Flock / Evil-Twin detections with GPS over time to flag devices **co-moving with you** (Possible → Likely → Confirmed), with haptic alert and mesh reputation sharing. Counter-tail extends the same test to vehicles.

**pwnpet**
- Tamagotchi-style goldfish that meets Pwnagotchi units, reacts to tails, and eats captured WPA handshakes / PMKIDs (passive, authorized testing only).

**System & misc**
- Task Mgr · Duress · USB SD (mass storage) · Download
- Mic Rec · Assist · Calmati · UDDA · Cameras · Printer

---

## Credits & License

Fork of **[r3dfish/13-37](https://github.com/r3dfish/13-37)** — MIT. See [LICENSE](LICENSE) and third-party notices in-tree.

**Responsible use:** the RF / WiFi / BLE tools are for education and authorized security testing on hardware and networks you own or have permission to test. You are responsible for complying with local regulations.
