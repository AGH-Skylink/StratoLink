<div align="center">
    <img src="assets/LOGO.png", width="200"/>
    <h1>StratoLink</h1>
</div>

A LoRa-based telemetry and communication system for stratospheric missions. StratoLink focuses on reliable long-range data links between a high-altitude balloon payload and a ground station. It has features like lossless photo & file transfers, remote camera control, on-the-fly channel & air rate switching, etc.

It uses Raspberry Pi + E32-900T30S EBYTE and was developed for a stratoshperic baloon payload made by the  Space Technology Centre at AGH Univeristy.

## Commands
- photo = take a photo and send it to ground (with error correction)
- exposure = change camera exposure (auto/manual)
- send &lt;file_name&gt; = send a file with a given path
- list &lt;path&gt; = ls -l result
- config = change air rate and channel
- status = disk & ram usage, transmission power and current config
- restart = remote reset E32 module and restart RPi

## Setup
### Config
- /boot/config.txt
```
enable_uart=1
dtoverlay=disable-bt
```
- `raspi-config` -> interface options -> serial -> “No” for login shell, “Yes” for serial port hardware, interface options -> serial -> camera -> enable
- sudo usermod -a -G dialout $USER
- sudo apt-get install fswebcam libgpiod-dev
- reboot
- M0 to 23, M1 to 24, AUX to 25

## Compile

- Compile with `-lgpiod`
- `./stratolink > stratolink_error_logs 2>&1 &` run and move to background and redirect output to log

