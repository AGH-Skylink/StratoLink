import loraE32
import time

print("Initializing E32...")
uart = loraE32.loraE32()
print("Checking parameters...")
uart.check_parameters()

print("\n")

def menu():
    try:
        while True:
            print("\n=== LoRa E32 Menu ===")
            print("1) Check module parameters (check_parameters)")
            print("2) Check operating mode (check_mode)")
            print("3) Send data (send_data)")
            print("4) Send file (process_command: send <file>)")
            print("5) Receive data (receive_data)")
            print("6) System status (process_command: status)")
            print("7) List files (process_command: list)")
            print("8) Restart module (process_command: restart)")
            print("9) Send text with CRC (send_data_with_crc)")
            print("0) Exit")
            print("======================")
            uart.send_data("Enter your command: ".encode("utf-8",errors="replace"))
            choice = uart.receive_data()

            if choice == "1":
                uart.check_parameters()

            elif choice == "2":
                uart.check_mode()

            elif choice == "3":
                txt = input("Enter data to send: ")
                ok = uart.send_data(txt.encode("utf-8", errors="replace"))
                print("Sent successfully." if ok else "Send failed.")

            elif choice == "4":
                path = input("Enter file path: ").strip()
                res = uart.process_command("send " + path)
                if res.get("status") == "success":
                    print(f"OK: sent {res.get('filename')} ({res.get('size')} B), CRC={res.get('crc')}")
                else:
                    print("Error:", res.get("output"))

            elif choice == "5":
                try:
                    t = float(input("Receive timeout in seconds [default 2]: ") or "2")
                except ValueError:
                    t = 2.0
                data = uart.receive_data(timeout=t)
                if data is None:
                    print("No data received.")
                else:
                    mode = input("Display as ASCII (A) or HEX (H)? [default A]: ").strip().lower() or "a"
                    if mode == "h":
                        print("Received (HEX):", data.hex())
                    else:
                        print("Received (ASCII):")
                        print(data.decode("utf-8", errors="replace"))

            elif choice == "6":
                res = uart.process_command("status")
                if res.get("status") == "success":
                    print("\n--- Disk ---\n" + res.get("diskspace", ""))
                    print("\n--- Memory ---\n" + res.get("memory", ""))
                    print("\n--- Wi-Fi ---\n" + res.get("configuration", ""))
                    print("\n--- IP ---\n" + res.get("ip", ""))
                else:
                    print("Error:", res.get("output"))

            elif choice == "7":
                res = uart.process_command("list")
                print(res.get("output") if res.get("status") == "success" else f"Error: {res.get('output')}")

            elif choice == "8":
                res = uart.process_command("restart")
                print("Restart command sent." if res.get("status") == "success" else f"Error: {res.get('output')}")

            elif choice == "9":
                txt = input("Enter data to send (CRC): ")
                ok = uart.send_data_with_crc(txt.encode("utf-8", errors="replace"))
                print("Sent with CRC." if ok else "Send with CRC failed.")

            elif choice == "0":
                print("Closing...")
                break

            else:
                print("Unknown option.")

            time.sleep(0.05)

    except KeyboardInterrupt:
        print("\nInterrupted by user (Ctrl+C).")
    finally:
        print("Connection closed.")
        uart.close()

menu()
