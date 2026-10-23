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
c) Download and open the Arch R Flasher for your PC:
     https://github.com/archr-linux/archr-flasher/releases
d) Go through its 5 steps: Console (Original / Clone / Soysauce) ->
   Image (Download latest version) -> Overlay (your screen panel file) ->
   SD Card -> Flash. Leave "Verify after writing" ticked.
e) Put the card in the R36S and turn it on. The first start shows setup
   messages for 1-2 minutes while the card is prepared. Wait for the menu.
If the screen shows wrong colors or a rolling band, see "SCREEN PROBLEMS" below.


2. COPY FASTECU-R36S ONTO THE R36S
----------------------------------
Arch R keeps games and ports on a Linux (ext4) partition called STORAGE.
Windows and Mac can't open it directly, so pick the way that fits you.
In every case, copy these from the zip into the "ports" folder:
     FastECU.sh
     FastECU PC Transfer On.sh
     FastECU PC Transfer Off.sh
     FastECU-R36S   (folder)

A) Over Wi-Fi (Windows, Mac, Linux) - needs a USB Wi-Fi adapter on the R36S
   On the R36S: Network Settings -> Network Services -> Enable Samba.
   On the PC, open the share:
     Windows:  \\archr\games-roms   (in File Explorer's address bar)
     Mac:      Finder -> Go -> Connect to Server -> smb://archr.local/games-roms
     Linux:    smb://archr.local/games-roms
   Open "ports" and copy the files in.

B) Card in a Linux PC
   Turn the R36S off, put the card in the PC, open the STORAGE partition,
   then roms -> ports, and copy the files in (as root if it asks).
   Eject the card properly before taking it out.

C) Card in a Windows or Mac PC, no Wi-Fi
   Install an ext4 driver first (for example Paragon "Linux File Systems"),
   then do the same as B.
   WINDOWS: if it asks "You need to format the disk", click CANCEL.
   Formatting erases the card.

After copying: in the R36S menu, Start -> Game Settings -> Update gamelists,
or restart the R36S.


3. START IT
-----------
Go to Ports, start FastECU. The menu closes and FastECU opens full screen.
Plug the OBD cable into the R36S with the OTG adapter, then into the car.
Turn the ignition on.

The sticks move the mouse pointer. To quit: File -> Quit.
If FastECU stops responding, hold SELECT + START to force it closed.
Either way the Arch R menu comes back.


4. GETTING FILES ON AND OFF (logs, ROMs, definitions)
-----------------------------------------------------
Everything is in ports/FastECU-R36S/FastECU-Files (see README.txt there).
With a PC cable:
  a) Ports -> "FastECU PC Transfer On".
  b) Connect the R36S to the PC with a USB-C DATA cable (charge-only cables
     don't work). The PC shows "ArchR Device".
  c) Open Roms -> ports -> FastECU-R36S -> FastECU-Files.
  d) When done, unplug and run Ports -> "FastECU PC Transfer Off" so the
     port works with the OBD cable again.
Or use Wi-Fi (Samba) as in step 2A.


WHERE YOUR FILES ARE
--------------------
All inside ports/FastECU-R36S:
  FastECU-Files/       datalogs, ROMs, definitions, log_setup.txt
  userdata/            FastECU's own settings
  fastecu-debug.log    what happened on the last run, including which USB
                       cables the R36S saw (attach to bug reports)


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
