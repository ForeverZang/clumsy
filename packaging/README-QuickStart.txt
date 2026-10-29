Game Traffic Controller - clumsy 0.4.1 - Quick Start
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
clumsy watches small rectangles of one of the target app's windows and
switches the loss level automatically when configured text appears or
disappears (for example an idle status bar vanishing while you "type",
then a reconnect prompt appearing). Every switch is timestamped in a CSV
timeline; clumsy makes no pass/fail judgment - you analyze the record.
1. Pick the target application above, then press "Choose window...": the
   current screen dims. With one visible app window it is highlighted
   immediately; with several, hover to highlight each and click once to
   lock. Only this application's own top-level windows are clickable -
   other programs' windows can never be framed. Esc, right-click, or
   "Cancel" exits without changes. If the target window closes during
   selection, the picker exits with a red error and nothing is saved.
2. Drag one or more rectangles inside the highlighted window (boxes
   smaller than 10 px are ignored). The floating toolbar offers Undo
   last box, Choose another window (boxes drawn this pass are dropped;
   previously saved regions are kept), Cancel, and Done (shows the box
   count; greyed out until at least one box exists on a fresh pick; on
   later add-region passes you may finish with zero new boxes to keep the
   old ones).
3. The fixed-height "OCR regions" table lists named rectangles
   (double-click a name to rename). The "Rule groups" table holds groups
   of one or more members - double-click a row to edit: within a group
   ALL member regions must match at once (AND), while comma-separated
   keywords in one member are alternatives (OR). Per group set
   appear/disappear, confirm frames, and target loss %. Deleting a region
   removes it from every group and reindexes the rest; a group left
   empty shows an inline error and Start is refused.
4. The full-width green bar at the bottom of the window is the ONLY
   Start entry ("Start affecting" / "Stop affecting"); with section 5
   enabled it also runs the OCR preflight. Before the first group fires,
   loss stays at the slider value; a fired gear holds until another group
   fires or you press Stop. When several groups fire on the same OCR tick,
   the first group in the table sets the gear and each fired group gets
   its own CSV row. If one member region cannot be captured that tick,
   the group freezes instead of firing, so one occluded or misread region
   never triggers by itself.
- The scrolling log shows human-readable lines; the canonical record is
  the CSV (UTF-8 BOM, opens in Excel without mojibake) under
  %LOCALAPPDATA%\clumsy\ocr-runs\ (the program folder is the fallback).
  "Open folder" selects the running CSV in Explorer.
- Configuration persists in ocr_config.txt next to clumsy.exe; the saved
  window is re-resolved by title at Start (zero or several matches ->
  inline red error, nothing is attached and no CSV is created). Configs
  written by clumsy 0.3.1 (rule.N.* rows) migrate automatically on
  startup and are rewritten in the new regions/groups format after the
  first Start.
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
