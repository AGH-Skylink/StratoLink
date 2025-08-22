import time
from loraE32 import LoraE32

def main():
    radio = LoraE32()

    try:
        payload = bytes([0x55]) * 58
        while True:
            radio.send_data(payload)
            time.sleep(0.02)
    except KeyboardInterrupt:
        pass
    finally:
        radio.close()

if __name__ == "__main__":
    main()
