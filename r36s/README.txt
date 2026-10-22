FastECU-R36S
============

FastECU for the R36S handheld: log and tune Subaru ECUs from the R36S.
This is an unofficial port of FastECU (https://github.com/miikasyvanen/fastecu-oem).
Please report problems on the FastECU-R36S GitHub page, not to the FastECU authors.

!! WARNING !!
This is a TEST version. Start with Logging only.
Only use Read/Write ROM on an ECU you can recover. A write that fails
(loose cable, flat battery) can leave the ECU unable to start the car.


WHAT YOU NEED
-------------
- R36S running Arch R (any board: original or clone)
- A PC with an SD card reader (Windows, Mac or Linux)
- USB-C OTG adapter
- OBD cable: Tactrix OpenPort 2.0 (tested with a clone).
  VAG-COM / KKL cables usually have an FTDI chip, which Arch R has no
  driver for yet. See "Cables" on the GitHub page for the latest list.


1. PUT ARCH R ON THE CARD (skip if you already run Arch R)
-----------------------------------------------------------
a) BACK UP THE CARD THAT CAME WITH THE R36S FIRST.
   Copy everything from its small boot partition to a folder on your PC.
   It holds the file needed to fix the screen if it looks wrong later.
b) Find out if your R36S is an original or a clone, and the board version
   printed on the circuit board (for example "R36S-V21").
c) Download the Arch R Flasher for your PC:
     https://github.com/archr-linux/archr-flasher/releases
d) In the flasher, pick the image (Original or Clone) and your screen panel,
   then flash a good-quality SD card.
e) Put the card in the R36S and turn it on. Let it finish its first start
   until you see the menu. Then turn it off.
If the screen shows wrong colors or a rolling band, see "SCREEN PROBLEMS" below.


2. COPY FASTECU-R36S TO THE CARD
--------------------------------
a) Put the card in your PC.
   WINDOWS: if it asks "You need to format the disk", click CANCEL.
   Formatting erases the card.
b) Open the ROMS partition, then the ports folder.
c) Copy BOTH of these from the zip into the ports folder:
     FastECU.sh
     FastECU-R36S   (folder)
d) Eject the card safely, put it back in the R36S.


3. START IT
-----------
Turn on the R36S, go to Ports, start FastECU.
Plug the OBD cable into the R36S with the OTG adapter, then into the car.
Turn the ignition on.

The sticks move the mouse pointer. To quit: File -> Quit.
If FastECU stops responding, hold SELECT + START to force it closed.


WHERE YOUR FILES ARE
--------------------
All on the card, inside ports/FastECU-R36S:
  FastECU-Files/       log_setup.txt (what is shown and logged)
  userdata/            settings, and datalogs (CSV files) in
                       userdata/.config/FastECU/<version>/datalogs
  fastecu-debug.log    what happened on the last run (attach to bug reports)


REPORTING A PROBLEM
-------------------
Open an issue on the FastECU-R36S GitHub page and include:
  - board version (e.g. R36S-V21) and Original or Clone image
  - your OBD cable (brand, and chip if you know it)
  - car and year
  - what you did and what happened
  - fastecu-debug.log (and the CSV log if it is about logging)


SCREEN PROBLEMS
---------------
Wrong colors or a rolling band means Arch R needs a screen overlay made from
your R36S's original files. Use "Overlay" in the Arch R Flasher, or the
online generator at https://arch-r.io/overlay-generator/, with the .dtb file
from the backup you made in step 1a.


LICENSE
-------
FastECU-R36S is free software under the GNU GPL version 3 (LICENSE.txt).
Its full source code is included: FastECU-R36S-<version>-source.tar.gz.
Changes from the original FastECU are listed in CHANGES-R36S.md.
Bundled libraries and their licenses are listed in THIRD_PARTY/.
