#!/bin/sh
# Lets a PC reach the R36S over a USB-C DATA cable (shows up as "ArchR Device").
# Uses Arch R's own usbgadget tool. setsid -f returns to the menu right away.
setsid -f /usr/bin/usbgadget file_transfer >/dev/null 2>&1 </dev/null
