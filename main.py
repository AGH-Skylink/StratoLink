import loraE32

uart=loraE32

response = uart._send_at_command("AT+POWER=?")
print(response)
response = uart._send_at_command("AT+CHANNEL=?")
print(response)
