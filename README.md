# StratoLink — Stratospheric LoRa Image & Telemetry Link (RPi + E32)

LoRA-based image downlink solution for stratospheric baloon missions. Raspberry Pi + E32-900T30S EBYTE module over UART. Developed for a stratoshperic baloon payload made by the  Space Technology Centre at AGH Univeristy.

StratoLink is a Raspberry Pi–based link for **sending photos and telemetry from a stratospheric balloon** over **LoRa** using **EBYTE E32** transceiver modules.  
The system runs in **transparent transmission mode** with a **58-byte packet limit**, synchronized by the module’s **AUX** pin, and is configured for **21 dBm TX power** (to protect the flight antenna).

> ✅ This README consolidates and expands setup details, wiring, software usage, protocol notes (file transfer header + CRC-16/XMODEM), and troubleshooting guidance for real-world flight operations.

---

## Table of Contents

- [Project Overview](#project-overview)
- [Hardware](#hardware)
- [Power & RF safety](#power--rf-safety)
- [Wiring (GPIO / UART)](#wiring-gpio--uart)
- [Raspberry Pi system configuration](#raspberry-pi-system-configuration)
- [Dependencies & installation](#dependencies--installation)
- [Repository layout](#repository-layout)
- [LoraE32 class — API overview](#lorae32-class--api-overview)
- [Class and Method Overview](#class-and-method-overview)
- [Transparent transmission rules (E32)](#transparent-transmission-rules-e32)
- [File/photo transfer protocol](#filephoto-transfer-protocol)
- [Scripts](#scripts)
  - [`burner.py` — RF “burner”](#burnerpy--rf-burner)
  - [`photo.py` — take a photo each minute & send](#photopy--take-a-photo-each-minute--send)
- [Program Menu](#program-menu) 
- [Sending Commands in HEX Mode](#sending-commands-in-hex-mode)  
- [Ground-station basics](#ground-station-basics)
- [Troubleshooting (no RF / flaky link)](#troubleshooting-no-rf--flaky-link)
- [Regulatory notice](#regulatory-notice)
- [Links and Documentation](#links-and-documentation)  

---

## Project Overview

StratoLink is a Python module (`loraE32` class) running on a Raspberry Pi that initializes and communicates with an **E32-900T30S** LoRa module (915 MHz band). By default:

- Sets RF power to **21 dBm** (~125 mW)
- Switches the module to **Normal mode** (TX/RX)
- Sends and receives data over UART with AUX pin synchronization

The code also includes helper methods to check pin states (M0, M1, AUX) and determine the operating mode.

---

## Hardware

- **EBYTE E32** in the ~900 MHz band (e.g., E32-9xxT30S / T30D). The code assumes **21 dBm** power (not 30 dBm) for antenna safety.
- **Raspberry Pi** (tested with UART + GPIO via RPi.GPIO).
- **Antenna** tuned for the chosen band/channel. **Never transmit without a proper antenna**.
- Wiring for UART and control pins **M0, M1, AUX**.
- Optional **level shifter** or resistor divider for E32 **TXD → Pi RXD** if your unit idles >3.3 V on TXD (many are 3.3 V-compatible; measure your module).

### Default pin mapping (BCM)

| Function | RPi Pin (BCM) |
| --- | --- |
| UART TXD (to E32 RXD) | 14 |
| UART RXD (from E32 TXD) | 15 |
| **M0** | 23 |
| **M1** | 24 |
| **AUX** (input) | 25 |

> You can change pins in code, but keep AUX as an **input** and M0/M1 as **outputs** with deterministic initial levels.

### Wiring Diagram

```text
Pi GPIO14 (TXD0) → E32 RXD  
Pi GPIO15 (RXD0) ← E32 TXD  
Pi GPIO23 → M0  
Pi GPIO24 → M1  
Pi GPIO25 ← AUX  
3.3 V → VCC  
GND → GND
```
> **Note:** While the E32 operates with TX levels around 4 V, it works without a level shifter with Pi — but proceed with caution.


---

## Power & RF safety

- Even at **21 dBm**, E32 can draw **large current spikes** on TX. Provide a **solid 5 V** rail with **≥1 A** headroom.
- Add **local decoupling** near the module: **100 nF + 10 µF + 470–1000 µF (low-ESR)**.
- Do **not** key the transmitter without a matched antenna. Protect people and electronics from RF exposure/overheating.

---

## Wiring (GPIO / UART)

- RPi **TXD (GPIO14)** → **E32 RXD**  
- RPi **RXD (GPIO15)** ← **E32 TXD** (use a divider if your E32 TXD is >3.3 V idle)  
- **M0/M1**: outputs from RPi to the module (0/1 logic levels)  
- **AUX**: input to RPi (module’s ready/busy indicator)

> Transparent transmission (Normal mode) requires **M0=0, M1=0**. Configuration (Sleep) uses **M0=1, M1=1**.

---

## Raspberry Pi system configuration

Disable the serial console and enable UART for userland:

1. Edit `/boot/config.txt`:
   ```ini
   enable_uart=1
   dtoverlay=disable-bt
   ```
2. Disable/get rid of login console on serial (`raspi-config` → Interface Options → Serial → “No” for login shell, “Yes” for serial port hardware).
3. Reboot. You should have `/dev/serial0` ready for the module.

---

## Dependencies & installation

System tools (for photos):
```bash
sudo apt update
sudo apt install -y fswebcam
# If your camera is not /dev/video0, pass -d /dev/videoX in code.
```

Python packages (inside venv recommended):
```bash
python3 -m venv .venv
source .venv/bin/activate
pip install --upgrade pip
pip install pyserial RPi.GPIO crc
```

---

## Repository layout

```
StratoLink/
├─ loraE32.py     # Core class to drive E32: modes, AUX sync, config, TX/RX, CRC, file send
├─ burner.py      # RF “burner”: sends a 58-byte 0x55 pattern in a loop (AUX-synchronized)
├─ photo.py       # Takes a photo every minute with fswebcam and sends it
├─ main.py        # (Optional) CLI / command dispatcher (list, send <file>, status, restart)
└─ README.md
```

---

## LoraE32 class — API overview

**Key features:**

- Safe **mode switching** (Normal ↔ Sleep) via **M0/M1**, with **AUX wait** and **post-mode delay**.
- Transparent TX with **58-byte chunk limit** and **AUX low→high** completion detection.
- Configuration via register frames in **Sleep** mode:  
  `C0` write, `C1` read params, `C3` version, `C4` restart.
- **Option byte** set to **0x47** by default → transparent mode, **21 dBm**, FEC on, push-pull, wake-up 500 ms.
- **SPED** byte default **0x1A** → UART 9600 8N1, air rate 2.4 kbps.
- **Channel** configurable (e.g., `0x06` → ≈ **850.125 MHz + 6** ≈ **856.125 MHz** on 900-series units).
- Optional **CRC-16/XMODEM** calculator for file payload integrity.
- Helper `take_photo(path="photo.jpg")` using `fswebcam` (you can change resolution, device, quality).

**Typical calls:**

```python
from loraE32 import LoraE32

radio = LoraE32(port="/dev/serial0", baudrate=9600, m0_pin=23, m1_pin=24, aux_pin=25)

# Configure module (Sleep -> C0 write -> C1 verify -> Normal)
radio.configure_module(channel=0x06)  # 21 dBm, transparent, 9600/2.4k

# Send data (split to 58-byte chunks, AUX-synchronized)
radio.send_data(b"hello world\n")

# Receive (packet-oriented with AUX; collects one or more packets until idle window)
data = radio.receive_data(timeout=2.0)

# Check current parameters (reads C1 and C3, then returns to Normal)
radio.check_parameters()

# Restart the module (C4) and return to Normal
radio.process_command("restart")

radio.close()
```

---

## Class and Method Overview

### `class loraE32`

- **`__init__()`**  
  Sets up GPIO pins, initializes UART, and immediately configures the module (sets power, channel, saves settings).

- **`configure_module()`**  
  Sends frame `C0 00 00 1A 06 47` — stores configuration (21 dBm, CH=0x06, 9600 8N1, 2.4 kbps).  
  Confirms by reading parameters (`C1 C1 C1`) and checking `resp[5] == 0x47`.  
  Returns to Normal mode.

- **`check_parameters()`**  
  Reads current parameters and module version (`C1 C1 C1` and `C3 C3 C3`), printing length and HEX data.

- **`check_mode()`**  
  Reads M0/M1 pin states and prints current operating mode: **Normal**, **Wake-up**, **Power-saving**, **Sleep**, or **Unknown**.

- **`send_data()` / `receive_data()`**  
  Sends/receives data in Normal mode, waits for **AUX=HIGH** before operations, splits into chunks (default **58 bytes**).

- **`process_command()`**  
  Simple command interpreter: `list` / `send <file>` / `status` / `restart` (module reset).

- **`send_data_with_crc()`**  
  Adds a header with data length and **CRC-XModem** before transmission.

- **`close()`**  
  Closes the UART port and cleans up GPIO.

---

## Transparent transmission rules (E32)

- **Packet limit:** In transparent mode the E32 transmits in **packets up to 58 bytes**. Larger payloads **must be chunked**.
- **AUX behavior:**  
  - **HIGH** → idle/ready (safe to write)  
  - **LOW** → busy (TX/RX in progress)  
  After each `write()`, wait for **AUX to go LOW then back HIGH** to confirm **RF emission completed**.
- **Mode changes:** After toggling M0/M1, wait for **AUX=HIGH** and a small **post-mode delay** (e.g., 60 ms) before further commands.
- **Config frames:** Only in **Sleep** (M0=1, M1=1). Always return to **Normal** for user data.

---

## File/photo transfer protocol

StratoLink uses a simple, robust application-layer framing on top of E32 transparent mode:

1. **Header (ASCII line)**  
   ```
   FILE: <basename>:<length>:<crc16_xmodem>\n
   ```
   - `length` is payload length in **bytes** (decimal).
   - `crc16_xmodem` is an **integer** checksum of the binary payload.
2. **Payload (binary)**  
   The file/photo bytes are then sent as raw data. The sender **chunks** to 58 B and waits on AUX for each chunk.

**Receiver logic (outline):**

- Read until a full line starting with `FILE:` is received.
- Parse basename, length, crc.
- Accumulate exactly `length` bytes from the stream.
- Compute CRC-16/XMODEM over the payload and compare with header.
- Save to disk (e.g., `rx/<basename>`).

This protocol is human-readable on the control line while staying fully **binary-clean** for payloads (JPEG/PNG/etc.). No Base64 is used (avoids ~33% bloat).

---

## Scripts

### `burner.py` — RF “burner”

Continuously sends a 58-byte `0x55` pattern (binary `01010101`) every 20 ms. This is useful to:

- Verify **RF emission** (AUX toggling, RF sniffer peak, current spikes)
- Validate **power rail** stability
- Confirm **wiring** and **AUX-synchronized TX**

Run:
```bash
python burner.py
# Ctrl-C to stop
```

### `photo.py` — take a photo each minute & send

Uses `fswebcam` to capture a photo and then calls `process_command("send <file>")` which packages the header and payload (with CRC) and transmits them over E32.

Run:
```bash
python photo.py
```

**Notes:**
- Default photo path is `photo.jpg` (can be changed in `take_photo()`).
- If your camera is not `/dev/video0`, add `-d /dev/videoX` in the `fswebcam` invocation.
- The loop is “once per ~minute after previous send finished” (not wall-clock-aligned).

---

## Program Menu

When you run `main.py` on the Raspberry Pi, you will see an interactive menu:

| Option | Function | Description |
|--------|----------|-------------|
| **1** | `check_parameters()` | Reads module parameters (`C1 C1 C1`) and version (`C3 C3 C3`) in HEX. |
| **2** | `check_mode()` | Displays M0/M1 pin states and current operating mode. |
| **3** | `send_data()` | Sends entered text in Normal mode (split into 58-byte chunks). |
| **4** | `process_command("send <file>")` | Sends a file with a CRC-XModem checksum in the header. |
| **5** | `receive_data()` | Receives data in Normal mode, displays as ASCII or HEX. |
| **6** | `process_command("status")` | Shows disk, memory, Wi-Fi, and IP status. |
| **7** | `process_command("list")` | Lists files in the working directory. |
| **8** | `process_command("restart")` | Sends `C4 C4 C4` to reset the module. |
| **9** | `send_data_with_crc()` | Sends text with length and CRC-XModem header. |
| **0** | Exit | Closes port and cleans up GPIO. |

---

## Sending Commands in HEX Mode

Unlike some other E32 modules that use **AT commands**, the **E32-900T30S EBYTE** uses **binary/HEX command frames** for configuration.  
To send a command, you must:

1. **Switch to Sleep mode** – M0=1, M1=1.  
2. Send the command bytes **in sequence, without pauses** over UART at **9600 8N1**.  
3. Wait for the AUX pin to go HIGH before changing modes or sending more commands.

### Common commands

| Purpose | Sequence (HEX) | Description |
|---------|---------------|-------------|
| Read parameters | `C1 C1 C1` | Returns a 6-byte frame with settings. |
| Read version | `C3 C3 C3` | Returns version info (e.g., `C3 44 xx yy` – 44 = 915 MHz). |
| Reset | `C4 C4 C4` | Resets and runs self-check (AUX LOW → HIGH). |
| Set parameters (save) | `C0` + 5 bytes | Writes parameters to flash. |
| Set parameters (no save) | `C2` + 5 bytes | Writes parameters to RAM only. |

> Example from `loraE32.py`:
```python
self.serial_conn.write(bytes([0xC0, 0x00, 0x00, 0x1A, 0x0F, 0x47]))
```

### Notes
- The default config is: **C0 00 00 1A 0F 47** (21 dBm, channel 0x0F, 9600 8N1, 2.4 kbps).  
- Max packet size in Normal mode: **58 bytes**.  
- Always check AUX pin status before sending.  
- Do not mix HEX configuration commands with normal data transmission.

---

## Ground-station basics

A minimal ground-side can be a **USB-UART** + second E32 module in the same mode/channel. A simple Python script can:

- Read lines to capture the file header (`FILE: ...\n`).
- Receive exactly `length` bytes of payload.
- Validate CRC-16/XMODEM.
- Save the file.

> For early tests, you can also run two RPis or a PC + USB serial adapter. Ensure the same **channel**, **SPED/OPTION** settings, and proper antenna/load on both ends.

---

## Troubleshooting (no RF / flaky link)

1. **Power first**  
   - Stable 5 V supply, ≥1 A headroom, decoupling near E32 (100 nF + 10 µF + ≥470 µF).  
   - If AUX never toggles on send → brownout or not actually in Normal mode.
2. **Mode pins**  
   - **Normal:** M0=0, M1=0; **Sleep:** M0=1, M1=1. Use firm logic levels (no floating).  
   - After each mode change: wait for **AUX=HIGH** and a short **post-mode delay**.
3. **AUX discipline**  
   - Before write: AUX should be HIGH.  
   - After write: wait to see **LOW→HIGH** cycle (TX completion).  
   - Chunk at **≤58 bytes**.
4. **UART plumbing**  
   - Disable serial console; ensure `/dev/serial0` is free.  
   - Confirm baudrate matches (default 9600).  
   - Consider a divider on E32 TXD if idle >3.3 V.
5. **Config verify**  
   - `C1 C1 C1` readback: SPED=0x1A, CH as configured, OPTION=0x47.  
   - **Channel→frequency:** for 900-series modules approx `f ≈ 850.125 MHz + CH` (1 MHz steps).
6. **Antenna & channel**  
   - Correct band antenna, proper matching, and legal channel for your region.  
   - Try line-of-sight and short distance first.

---

## Regulatory notice

Operating radio equipment is subject to **local regulations** (power, band, channel, duty cycle). Always comply with your jurisdiction’s rules—especially for **high-altitude balloon flights**.

---

## Links and Documentation

- **EBYTE E32-900T30S datasheet** (command frames and settings):  
  ↗ [RF_E32-900T30S_0002.pdf](https://www.micros.com.pl/mediaserver/RF_E32-900T30S_0002.pdf)  
- **StratoLink repository:** https://github.com/AGH-Skylink/StratoLink
