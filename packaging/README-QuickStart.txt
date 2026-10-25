Game Traffic Controller - clumsy 0.3 - Quick Start
==================================================

[Run]
1. Double-click clumsy.exe and approve the UAC prompt (administrator
   rights are required to load the WinDivert driver; only this machine's
   network traffic is touched).
2. WinDivert.dll, WinDivert64.sys and rapidocrlite.dll must stay in the
   same folder as clumsy.exe - the program cannot start if any is
   missing. The folder is portable - copy it anywhere, no installer.
   The models\ folder is used only by the optional OCR add-on; ordinary
   packet simulation works without it.

[Basic tab]
1. Application: pick a program by image name (e.g. chrome.exe), or leave
   "All applications" to affect the whole machine. "Refresh" rebuilds the
   process list. Only the selected app is affected; everything else stays
   online.
2. Direction: Both / Inbound (download) / Outbound (upload).
3. Network condition: drag the loss slider or use the presets
   Normal 0%, Severe 95%, Offline 100%.
   - Mid percentages drop packets at random: pages crawl and sometimes
     load;
   - 100% feels like unplugging the cable, including existing connections.
4. Auto-recover: check it and set seconds to stop automatically.
5. Live traffic: blue = download, green = upload; the red dashed line marks
   when Start was pressed. Before start the chart is a read-only listener
   (no traffic is touched).
6. Press the green Start button; press the red Stop button to end it.

[Automatic testing (OCR) - collapsible section 5 on the Basic tab]
An optional helper for QA against YOUR OWN or authorized applications:
clumsy watches small rectangles of a target window and switches the loss
level automatically when configured text appears or disappears (for
example an idle status bar vanishing while you "type", then a reconnect
prompt appearing). Every switch is timestamped in a CSV timeline; clumsy
makes no pass/fail judgment - you analyze the record.
1. Pick the target application, press "Choose window" and select one of
   its top-level windows.
2. Press "Add region (drag)" and rubber-band rectangles directly on the
   target window. Per region fill in: name, keywords (several separated
   by commas; whitespace-insensitive substring match), trigger
   (appear/disappear), confirm frames, target loss %.
3. Start as usual. Before the first rule fires, loss stays at the
   slider's current value; once a rule fires its gear holds until another
   rule fires or you press Stop (manual recovery stays your decision).
- The scrolling log shows human-readable lines; the canonical record is
  the CSV (UTF-8 BOM, opens in Excel without mojibake) under
  %LOCALAPPDATA%\clumsy\ocr-runs\ (the program folder is the fallback).
  "Open folder" selects the running CSV in Explorer.
- Configuration persists in ocr_config.txt next to clumsy.exe; the saved
  window is re-resolved by title at Start (zero or several matches ->
  inline red error, nothing is attached and no CSV is created).
- Known limitation (v1): capture is PrintWindow only. Minimized windows
  and some GPU-rendered / exclusive-fullscreen windows cannot be
  captured; such polls are logged "unavailable", rate-limited, and never
  trigger a gear change. Windows.Graphics.Capture is the planned
  fast-follow.
- Packets that cannot be attributed to the target application are always
  passed through untouched, at every loss percentage and in every mode.
- clumsy ships NO built-in recognition rules for third-party games or
  applications: regions, keywords and gears are entirely user-defined.
- OCR runtime license: RapidOCRLite / RapidAI PP-OCRv6 small models,
  Apache License 2.0, CPU inference only; see LICENSE.RapidOCRLite.txt
  and LICENSE.IndraOCR.txt. Total model size is about 31 MB.

[Advanced tab]
Lag, drop, throttle, duplicate, out-of-order, tamper, RST reset and
bandwidth limit modules plus a WinDivert filter box. Bad filter syntax is
shown as red inline text under the box - no popup dialogs. Advanced
settings are independent of the Basic tab.

[Language]
Use the ZH/EN buttons at the top right at any time, even while running.

[Uninstall]
Stop affecting traffic, close the program, then delete the folder. The
WinDivert driver is loaded temporarily and unloads when the program exits.
