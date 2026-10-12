# ESP32 Wakeup Light

Minimal ESP-IDF firmware for the ESP32-C3 Super Mini. It drives one WS2812B LED
on GPIO 4, ramping from off to red and then to warm white over 20 seconds.

## Requirements

- ESP-IDF v5.5.2, including the ESP32-C3 toolchain
- The board connected over USB
- A 2.4 GHz Wi-Fi network with internet access for time synchronization

On CachyOS, install the ESP-IDF build prerequisites:

```sh
sudo pacman -S --needed git wget flex bison gperf python cmake ninja ccache dfu-util libusb
```

Install ESP-IDF if it is not already installed:

```sh
mkdir -p ~/esp
git clone --branch v5.5.2 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf
./install.sh esp32c3
source ./export.fish
```

Whenever opening a new terminal, activate the tools with:

```sh
source ~/esp/esp-idf/export.fish
```

## Build and flash

On a fresh clone, create the local settings file, then edit it:

```sh
cp -n main/wakeup_config.example.h main/wakeup_config.h
```

Set your Wi-Fi name and password, timezone rule, wake hour/minute, and ramp
duration in `main/wakeup_config.h`. This private file is ignored by Git; the
tracked example contains only placeholders. The example timezone is for Central
European time. The configured wake time means when the ramp should finish, so a
07:00 wake time with a 20-second ramp starts at 06:59:40.

Then, from the project directory, configure the target and build. If you are
still in the ESP-IDF installation directory, first change to this repository:

```sh
cd ~/git/esp32_wakeup_light
idf.py set-target esp32c3
idf.py build
```

The ramp has full red halfway through and reaches the configured warm-white
color at the wake time. A smooth ease-in/ease-out curve makes transitions
gentler. If Wi-Fi or time synchronization is unavailable, the LED stays off
while the device retries. This initial scheduler stays awake, runs once per
boot, and leaves the LED at its final color afterward; deep sleep and an
automatic off time can be added later.

For the ESP32-C3's native USB-Serial/JTAG connection, use esptool's USB reset
strategy to enter download mode and flash:

```sh
cd ~/git/esp32_wakeup_light/build
python -m esptool --chip esp32c3 -p /dev/ttyACM0 -b 460800 --before usb_reset --after no_reset write_flash @flash_args
```

Replace `/dev/ttyACM0` if the board appears on a different port. After flashing,
press RESET to start the ramp. If USB reset cannot connect, enter download mode
manually by holding BOOT, tapping RESET, then releasing BOOT. If the port reports
a permission error, add your user to the `uucp` group and log in again:

```sh
sudo usermod -aG uucp "$USER"
```

## Status and schedule page

After boot, the device connects to Wi-Fi, synchronizes time, and prints its IP
address to the serial log. Open the monitor with:

```sh
idf.py -p /dev/ttyACM0 monitor
```

From a phone or computer on the same Wi-Fi network, open
`http://wakeup-light.local/`. The ESP also advertises `wakeup-light` as its
DHCP hostname, so it may appear by that name in your router. The page shows
Wi-Fi/time/schedule status and lets you save the wake hour, minute, and ramp
duration. Duration examples are 300 seconds for 5 minutes, 600 for 10 minutes,
and 1800 for 30 minutes. Saved settings persist across restarts and override the
build-time defaults in `wakeup_config.h`.

The `.local` address uses mDNS and works on most modern phones and computers on
the same LAN. Some routers or guest Wi-Fi networks block mDNS; in that case use
the IP shown in the serial log or router. A short name such as `wake.up` needs
a local DNS entry from your router; the ESP cannot create that DNS alias itself.

The page has no login and is intended only for your trusted home network. Do not
forward its port from your router or expose it directly to the internet.

The LED needs a suitable power supply and a ground shared with the ESP32. Do
not power an LED strip from a GPIO pin.