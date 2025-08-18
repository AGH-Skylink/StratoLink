import loraE32, time

uart = loraE32.loraE32()

def main():
    while True:
        uart.take_photo()
        uart.process_command("send photo.jpg")
        time.sleep(60)
