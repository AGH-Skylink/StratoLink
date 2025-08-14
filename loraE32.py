import serial, sys, time, subprocess, os
from crc import Calculator,Crc16
import RPi.GPIO as GPIO
# symulacja rpi na pc:
# sys.modules['RPi'] = None
# from fake_rpi.RPi import GPIO
from typing import Optional, Dict


class loraE32:
    def __init__(self, port : str = '/dev/serial0', baudrate: int = 9600):
        self.M0_pin = 23
        self.M1_pin = 24
        self.AUX_pin = 25

        self.port = port
        self.baudrate = baudrate
        self.serial_conn = None
        self.crc_calculator = Calculator(Crc16.XMODEM, optimized=True)

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
        GPIO.setup(self.M0_pin, GPIO.OUT)
        GPIO.setup(self.M1_pin, GPIO.OUT)
        GPIO.setup(self.AUX_pin, GPIO.IN, pull_up_down = GPIO.PUD_UP)

    def _set_mode(self, m0: int, m1: int):
        GPIO.output(self.M0_pin, m0)
        GPIO.output(self.M1_pin, m1)
        time.sleep(0.05)

    def _enter_normal_mode(self):
        self._set_mode(GPIO.LOW, GPIO.LOW)

    def _enter_config_mode(self):
        self._set_mode(GPIO.HIGH, GPIO.HIGH)

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

    def _wait_for_aux(self, timeout: float=2.0) -> bool:
        print("[Waiting for AUX HIGH...]")
        start = time.time()
        while (time.time() - start) < timeout:
            if  GPIO.input(self.AUX_pin):
                print("[AUX is HIGH]")
                return True
            time.sleep(0.01)
        return False

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

    def configure_module(self) -> bool:
        self._enter_config_mode()
        self._wait_for_aux()

        self.serial_conn.reset_input_buffer()
        self.serial_conn.reset_output_buffer()

        self.serial_conn.write(bytes([0xC0, 0x00, 0x00, 0x1A, 0x0F, 0x47]))

        self._wait_for_aux(timeout=2.0)
        time.sleep(0.02)

        self.serial_conn.reset_input_buffer()
        self.serial_conn.write(bytes([0xC1, 0xC1, 0xC1]))
        resp = self._read_exact(6, timeout=2.0)

        print(f"Configuration after power change: len={len(resp)} data={resp.hex()}")

        ok = len(resp) == 6 and resp[5] == 0x47
        if ok:
            print("Configuration OK")
        else:
            print("Configuration FAILED")

        self._enter_normal_mode()
        return ok

    def check_parameters(self):
        self._enter_config_mode()
        self._wait_for_aux()
        self.serial_conn.reset_input_buffer()
        self.serial_conn.write(bytes([0xC1, 0xC1, 0xC1]))
        resp = self._read_exact(6, timeout=2.0)
        print(f"Present configuration parameters: len={len(resp)} data={resp.hex()}")

        self.serial_conn.reset_input_buffer()
        self.serial_conn.write(bytes([0xC3, 0xC3, 0xC3]))
        resp = self._read_exact(6, timeout=2.0)
        print(f"Present version number: len={len(resp)} data={resp.hex()}")

        print("#### To read settings go to documentation ####")

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

    def send_data(self, data: bytes, chunk_size: int = 58, timeout: float=5.0) -> bool:
        self._enter_normal_mode()
        try:
            for i in range(0, len(data), chunk_size):
                chunk = data[i:i+chunk_size]
                if not self._wait_for_aux(timeout):
                    return False
                self.serial_conn.write(chunk)
                time.sleep(0.01)
            return True
        except serial.SerialTimeoutException:
            print("Serial Timeout")
            return False
        except Exception as e:
            print(f"Send error: {str(e)}")
            return False

    def receive_data(self, timeout: float = 2.0) -> Optional[bytes]:
        self._enter_normal_mode()
        try:
            start = time.time()
            while (time.time() - start) < timeout:
                if GPIO.input(self.AUX_pin) == GPIO.HIGH:
                    if self.serial_conn.in_waiting > 0:
                        return self.serial_conn.read(self.serial_conn.in_waiting)
                time.sleep(0.01)
            return None
        except Exception as e:
            print(f"Receive error: {str(e)}")
            return None

    def process_command(self, command: str) -> Dict:
        self._enter_normal_mode()
        if command == "list":
            try:
                result = subprocess.run(['ls','-l'], capture_output=True, text=True)
                return {"status": "success", "output": result.stdout.strip()}
            except Exception as e:
                return {"status": "error", "output": str(e)}

        elif command.startswith("send"):
            filename = command[5:]
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
            return {"status": "success"}

    def send_data_with_crc(self, data : bytes) -> bool:
        crc = self.crc_calculator.checksum(data)
        packet = f"{len(data)}:{crc}\n".encode('UTF-8', errors='replace')+data
        return self.send_data(packet)

    def close(self):
        if self.serial_conn and self.serial_conn.is_open:
            self.serial_conn.close()
        GPIO.cleanup()