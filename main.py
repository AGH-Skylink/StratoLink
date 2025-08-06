import loraE32

uart=loraE32.loraE32()

uart.send_query()
uart.close()