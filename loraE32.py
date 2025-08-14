import time, sys, serial
import RPi.GPIO as GPIO

PORT = "/dev/serial0"
BAUD = 9600
TIMEOUT = 1.0
CMD_READ_CONFIG = b"\xC1\xC1\xC1"
M0_pin = 23
M1_pin = 24
AUX_pin = 25

GPIO.setmode(GPIO.BCM)
GPIO.setup(M0_pin, GPIO.OUT, initial=GPIO.HIGH)
GPIO.setup(M1_pin, GPIO.OUT, initial=GPIO.HIGH)
GPIO.setup(AUX_pin, GPIO.IN, pull_up_down=GPIO.PUD_UP)

t0 = time.time()
while time.time() - t0 < 2.0:
    if GPIO.input(AUX_pin):
        break
    time.sleep(0.001)
time.sleep(0.002)

ser = None
try:
    ser = serial.Serial(PORT, BAUD, timeout=TIMEOUT)
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    time.sleep(0.01)
    print("TX:", CMD_READ_CONFIG.hex(" ").upper())
    ser.write(CMD_READ_CONFIG)
    ser.flush()
    time.sleep(0.05)

    resp = ser.read(6)
    print("RX:", resp.hex(" ").upper() if resp else "(no data)")

except Exception as e:
    print("[ERR]", e)
finally:
    if ser and ser.is_open:
        ser.close()
    GPIO.cleanup()