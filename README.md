# StratoLink
LoRA-based image downlink solution for stratospheric baloon missions. Raspberry Pi + E32-900T30S EBYTE module over UART. Developed for a stratoshperic baloon payload made by the  Space Technology Centre at AGH Univeristy.

---

## Table of Contents

- [Project Overview](#project-overview)  
- [Requirements](#requirements)  
- [Wiring Diagram](#wiring-diagram)  
- [Quick Start](#quick-start)  
- [Class and Method Overview](#class-and-method-overview)  
- [Program Menu](#program-menu)  
- [Sending Commands in HEX Mode](#sending-commands-in-hex-mode)  
- [Diagnostics and Debugging](#diagnostics-and-debugging)  
- [Links and Documentation](#links-and-documentation)  
- [License](#license)

---

## Project Overview

StratoLink is a Python module (`loraE32` class) running on a Raspberry Pi that initializes and communicates with an **E32-900T30S** LoRa module (915 MHz band). By default:

- Sets RF power to **21 dBm** (~125 mW)
- Switches the module to **Normal mode** (TX/RX)
- Sends and receives data over UART with AUX pin synchronization

The code also includes helper methods to check pin states (M0, M1, AUX) and determine the operating mode.

---

## Requirements

- Raspberry Pi (e.g., Pi 4) with UART interface `/dev/serial0` (TXD0/RXD0).  
- Python ≥ 3.7 with:
  - `pyserial`
  - `RPi.GPIO`
  - `crc` (CRC-XModem calculator)
- **EBYTE E32-900T30S** module powered at 3.3 V with a common ground.

---

## Wiring Diagram

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

## Quick Start

1. **Install required packages**
   ```bash
   pip install pyserial RPi.GPIO crc
   ```

2. **Enable UART**  
   Edit `/boot/config.txt` and set:
   ```
   enable_uart=1
   ```
   Disable Bluetooth to free up the main UART by adding this line to `/boot/config.txt`:
    ```
   dtoverlay=disable-bt
    ```

3. **Run the test program**
   ```bash
   python3 main.py
   ```
   You should see configuration parameters (HEX) and a mode confirmation.

---

## Class and Method Overview

### `class loraE32`

- **`__init__()`**  
  Sets up GPIO pins, initializes UART, and immediately configures the module (sets power, channel, saves settings).

- **`configure_module()`**  
  Sends frame `C0 00 00 1A 0F 47` — stores configuration (21 dBm, CH=0x0F, 9600 8N1, 2.4 kbps).  
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

## Diagnostics and Debugging

- Configuration issues (e.g., `IndexError`) usually mean **no response** — check AUX pin, wiring, supply voltage, and UART settings.  
- `check_mode()` quickly shows the current operating mode (useful in `main.py` during tests).  
- After a read, check `len(resp)` to confirm a **full 6-byte frame** was received.  
- Ensure **M0/M1** are at correct levels and **AUX** toggles appropriately for each command.

---

## Links and Documentation

- **EBYTE E32-900T30S datasheet** (command frames and settings):  
  ↗ [RF_E32-900T30S_0002.pdf](https://www.micros.com.pl/mediaserver/RF_E32-900T30S_0002.pdf)  
- **StratoLink repository:** https://github.com/AGH-Skylink/StratoLink
