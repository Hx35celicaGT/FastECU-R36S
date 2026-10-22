# FastECU-R36S changes

FastECU-R36S is an unofficial port of [FastECU](https://github.com/miikasyvanen/fastecu-oem)
to the R36S handheld running [Arch R](https://github.com/archr-linux/Arch-R).
It is based on upstream commit `d7198f1` ("Cleanup.", 2026-07-25).

Every modified source file starts with a notice saying it was modified, by whom and when,
as GPLv3 section 5(a) requires. Changed code is marked with `// R36S:` comments.

## Modified files

| File | What changed |
|---|---|
| `log_operations_ssm.cpp` | Most of the port: corrected math for logged values; logging fixes; `log_setup.txt` in `FastECU-Files` chooses what is shown and logged; fixed 6-box live panel with min/max; switch logging; RomRaider extended parameters (`ecuparam`) matched by ECU ID; Arduino Nano as an extra log device (talks to the chip directly when the kernel has no serial driver for it); CSV written to the card about once a second; live CSV viewer (desktop 2); D-pad friendly "Set value" window; pointer that hides when not in use; L1/R1 desktops. |
| `mainwindow.cpp` | Layout for the 640 x 480 screen: 6-box panel (2 across, 3 down), resizable bottom grid, narrower left panel, long ECU text no longer widens the window; L1/R1 desktop switching; buttons ignored while a ROM read/write runs. |
| `menu_actions.cpp` | Guards so Logging/CSV/Connect/Disconnect can't be triggered during a ROM read/write; "Set value" uses the D-pad window and accepts `=value`; `log_setup.txt` is re-read when Logging starts; CSV status box. |
| `file_actions.cpp`, `file_actions.h` | Reads RomRaider extended parameters, storage type and endianness. |
| `logbox.cpp` | Minimum font sizes so labels stay readable on the small screen. |

## Added files

| Path | Purpose |
|---|---|
| `r36s/` | Launcher, packaging script, tester README and build notes for the R36S. |
| `CHANGES-R36S.md` | This file. |
| `.github/ISSUE_TEMPLATE/` | Test report form. |

## History

| Date | Change |
|---|---|
| 2026-09 | Port started: screen layout, logging fixes, log setup file. |
| 2026-10 | Arduino log device, extended parameters, CSV viewer, D-pad Set value window. |
| 2026-10 | Packaging for testers (`r36s/`). |
