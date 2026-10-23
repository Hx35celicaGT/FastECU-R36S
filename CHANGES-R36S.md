# FastECU-R36S changes

FastECU-R36S is an unofficial port of [FastECU](https://github.com/miikasyvanen/fastecu-oem)
to the R36S handheld running [Arch R](https://github.com/archr-linux/Arch-R).
It is based on upstream commit `d7198f1` ("Cleanup.", 2026-07-25).

Every modified source file starts with a notice saying it was modified, by whom and when,
as GPLv3 section 5(a) requires. Changed code is marked with `// R36S:` comments.

## Modified files

| File | What changed |
|---|---|
| `log_operations_ssm.cpp` | Most of the port: corrected math for logged values; logging fixes; `log_setup.txt` in `FastECU-Files` chooses what is shown and logged; "Log values" window with warning limits and math channels; full-screen 6-box live panel with min/max; switch logging; RomRaider extended parameters (`ecuparam`) matched by ECU ID; Arduino Nano as an extra log device (talks to the FTDI chip directly when the kernel has no serial driver); CSV written to the card about once a second; CSV viewer with live graph (desktop 2); D-pad friendly "Set value" window; pointer that hides when not in use; L1/R1 desktops; 3D graph turned with drag or arrow keys. |
| `mainwindow.cpp`, `mainwindow.h` | Layout for the 640 x 480 screen: 6-box panel (2 across, 3 down), status bar kept on screen, narrower left panel, long ECU text no longer widens the window, centred splash; L1/R1 desktop switching; buttons ignored while a ROM read/write runs; map window menu (Copy Table, Paste Table, Send To Graph). |
| `menu_actions.cpp` | Copy/Paste Table and Send To Graph; guards so Logging/CSV/Connect/Disconnect can't be triggered during a ROM read/write; "Set value" uses the D-pad window and accepts `=value`; `log_setup.txt` is re-read when Logging starts; CSV status box. |
| `file_actions.cpp`, `file_actions.h` | Reads RomRaider extended parameters, storage type and endianness. |
| `logbox.cpp` | Minimum font sizes so labels stay readable on the small screen. |

## Added files

| Path | Purpose |
|---|---|
| `r36s/` | Launcher, packaging script, tester README and build notes for the R36S. |
| `r36s/FastECU-Files/`, `r36s/fastecu.cfg.template` | Folder layout for ROMs, definitions and datalogs, and first-start settings pointing FastECU at it. |
| `r36s/FastECU-Data-Transfer-OFF.sh` | One-button toggle for Arch R's USB file transfer (MTP); renames itself to show ON/OFF. |
| `r36s/gamepad_mouse/` | Turns the R36S buttons and sticks into a mouse and keyboard for FastECU. |
| `CHANGES-R36S.md` | This file. |
| `.github/ISSUE_TEMPLATE/` | Test report form. |

## History

The full history is in the git log (`git log d7198f1..`). In short:

| Date | Change |
|---|---|
| 2026-09-28 | Port baseline: screen layout, logging fixes, log setup file, Log values window. |
| 2026-09-30 to 10-02 | Arduino Nano log device; status bar and CSV box; Set value window; L1/R1 desktops. |
| 2026-10-03 | CSV viewer on desktop 2; pointer hides when not in use. |
| 2026-10-06 | `FASTECU_FILES` sets the Files folder. |
| 2026-10-21 | Copy/Paste Table, Send To Graph; turn the 3D graph. |
| 2026-10-22 | Warning limits and math channels in Log values; live CSV graph; packaging for testers (`r36s/`). |
