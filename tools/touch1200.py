"""Put the Feather into RP2040 BOOTSEL from the host, with no button press.

Adafruit_USBD_CDC's tud_cdc_line_state_cb() calls TinyUSB_Port_EnterDFU() when
DTR drops while the line coding is 1200 bps. Opening the console at 1200 and
dropping DTR is therefore the same thing as double-tapping the reset button,
except it can be scripted -- which the whole point of this rig requires.

The RPI-RP2 drive appears on D: a few seconds later; that is where
platformio.ini's upload_command copies the UF2.

    python touch1200.py COM14
    pio run -e adafruit_feather_rp2040_usb_host -t upload

Not to be confused with putting the *OnlyKey* into ITS bootloader, which is the
`b` console command (a pulse on GPIO 6). Two different bootloaders.
"""
import sys
import time

import serial


def touch(port):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 1200
    ser.dtr = True
    ser.open()
    time.sleep(0.2)
    ser.dtr = False
    time.sleep(0.2)
    # The board resets out from under us, so closing may well fail. Harmless.
    try:
        ser.close()
    except Exception:
        pass


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    port = sys.argv[1]
    touch(port)
    print("1200-baud touch sent to %s -- expect RPI-RP2 on D: shortly" % port)
    return 0


if __name__ == "__main__":
    sys.exit(main())
