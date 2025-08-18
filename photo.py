import loraE32, time

uart = loraE32.loraE32()

def main():
    while True:
        uart.take_photo()
        res = uart.process_command("send photo.jpg")
        if res.get("status") != "success":
            print("ERROR:", res.get("output"))
        else:
            print(f"Correctly sent {res['filename']} ({res['size']} B), CRC={res['crc']}")
        time.sleep(60)


main()