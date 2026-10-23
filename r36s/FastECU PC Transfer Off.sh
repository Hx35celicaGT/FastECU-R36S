#!/bin/sh
# Turns PC transfer off, so the USB-C port works with the OBD cable again.
setsid -f /usr/bin/usbgadget disabled >/dev/null 2>&1 </dev/null
