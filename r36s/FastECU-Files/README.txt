FastECU-Files
=============
Everything you add or take off the R36S goes here. This folder is set as
FastECU's folder for ROMs, definitions and datalogs on first start.

  datalogs/                 CSV logs FastECU records
  roms/                     ECU ROM files (.bin) to open, and ROMs you read
  definitions/Romraider/    RomRaider definitions (ECU defs and logger defs, .xml)
  definitions/ECUFlash/     EcuFlash definitions (.xml, in their subfolders)
  log_setup.txt             what the 6-box panel shows and what is logged
                            (created by FastECU; edit on the R36S or here)

Definitions are not included. Download them from the RomRaider or EcuFlash
communities, copy them in, then select them in FastECU: Settings ->
Definitions.

Getting files on and off the R36S:
  - PC cable: run "FastECU PC Transfer On" from Ports, connect a USB-C DATA
    cable (charge-only cables won't work). The PC shows "ArchR Device" ->
    Roms -> ports -> FastECU-R36S -> FastECU-Files. Run "FastECU PC
    Transfer Off" when done.
  - Wi-Fi: turn on Samba in Arch R, open \\archr\games-roms on the PC.
  - Card in a Linux PC: STORAGE partition -> roms/ports/FastECU-R36S.
