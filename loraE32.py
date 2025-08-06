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
        self.CLK_pin = 11

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
        GPIO.setup(self.AUX_pin, GPIO.IN, pull_up_down = GPIO.PUD_DOWN)
        GPIO.setup(self.CLK_pin, GPIO.OUT, initial=GPIO.HIGH)

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

    def _wait_for_aux(self, timeout: float=1.0) -> bool:
        start = time.time()
        while (time.time() - start) < timeout:
            if  GPIO.input(self.AUX_pin) == GPIO.HIGH:
                return True
            time.sleep(0.01)
        return False

    def _send_at_command(self, command: str, timeout: float=1.0) -> bool:
        try:
            print(f"Sending AT command: {command}")
            self.serial_conn.reset_input_buffer()
            self.serial_conn.write(f"{command}\r\n".encode(errors='replace'))
            time.sleep(0.01)

            start = time.time()
            while (time.time() - start) < timeout:
                if self.serial_conn.in_waiting > 0:
                    response = self.serial_conn.read(self.serial_conn.in_waiting).decode(errors='replace')
                    print("Response:", response)
                    if "ERROR" in response:
                        return False
                time.sleep(0.01)
            return True
        except Exception as e:
            print(f"Command error: {str(e)}")
            return False

    def configure_module(self) -> bool:
        self._enter_config_mode()
        if not self._wait_for_aux(timeout=2.0):
            return False

        config_commands = [
            "AT+POWER=3",
            "AT+PARAMETER=10,7,1,7",
            "AT+ADDRESS=0",
            "AT+NETWORKID=0"
        ]

        for command in config_commands:
            if not self._send_at_command(command):
                print(f"Command failed: {command}")
                return False
            time.sleep(0.2)
        return True

    def send_query(self):
        print("Sending power query: ")
        print(self._send_at_command("AT+POWER=?"))
        print("\nSending channel query: ")
        print(self._send_at_command("AT+CHANNEL=?"))

    def send_data(self, data: bytes, chunk_size: int = 58, timeout: float=5.0) -> bool:
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

                    if not self.send_data(header.encode("UTF-8" ,errors='replace'), data):
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
                    "signal_strength": self.get_signal_strength()
                }
            except Exception as e:
                return {"status": "error", "output": str(e)}

        elif command == "restart":
            response = self._send_at_command("AT+RESET")
            return {"status": "success" if response else "error", "response": response}

    def send_data_with_crc(self, data : bytes) -> bool:
        crc = self.crc_calculator.checksum(data)
        packet = f"{len(data)}:{crc}\n".encode('UTF-8', errors='replace')+data
        return self.send_data(packet)

    def get_signal_strength(self) -> Optional[float]:
        self._enter_config_mode()
        response = self._send_at_command("AT+RRSI?")
        self._enter_normal_mode()

        if response and "RSSI=" in response:
            try:
                return float(response.split("RSSI=")[1].split()[0])
            except (IndexError, ValueError):
                pass
        return None

    def close(self):
        if self.serial_conn and self.serial_conn.is_open:
            self.serial_conn.close()
        GPIO.cleanup()