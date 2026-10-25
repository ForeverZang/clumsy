# Vendored: RapidOCRLite + PP-OCRv6 small ONNX models

OCR runtime for the OCR-triggered automatic loss feature (branch
`feat/ocr-window-auto-loss`, spike gate 2026-10-25). Everything in this
directory is third-party; clumsy links it via a stable C ABI and never
modifies it.

## Runtime

- **Package:** RapidOCRLite SDK `win_amd64-5.0.0a15` (RapidAI).
- **Engine reality:** ONNX models executed by the bundled **indra**
  runtime (CPU; Vulkan GPU backends exist in the SDK but are not used,
  see Rulings below). This is **not** the ncnn/INT8 stack assumed by the
  original design doc; the SDK this version actually ships is
  ONNX/indra. Gate results were measured against this real stack and
  pass, so the design's accuracy/latency intent is met by a different
  implementation.
- **Files kept:** `include/rapidocrlite/rapidocrlite.h` (C ABI),
  `export.h`, `bin/x64/rapidocrlite.dll` (3.4 MB, MSVC build; x64
  exports are undecorated and mingw-w64 gcc links the DLL directly).
- **Files not vendored:** `rapidocrlite.lib` (MSVC import lib),
  `rapidocrlite_static.lib` (40 MB, MSVC `/MT`), C++ wrapper
  `rapidocrlite.hpp`, cmake package. Product builds with gcc only.
- `rapidocrlite.dll` imports only `KERNEL32.dll` — the ONNX/indra
  runtime is statically linked; no additional runtime DLLs are needed.

## Models

From the RapidAI/RapidOCR v3.10.0 model distribution (ModelScope),
current multilingual ch_PP-OCRv6_small equivalents:

| File | Size | SHA-256 |
| --- | --- | --- |
| `models/PP-OCRv6_det_small.onnx` | 9,929,594 | `090f04abcd9d9a7498bc4ebf677e4cb9bdce1fe4197ddb7e529f1ef44e1ff94f` |
| `models/PP-OCRv6_rec_small.onnx` | 21,234,383 | `6f327246b50388f3c176ae304bd95767ea6dc0c9ae92153ef8cbe210b3c14884` |
| `models/ppocrv6_dict.txt` | 74,947 | `b5f2bfe2bdd9448429e3e82b51c789775d9b42f2403d082b00662eb77e401c5d` |

Det/rec hashes match the upstream `default_models.yaml` manifest.
The alternative "lite/indra" weights were rejected: they carry a
modified OpenRAIL-M license (revenue/funding and non-compute clauses).
These PP-OCRv6 ONNX weights are distributed by RapidAI under the
Apache-compatible route.

## Licenses

Apache License 2.0: `LICENSE` (RapidOCRLite) and
`LICENSE-indra-ocr` (bundled indra OCR runtime; text identical in this
SDK release).

## Spike gate results (Intel UHD 770 class machine, CPU backend)

- Engine create: ~130–175 ms (once per Start).
- Synthetic status strip "Online 仓库 12:30" and centered bold "重连",
  GDI+ ClearType, 14–48 px glyph sizes: 20/20 correct at every size,
  confidence 0.94–1.00, no false positives on blank frames.
- Region-sized OCR latency: status strip p50 25.6 ms, reconnect p50
  53.4 ms (gate was p50 ≤ 250 ms).
- Repeated create→recognize→destroy cycles exit cleanly on CPU.

## Rulings

1. **CPU only.** `rpocr_query_backend_info` and the Vulkan/hybrid
   backends crash at process exit (0xC0000409) on this machine even
   after a clean destroy. Product code never queries backend info and
   always inits `RPOCR_BACKEND_CPU`.
2. **build.zig does not build OCR.** No zig toolchain is maintained on
   this machine (the shipping builds are the gcc scripts); linking an
   MSVC-built DLL under `zig cc` was therefore never verified.
   build.zig compiles all sources with `CLUMSY_OCR=0` (no-op stubs),
   for both x86 and x64. OCR ships only in the gcc x64 debug/release
   builds.
3. **Capture is PrintWindow-only in v1**
   (`PW_CLIENTONLY | PW_RENDERFULLCONTENT`). Classic GDI windows
   capture correctly even when fully occluded (pixel-identical to the
   unoccluded frame); minimized windows fail (`PrintWindow` returns 0
   and/or zero-variance black frame) and must be reported as
   `capture_unavailable`. DirectX/Unity-style windows were not
   exercised in the spike; a black frame there is likewise detected by
   the variance guard. Windows.Graphics.Capture is a fast-follow
   (mingw ships the interop headers, but activation needs manual WinRT
   plumbing from C).
4. **Known recognition blind spot:** classic GDI `DrawText` with
   Microsoft YaHei UI at small text heights (~14–22 px ink) is
   misread by `rec_small` under every font quality mode and after
   bicubic upscale; modern rasterizers (GDI+/WPF/Qt/CEF/Unity/emulator
   stacks) read perfectly at the same sizes. Failure direction is
   safe-ish: misreads look like "keyword absent", so a rule that never
   reads its keyword simply never arms (fail-closed); the residual
   risk (intermittent misread after arming causing a false disappear
   trigger) is mitigated by deterministic per-pixel recognition,
   change-detect gating, and confirm-N frames. Users validate their
   picked region via the live OCR text in the scrolling log before
   relying on a rule.
