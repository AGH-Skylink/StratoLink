# send_simple.py
import time
import serial
import RPi.GPIO as GPIO

# Piny BCM
M0_PIN = 23
M1_PIN = 24
AUX_PIN = 25

PORT = '/dev/serial0'
BAUD = 9600

CFG_BYTES = bytes([0xC0, 0x00, 0x00, 0x1A, 0x0F, 0x47])  # takie jak u Ciebie

def wait_aux(timeout=2.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if GPIO.input(AUX_PIN) == GPIO.HIGH:
            return True
        time.sleep(0.01)
    return False

def set_mode(m0_level, m1_level):
    GPIO.output(M0_PIN, m0_level)
    GPIO.output(M1_PIN, m1_level)
    time.sleep(0.04)  # zalecana przerwa po zmianie trybu

def main():
    GPIO.setmode(GPIO.BCM)
    GPIO.setup(M0_PIN, GPIO.OUT)
    GPIO.setup(M1_PIN, GPIO.OUT)
    GPIO.setup(AUX_PIN, GPIO.IN, pull_up_down=GPIO.PUD_UP)

    ser = serial.Serial(PORT, BAUD, timeout=1)

    try:
        # Tryb konfiguracji (M0=1, M1=1)
        set_mode(GPIO.HIGH, GPIO.HIGH)
        wait_aux()

        # Wyślij parametry
        ser.reset_input_buffer()
        ser.write(CFG_BYTES)
        ser.flush()
        wait_aux()

        # Powrót do normalnego trybu (M0=0, M1=0)
        set_mode(GPIO.LOW, GPIO.LOW)
        wait_aux()

        # Prosty payload
        payload = b"HELLO E32 / UART test\n"
        ser.write(payload)
        ser.flush()
        wait_aux()

        print("Wysłano:", payload)

    finally:
        ser.close()
        GPIO.cleanup()

if __name__ == "__main__":
    main()
