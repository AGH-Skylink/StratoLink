import serial, time, subprocess, os
from crc import Calculator,Crc16
import RPi.GPIO as GPIO
# symulacja rpi na pc:
# sys.modules['RPi'] = None
# from fake_rpi.RPi import GPIO
from typing import Optional, Dict, Any


class LoraE32:
    def __init__(self, port : str = '/dev/serial0', baudrate: int = 9600):
        self.M0_pin = 23
        self.M1_pin = 24
        self.AUX_pin = 25

        self.port = port
        self.baudrate = baudrate
        self.serial_conn = None
        self.crc_calculator = Calculator(Crc16.XMODEM.value, optimized=True)

        try:
            self._setup_gpio()
            self._enter_normal_mode()
            self._init_serial()
            if not self.configure_module():
                raise Exception('Failed to configure module')
        except Exception as e:
            self.close()
            raise RuntimeError(f"Initialization error: {str(e)}")

    def _setup_gpio(self):
        GPIO.setmode(GPIO.BCM)
        GPIO.setup(self.M0_pin, GPIO.OUT, initial=GPIO.LOW)
        GPIO.setup(self.M1_pin, GPIO.OUT, initial=GPIO.LOW)
        GPIO.setup(self.AUX_pin, GPIO.IN, pull_up_down = GPIO.PUD_UP)

    def _set_mode(self, m0, m1):
        GPIO.output(self.M0_pin, GPIO.HIGH if m0 else GPIO.LOW)
        GPIO.output(self.M1_pin, GPIO.HIGH if m1 else GPIO.LOW)
        if not self._wait_for_aux(1, timeout=2.0):
            raise TimeoutError("AUX didn't go HIGH after mode change")
        self._post_mode_delay()

    def _enter_normal_mode(self):
        self._set_mode(0, 0)

    def _enter_config_mode(self):
        self._set_mode(1, 1)

    def _init_serial(self):
        self.serial_conn = serial.Serial(
            port=self.port,
            baudrate=self.baudrate,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            bytesize=serial.EIGHTBITS,
            timeout=1
        )
        if not self.serial_conn.is_open:
            self.serial_conn.open()
        time.sleep(0.1)

    def _wait_for_aux(self, level=1, timeout=2.0) -> bool:
        t0 = time.time()
        while GPIO.input(self.AUX_pin) != level:
            if time.time() - t0 > timeout:
                return False
            time.sleep(0.01)
        return True

    @staticmethod
    def _post_mode_delay():
        time.sleep(0.08)

    def _read_exact(self, n: int, timeout: float = 2.0) -> bytes:
        end = time.time() + timeout
        out = bytearray()
        while len(out) < n and time.time() < end:
            chunk = self.serial_conn.read(n - len(out))
            if chunk:
                out.extend(chunk)
            else:
                time.sleep(0.01)
        return bytes(out)

    def _wait_tx_complete(self, overall_timeout=5.0):
        t0 = time.time()
        seen_low = False
        while time.time() - t0 < overall_timeout:
            lvl = GPIO.input(self.AUX_pin)
            if lvl == 0:
                seen_low = True
            if seen_low and lvl == 1:
                return True
            time.sleep(0.001)
        return False

    def configure_module(self) -> bool:
        self._enter_config_mode()
        self._wait_for_aux()

        frame = bytes([0xC0, 0x00, 0x00, 0x1A, 0x06, 0x47])

        self.serial_conn.reset_input_buffer()
        self.serial_conn.reset_output_buffer()

        if not self._wait_for_aux(1, timeout=1.0):
            raise TimeoutError("AUX not ready before config write")

        self.serial_conn.write(frame)
        self.serial_conn.flush()
        self._wait_for_aux(1, timeout=1.0)
        time.sleep(0.02)

        self.serial_conn.reset_input_buffer()
        self.serial_conn.write(bytes([0xC1, 0xC1, 0xC1]))
        self.serial_conn.flush()

        resp = self._read_exact(6, timeout=2.0)
        print(f"Configuration after power change: len={len(resp)} data={resp.hex()}")

        ok = len(resp) == 6 and resp[5] == 0x47
        if len(resp) != 6:
            raise RuntimeError(f"Read params failed, got len={len(resp)}")
        if resp[5] != 0x47:
            raise RuntimeError(f"Option byte mismatch: got 0x{resp[5]:02X}, expected 0x47")

        self._enter_normal_mode()
        return ok

    def check_parameters(self):
        self._enter_config_mode()
        self._wait_for_aux()
        self.serial_conn.reset_input_buffer()
        self.serial_conn.write(bytes([0xC1, 0xC1, 0xC1]))
        self.serial_conn.flush()
        resp = self._read_exact(6, timeout=2.0)
        print(f"Present configuration parameters: {resp.hex()}")

        if len(resp) == 6 and resp[0] == 0xC0:
            sped, chan, powe = resp[3], resp[4], resp[5]

            baud_bits = (sped >> 3) & 0b111
            baud_map = {
                0b000: 1200, 0b001: 2400, 0b010: 4800, 0b011: 9600,
                0b100: 19200, 0b101: 38400, 0b110: 57600, 0b111: 115200
            }
            baudrate = baud_map.get(baud_bits, "??")

            freq_mhz = 850.125 + chan

            power_bits = powe & 0b11
            power_map = {0b00: 30, 0b01: 27, 0b10: 24, 0b11: 21}
            power = power_map.get(power_bits, "??")

            print(f"BAUDRATE UART: {baudrate} bps")
            print(f"CHANNEL: {chan} (freq {freq_mhz} MHz)")
            print(f"POWER: {power} dBm")
        else:
            print("WRONG CONFIGURATION - precising in progress...")

        self.serial_conn.reset_input_buffer()
        self.serial_conn.write(bytes([0xC3, 0xC3, 0xC3]))
        self.serial_conn.flush()
        resp = self._read_exact(6, timeout=2.0)
        print(f"Present version number: {resp.hex()}")

        print("### To read settings go to documentation ###")
        self._enter_normal_mode()

    def check_mode(self):
        m0 = GPIO.input(self.M0_pin)
        m1 = GPIO.input(self.M1_pin)
        print(f"M0={m0}, M1={m1}", end=" → ")

        if m0 == GPIO.LOW and m1 == GPIO.LOW:
            print("Normal Mode")
        elif m0 == GPIO.HIGH and m1 == GPIO.LOW:
            print("Wake-up Mode")
        elif m0 == GPIO.LOW and m1 == GPIO.HIGH:
            print("Power-saving Mode")
        elif m0 == GPIO.HIGH and m1 == GPIO.HIGH:
            print("Sleep Mode")
        else:
            print("Unknown state (check wiring)")

    def send_data(self, data: bytes, chunk_size: int = 58) -> bool:
        self._enter_normal_mode()
        if chunk_size > 58:
            chunk_size = 58

        off = 0
        while off < len(data):
            part = data[off:off + chunk_size]
            if not self._wait_for_aux(1, timeout=1.0):
                raise TimeoutError("AUX not ready before send")

            self.serial_conn.write(part)
            self.serial_conn.flush()

            if not self._wait_tx_complete(overall_timeout=5.0):
                raise TimeoutError("TX AUX did not complete (no low->high cycle)")

            off += len(part)
        return True

    def receive_data(self, overall_timeout: float = 2.0, window: float = 0.05) -> Optional[bytes]:
        self._enter_normal_mode()
        end = time.time() + overall_timeout
        out = bytearray()

        while time.time() < end:
            if not self._wait_for_aux(1, timeout=max(0.0, end - time.time())):
                break

            while self.serial_conn.in_waiting:
                out += self.serial_conn.read(self.serial_conn.in_waiting)
                time.sleep(0.001)

            t0 = time.time()
            next_pkt = False
            while time.time() - t0 < window:
                if GPIO.input(self.AUX_pin) == GPIO.LOW:
                    next_pkt = True
                    break
                time.sleep(0.001)

            if not next_pkt:
                break

        return bytes(out) if out else None

    def process_command(self, command: str) -> Dict[str,Any]:
        self._enter_normal_mode()
        if command == "list":
            try:
                result = subprocess.run(['ls','-la', '/'], capture_output=True, text=True)
                return {"status": "success", "output": result.stdout.strip()}
            except Exception as e:
                return {"status": "error", "output": str(e)}

        elif command.startswith("send"):
            filename = command[5:].strip()
            if not filename:
                return {"status": "error", "output": "No filename specified"}
            try:
                if not os.path.exists(filename):
                    return {"status": "error", "output": "File does not exist"}

                with open(filename, "rb") as f:
                    data = f.read()
                    crc = self.crc_calculator.checksum(data)

                    header = f"FILE: {os.path.basename(filename)}:{len(data)}:{crc}\n"

                    if not self.send_data(header.encode("UTF-8" ,errors='replace')):
                        return {"status": "error", "output": "Header send failed"}

                    if not self.send_data(data):
                        return {"status": "error", "output": "Data send failed"}

                return {"status": "success", "filename": filename, "size": len(data), "crc":crc}

            except PermissionError:
                return {"status": "error", "output": f"Permission denied: {filename}"}
            except IsADirectoryError:
                return {"status": "error", "output": f"Is a directory: {filename}"}
            except Exception as e:
                return {"status": "error", "output": f"Unexpected error: {str(e)}"}

        elif command == "status":
            try:
                disk = subprocess.run(['df','-h'], capture_output=True, text=True)
                mem = subprocess.run(['free','-m'], capture_output=True, text=True)
                conf = subprocess.run(['iwconfig'], capture_output=True, text=True)
                ip = subprocess.run(['ip', 'addr'], capture_output=True, text=True)

                return{
                    "status": "success",
                    "diskspace": disk.stdout.strip('\n'),
                    "memory": mem.stdout.strip('\n'),
                    "configuration": conf.stdout.strip('\n'),
                    "ip": ip.stdout.strip('\n'),
                }
            except Exception as e:
                return {"status": "error", "output": str(e)}

        elif command == "restart":
            self._enter_config_mode()
            self.serial_conn.write(bytes([0xC4, 0xC4, 0xC4]))
            self.serial_conn.flush()
            self._wait_for_aux()
            self._post_mode_delay()
            self._enter_normal_mode()
            return {"status": "success"}

        return {"status": "error", "output": f"Unknown command: {command}"}

    def send_data_with_crc(self, data : bytes) -> bool:
        crc = self.crc_calculator.checksum(data)
        packet = f"{len(data)}:{crc}\n".encode('UTF-8', errors='replace')+data
        return self.send_data(packet)

    def take_photo(self):
        subprocess.run(['fswebcam', '-r', '320x240', '--jpeg', '60', '--no-banner', 'photo.jpg'])
        time.sleep(1)

    def close(self):
        if self.serial_conn and self.serial_conn.is_open:
            self.serial_conn.close()
        GPIO.cleanup()