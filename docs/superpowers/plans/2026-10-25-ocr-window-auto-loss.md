# OCR 触发自动丢包 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在基础功能页新增折叠的「自动测试（OCR）」块：框选被测（自有）应用窗口区域，按文字出现/消失沿边自动切换丢包档位，并记录 CSV 时间线与页面滚动日志。

**Architecture:** 纯逻辑分层（规则状态机、CSV、归一化坐标/变化检测、窗口重解析、会话编排）全部 C + 单元测试；抓帧（v1 仅 PrintWindow）与 RapidOCRLite 识别在单一后台线程执行，经互斥锁事件队列送到 IUP 主线程；UI 为独立 `ui_ocr.c`/`ui_picker.c`，通过一个 loss applier 回调调用现有 `simpleSetLoss()` 切档，不改包处理与安全铁律。

**Tech Stack:** C11 (gnu11, msys2 mingw64 gcc), IUP 3.30, WinDivert 2.2, RapidOCRLite ncnn（`c_api.h` 稳定 C binding，ch_PP-OCRv6_small INT8，无 cls），Win32 PrintWindow（Windows.Graphics.Capture 列为 v1 之后快跟进，本期不写产品代码）。

**Spec:** `docs/superpowers/specs/2026-10-25-ocr-window-auto-loss-design.md`（计划与 spec 冲突以 spec 为准；冲突在 SDD ledger 记 Ruling）。

## Global Constraints

- 业务代码全部 C（gnu11）；v1 抓帧仅 PrintWindow，全黑/最小化/独占画面如实记 `capture_unavailable`；WGC 兜底是 v1 之后的快跟进，本期不写产品代码（用户 2026-10-25 决定）。
- 构建范围：OCR 仅 x64；gcc debug/release 脚本为权威路径并加 `-DCLUMSY_OCR=1`；build.zig x64 同步加入新源文件并尝试链接（可行性由 Task 1 spike 用 zig cc 验证，不可行则同样以宏裁掉并记 Ruling）；x86 目标以 `CLUMSY_OCR=0` 编译，`ui_ocr.c` 禁用时整体退化为 no-op 桩（`uiOcrCreate` 返回 NULL），main.c 调用点以宏保护。
- 文件 ≤800 行、函数 ≤50 行；每个 malloc/calloc 返回值检查；错误显式传播，不静默吞。
- 所有 UI 文案走 i18n key（新增前缀 `ocr.*`，中/英成对）；LOG 与 CSV 表头/事件名英文。
- 安全铁律不变：无法归属到目标应用的包一律放行；OCR 只改 `simpleSetLoss` 的档位，不动 shouldAffect/包路径。
- 不内置任何第三方应用的区域/关键词；所有规则由使用者输入。
- 产品构建 `SIGN=A scripts/build-debug.sh`（gcc 走 `/c/msys64/usr/bin/bash -lc`）；测试 `scripts/run_tests.sh`，新增 TU 必须登记进脚本。
- conventional commits；中文设计，提交信息英文。
- 规则触发后档位保持，不自动回退；抓不到画面记 unavailable，绝不假装识别成功。

## Review Focus

1. **模型/引擎缺失或 ABI 不兼容**：models 目录缺 det/rec、版本不匹配、OCR 初始化失败 → 开始时块内红字内联错误、WinDivert 不挂接、不崩溃。Task 5 fake-init 单测 + Task 12 手动删模型验收。
2. **OCR 文本含干扰**：识别成“重 连”“正在重连…”“RECONNECT”等 → 去全部空白、大小写不敏感、子串包含即命中（Task 2 单测钉死）；“車库”等近形误识仍可能漏，由确认帧数与事件截图（默认关）兜底，不在首版承诺。
3. **高 DPI 与运行中窗口缩放/移动**：归一化坐标按当前客户区与 DPI 每帧重算（Task 4 单测 100%/150% 缩放两组）；窗口最小化/关闭 → unavailable 事件限频、不崩、不错误触发（Task 6 单测）。
4. **200ms 间隔慢于单帧 OCR**：单工作线程，上一轮没结束就跳过本轮（不叠加线程/不积压），事件时间线时间戳真实（Task 6 fake 慢引擎单测）。
5. **“不限制应用”下启用 OCR 与非法规则输入**：应用为 NULL 时启用勾选被拒并内联提示（Task 8）；目标档位非数字/越界、确认帧数 <1、关键词为空 → 行级红字或钳制，开始被拒（Task 7 + Task 8）。
6. **持久化窗口失效**：保存的窗口标题在开始时 0 匹配（窗口关闭/标题变化）或多匹配（两个同名窗口）→ 开始被拒、内联红字提示重新选择，不建时间线、不挂 WinDivert（Task 4 纯函数 `captureResolveWindow` 单测 + Task 12 手动关窗/双开验收）。

---

### Task 1: RapidOCRLite + 抓帧 spike（抛弃型门槛验证）

**Files:**
- Create（spike，不进产品树）: `build/spike-ocr/ocr_probe.c`, `build/spike-ocr/cap_probe.c`
- Vendor 目标目录（spike 通过后才保留并提交）: `external/RapidOCRLite/`

**Interfaces:**
- Produces（结论写入 SDD ledger，后续任务依赖）: C 头文件名与 init/recognize 签名、静态库/动态库文件清单与链接选项、模型文件名与总字节数、许可名称、三张样例图的单帧耗时区间、PrintWindow/WGC 在 Unity/D3D11 窗口上的成败。

- [x] **Step 1: 取得 RapidOCRLite 官方产物**

定位官方 RapidOCRLite（ncnn 内核，提供 `c_api.h`）发布包或源码：头文件、x64 可用的预编译库（或可由 mingw/cmake 构建的源码）、ch_PP-OCRv6_small INT8 的 det/rec 模型；放入 `external/RapidOCRLite/`（`include/`、`lib/`、`models/`、`LICENSE`）。记录来源 URL 与许可证到 ledger。

- [x] **Step 2: OCR 探针**

`build/spike-ocr/ocr_probe.c`：纯 C，用 `c_api.h` 初始化（无 cls），依次识别三张样例图：状态栏“仓库”、中央“重连”、无文字区域。样例图放 `build/spike-ocr/samples/`（可用画图生成或被测程序截图）。msys gcc 编译链接（命令记入 ledger），打印：初始化 ms、每图 det+rec ms × 20 次的中位数与最大、20 次文本结果、模型总大小。另用 **zig cc x64** 对同一静态库做一次最小链接探测（build.zig 路径可行性）。

Expected: “仓库”“重连”各 ≥18/20 次包含目标词（去空白后子串），无文字图不误报这两个词；区域图（约 300×80）中位耗时 ≤250ms；gcc 链接成功。任一不满足：在 ledger 记 Ruling 并 STOP，带回重新选栈，不继续 Task 5 之后的接线。zig cc 链接失败不 STOP：记 Ruling「build.zig 同样以 CLUMSY_OCR 宏裁掉 OCR」，Task 11 按此执行。

- [x] **Step 3: 抓帧探针**

`build/spike-ocr/cap_probe.c`：对一个 HWND 试 `PrintWindow(PW_CLIENTONLY|PW_RENDERFULLCONTENT)`；窗口遮挡、移动、最小化三种状态各存 PNG；记录全黑帧（方差 0）出现条件。另做一次 WGC（`IGraphicsCaptureItemInterop`）**信息性**编译探测，只记录 mingw gcc 下缺什么，不作为门槛（v1 范围已定为 PrintWindow-only，用户 2026-10-25 拍板）。

Expected: 窗口化普通 Win32/WinForms/WPF 窗口遮挡下 PrintWindow 出图非全黑；最小化失败与全黑能被调用方检测（返回失败或方差 0）。D3D11/Unity 窗口若出全黑：记入 ledger 与发布说明限制，运行时走 `capture_unavailable`，不阻塞。

- [x] **Step 4: Commit**

仅当 spike 通过：`external/RapidOCRLite/` 中体积允许的二进制/模型（若模型过大，记录替代分发方式并先不提交，ledger 说明），以及一条 ledger 结论。

```bash
git add external/RapidOCRLite .superpowers/sdd/<workspace>/progress.md
git commit -m "chore: vendor RapidOCRLite ncnn runtime and v6_small INT8 models"
```

### Task 2: 规则状态机与文本匹配（纯逻辑）

**Files:**
- Create: `src/ocr_rules.h`, `src/ocr_rules.c`
- Test: `tests/test_ocr_rules.c`
- Modify: `scripts/run_tests.sh`（新增 `ocr_rules` TU，SRC=`src/ocr_rules.c`，TESTS 列表追加）

**Interfaces:**
- Produces:
```c
typedef enum { OCR_EDGE_APPEAR = 0, OCR_EDGE_DISAPPEAR = 1 } OcrEdge;
typedef struct {
    char     name[32];
    char     keywords[256];  // 逗号分隔，子串 OR
    OcrEdge  edge;
    int      confirmFrames;  // >=1
    int      lossPercent;    // 0..100
} OcrRule;
typedef enum { OCR_STABLE_ABSENT = 0, OCR_STABLE_PRESENT = 1 } OcrStable;
// armed=FALSE：尚未拿到第一次成功 OCR，任何边都不触发（防首帧失败误判“消失”）
typedef struct { OcrStable stable; int candidate; int run; BOOL armed; } OcrRuleState;
typedef enum { OCR_POLL_MISSED = 0, OCR_POLL_MATCHED = 1,
               OCR_POLL_UNAVAILABLE = 2 } OcrPoll;

void ocrRuleStateInit(OcrRuleState *st);              // 起始 unarmed
// 第一次成功 OCR 后调用：presentNow=本次是否命中，据此设定稳定态并武装
void ocrRuleArm(OcrRuleState *st, BOOL presentNow);
// 去首尾空白、内部全部空白、ASCII 小写；返回是否有非空内容
BOOL ocrNormalizeText(const char *in, char *out, size_t cap);
// 关键词切分（逗号，支持中文逗号）后逐个归一化，子串 OR
BOOL ocrRuleMatches(const OcrRule *rule, const char *rawText);
// 沿边触发返回 TRUE 且 *targetPercent=rule->lossPercent；unavailable 不改 run
BOOL ocrRuleStep(OcrRuleState *st, const OcrRule *rule,
                 OcrPoll poll, int *targetPercent);
```

- [x] **Step 1: 写失败测试** `tests/test_ocr_rules.c`（AAA，main 末尾打印 `OK: ocr_rules`）

断言：normalize `" 重 连 "`→`"重连"`、`"Reconnect!"` 小写保留符号；匹配用例 `"正在重连，请稍候"` 命中关键词 `"重连"`、`"RECONNECT"` 命中 `"reconnect"`、`"仓库2级"` 命中 `"仓库"`、空关键词不命中；状态机用例：**未武装时连续 MISSED/UNAVAILABLE 任意次都不触发（钉死首帧失败防护）**；`ocrRuleArm(st, TRUE)` 后 DISAPPEAR 规则处于 PRESENT，连续 2 次 MISSED 第二次触发一次且 `*targetPercent` 为规则值，再来 MISSED 不重复；`ocrRuleArm(st, FALSE)` 后 APPEAR 规则处于 ABSENT，连续 confirmFrames=2 次 MATCHED 第二次触发；中间插 UNAVAILABLE 不计入 run、不翻转；MIXED 打断 run 重新计数；trigger 后 stable 翻转。

- [x] **Step 2: 验证失败**

Run: `scripts/run_tests.sh`（先在脚本登记 TU）
Expected: BUILD FAIL `ocr_rules`（头文件不存在）

- [x] **Step 3: 实现** `src/ocr_rules.c`

切词用静态缓冲（关键词切分 ≤16 个、每个归一化后 ≤128 字节）；不分配堆内存。

- [x] **Step 4: 验证通过**

Run: `scripts/run_tests.sh`
Expected: `PASS: ocr_rules`，其余 11 个 TU 不受影响全绿。

- [x] **Step 5: Commit**

```bash
git add src/ocr_rules.h src/ocr_rules.c tests/test_ocr_rules.c scripts/run_tests.sh
git commit -m "feat: OCR rule edge-trigger state machine and text matching"
```

### Task 3: CSV 时间线

**Files:**
- Create: `src/ocr_timeline.h`, `src/ocr_timeline.c`
- Test: `tests/test_ocr_timeline.c`
- Modify: `scripts/run_tests.sh`

**Interfaces:**
- Consumes: `common.h`（MSG_BUFSIZE）
- Produces:
```c
typedef struct OcrTimeline OcrTimeline;
typedef struct {
    UINT64     timeMs;       // 会话相对毫秒
    const char *event;       // session_start/loss_change/rule_trigger/...
    const char *region;
    const char *ocrText;
    const char *keyword;
    const char *edge;        // appear/disappear 或 NULL
    int         lossPercent; // -1 = 不适用
    const char *source;      // initial_slider / rule:<name> / manual_stop
    const char *note;
} OcrTimelineEvent;
OcrTimeline *ocrTimelineOpen(const char *dirHint, char *pathOut, size_t pathCap,
                             char err[MSG_BUFSIZE]);
void ocrTimelineClose(OcrTimeline *tl);
BOOL ocrTimelineEvent(OcrTimeline *tl, const OcrTimelineEvent *ev);
// 纯函数：逗号/引号/CRLF 按 RFC4180 双引号包裹
void ocrCsvEscape(const char *field, char *out, size_t cap);
// %LOCALAPPDATA%\clumsy\ocr-runs，失败回退 exe 目录；失败返回 FALSE 并填 fallback
BOOL ocrTimelineDefaultDir(char *out, size_t cap, char *fallback, size_t fbCap);
```
固定表头：`time_ms,time_local,event,region,ocr_text,keyword,edge,loss_percent,source,note`；UTF-8 BOM；`time_local` 用本地 `YYYY-MM-DD HH:MM:SS.mmm`。

- [x] **Step 1: 写失败测试**

断言 `ocrCsvEscape`：普通串原样；含逗号/双引号/换行的包裹与引号翻倍；NULL→空串。`ocrTimelineOpen/Event/Close` 用 `dirHint` 指向 `build/tests/` 下临时目录：文件存在、首行 BOM+表头、写入两行后读回列数与 loss_percent 一致（-1 写空）。`ocrTimelineDefaultDir` 在测试环境设置 LOCALAPPDATA 临时目录后路径以 `ocr-runs` 结尾，目录被创建。

- [x] **Step 2: 验证失败**：BUILD FAIL `ocr_timeline`。
- [x] **Step 3: 实现**（CreateDirectory 逐级；fopen "utf-8-sig" 等价：手写 `\xEF\xBB\xBF`；每事件 fflush）。
- [x] **Step 4: 验证通过**：`PASS: ocr_timeline`，全量绿。
- [x] **Step 5: Commit**

```bash
git add src/ocr_timeline.h src/ocr_timeline.c tests/test_ocr_timeline.c scripts/run_tests.sh
git commit -m "feat: OCR session CSV timeline with RFC4180 escaping"
```

### Task 4: 抓帧纯逻辑 + 窗口枚举

**Files:**
- Create: `src/ocr_capture.h`, `src/ocr_capture.c`
- Test: `tests/test_ocr_capture.c`
- Modify: `scripts/run_tests.sh`（链接 `src/ocr_capture.c src/procmap.c`）

**Interfaces:**
- Consumes: `procmap.h` AppGroup pids
- Produces:
```c
typedef struct { double x, y, w, h; } NormRect; // 客户区比例 0..1
typedef struct { HWND hwnd; DWORD pid; char title[256]; int width, height; } CaptureWindow;
int  captureEnumWindowsForApp(const char *exeName, CaptureWindow *out, int cap);
// 标题匹配等级：0=无 1=规范化后子串 2=完全相等（trim、大小写不敏感）
int  captureTitleMatchLevel(const char *saved, const char *candidate);
// 在候选数组中挑唯一窗口：matchCount=0 无、1 唯一（优先完全相等）、>1 歧义
BOOL captureResolveWindow(const CaptureWindow *cands, int candCount,
                          const char *savedTitle, CaptureWindow *out, int *matchCount);
RECT captureMapNorm(int clientW, int clientH, const NormRect *r);
void captureMakeThumbnail(const unsigned char *bgra, int w, int h,
                          int thumbW, unsigned char *grayOut, int *outTh,
                          int *outThumbH);
BOOL captureChanged(const unsigned char *grayA, const unsigned char *grayB,
                    int pixels, int madThreshold);
// 实时部分（手动验收，无单测）：
BOOL captureClientRegion(HWND hwnd, const RECT *sub, unsigned char **outBgra,
                         int *outW, int *outH, char err[MSG_BUFSIZE]);
void captureFreeBitmap(unsigned char *bgra);
BOOL captureIsCapturable(HWND hwnd); // 存在/可见/非 iconic
```

- [x] **Step 1: 写失败测试**

`captureMapNorm`：1000×800 客户区 rect{0.6,0.85,0.35,0.12} → 像素 RECT 与容差 1；150% 缩放由调用方传物理像素，再给一组 1500×1200 断言比例一致；越界比例钳制在客户区内。`captureTitleMatchLevel`：`"记事本"` vs `"无标题 - 记事本"`=1 子串、vs `"记事本"`=2 完全、vs `"chrome"`=0、空串/NULL=0。`captureResolveWindow`：0 候选→matchCount=0；一个完全相等 + 若干子串时取完全相等 matchCount=1；仅两个子串→matchCount=2 且 out 不写；单子串→1 并写 out。`captureMakeThumbnail`：构造 4×4 已知灰度（B=G=R）BGRA 缓冲，thumbW=2，输出灰度均值正确、outTh 按比例。`captureChanged`：相同缓冲 FALSE；改一个像素且 MAD 超阈值 TRUE；阈值边界（等于阈值 FALSE）。

- [x] **Step 2: 验证失败**：BUILD FAIL `ocr_capture`。
- [x] **Step 3: 实现**

EnumWindows 回调按 AppGroup（procmapEnumApps 找到 exeName 组）pid 集合过滤，要求顶层、WS_VISIBLE、有标题或尺寸 ≥100×100；PrintWindow 用 PW_CLIENTONLY|PW_RENDERFULLCONTENT（0x00000002），BGRA 来自 GetDIBits 32bit；全黑判定留给 Task 6 调用方（方差/与上一帧比较）。

- [x] **Step 4: 验证通过**：`PASS: ocr_capture`，全量绿。
- [x] **Step 5: Commit**

```bash
git add src/ocr_capture.h src/ocr_capture.c tests/test_ocr_capture.c scripts/run_tests.sh
git commit -m "feat: window enumeration, normalized rects and frame change detection"
```

### Task 5: RapidOCRLite 引擎桥

**Files:**
- Create: `src/ocr_engine.h`, `src/ocr_engine.c`（仅此文件 include RapidOCR `c_api.h`）
- Modify: `scripts/build-debug.sh`, `scripts/build-release.sh`（新增 `-DCLUMSY_OCR=1`、external include/lib 与链接行）；build.zig 的登记统一在 Task 11 按 spike 结论处理
- Manual: `build/spike-ocr/engine_smoke.c`（不提交）

**Interfaces:**
- Produces:
```c
typedef struct OcrEngine OcrEngine;
OcrEngine *ocrEngineCreate(const char *modelsDir, char err[MSG_BUFSIZE]);
void ocrEngineDestroy(OcrEngine *);
// 裁剪好的单区域 BGRA；所有文本框按 y 再 x 排序拼接，UTF-8，以 '\0' 结
BOOL ocrEngineRead(OcrEngine *, const unsigned char *bgra,
                   int w, int h, char *out, size_t cap, char err[MSG_BUFSIZE]);
```
无 cls；精确模式参数按 Task 1 ledger 固化为常量。

- [x] **Step 1: 引擎烟雾测试（手动）**

`engine_smoke.c` 调 `ocrEngineCreate(external/RapidOCRLite/models)` 读 Task 1 三张样例，复现 spike 数字。
Expected: 与 spike 一致，缺失模型时返回 NULL + err 英文说明。

- [x] **Step 2: 实现桥**

init 失败、recognize 空图/超宽防御；库若是动态链接，DLL 复制进构建输出目录（debug+release 脚本各加 cp）。

- [x] **Step 3: 产品构建**

Run: `SIGN=A scripts/build-debug.sh`
Expected: 链接通过无 warning 新增；`build/mingw64-debug/` 内 DLL/模型就位（模型是否复制由 Task 1 结论决定大小策略）。

- [x] **Step 4: 全量测试**：11+3=14 个 TU 全绿（引擎不进任何测试 TU，避免测试依赖模型）。
- [x] **Step 5: Commit**

```bash
git add src/ocr_engine.h src/ocr_engine.c scripts/build-debug.sh scripts/build-release.sh build.zig
git commit -m "feat: RapidOCRLite engine bridge for region text recognition"
```

### Task 6: 会话编排（纯逻辑，注入引擎/抓帧）

**Files:**
- Create: `src/ocr_session.h`, `src/ocr_session.c`, `src/ocr_worker.h`, `src/ocr_worker.c`
- Test: `tests/test_ocr_session.c`
- Modify: `scripts/run_tests.sh`（SRC=`src/ocr_session.c src/ocr_rules.c src/ocr_capture.c src/ocr_timeline.c`）

**Interfaces:**
- Consumes: Task 2 规则、Task 3 时间线、Task 4 抓帧与缩略图、Task 5 引擎
- Produces:
```c
typedef struct {
    OcrRule rule;     // 含 NormRect（给 OcrRule 增字段 norm，本任务加在 ocr_rules.h）
    OcrRuleState state;
    unsigned char thumb[/*32宽缩略图，定长缓冲由 session 分配*/];
} OcrRegion;
typedef struct {
    int      intervalMs;     // >=200
    BOOL     changeDetect;
    int      madThreshold;
    CaptureWindow target;    // hwnd
    OcrTimeline *timeline;
    size_t   regionCount;
    OcrRegion *regions;
    // 注入点（测试用 fake；生产传真实函数）
    BOOL (*captureFn)(HWND, const RECT*, unsigned char**, int*, int*, char[MSG_BUFSIZE]);
    BOOL (*readFn)(OcrEngine*, const unsigned char*, int,int, char*, size_t, char[MSG_BUFSIZE]);
    OcrEngine *engine;
    UINT64 (*nowMsFn)(void);
} OcrSession;
typedef struct { int regionIndex; int lossPercent; const char *event; const char *text; } OcrSessionEvent;
// 开始时同步逐区域初扫：成功区域 ocrRuleArm(presentNow)；失败区域保持 unarmed，
// 之后 pump 中第一次成功读取时补武装（武装前绝不触发）
BOOL ocrSessionSeed(OcrSession *, char err[MSG_BUFSIZE]);
BOOL ocrSessionPump(OcrSession *, OcrSessionEvent *ev, int *evCount, int evCap);
// worker：单线程，上一轮未结束则跳过本轮；事件经临界区队列送主线程
typedef void (*OcrEventSink)(const OcrSessionEvent *ev);
BOOL ocrWorkerStart(OcrSession *, OcrEventSink, char err[MSG_BUFSIZE]);
void ocrWorkerStop(void);
```

- [x] **Step 1: 写失败测试**

用 fake capture/read 编排文本序列：
(a) 区域先“仓库”×3（seed 基线 PRESENT），随后“”×2（confirmFrames=2）→ 第二个空帧产生一条 rule_trigger + loss 95；
(b) 再喂“重连”×2（另一区域 APPEAR）→ loss 50，事件顺序按区域索引；
(c) 连续 unavailable 帧不触发、不重置 run；
(f) seed 阶段全部 unavailable，随后 2 次空文本 MISSED → **不触发**；再喂一次“仓库”成功读取（补武装 PRESENT），随后“”×2 → 触发一次 loss 95；
(d) `changeDetect=TRUE` 时相同像素帧跳过 readFn（用调用计数断言）；
(e) fake read 耗时超过 interval 两次：pump 由测试手动驱动不等待，断言 worker 层用一个立即返回的慢 fake + nowMsFn 验证“忙时跳过”标志（通过 ocrWorker 忙标志或事件时间戳不重叠——取可断言的忙标志接口 `BOOL ocrWorkerBusy(void)` 并在同头声明）。

- [x] **Step 2: 验证失败**：BUILD FAIL `ocr_session`。
- [x] **Step 3: 实现 session + worker**

`ocrSessionPump` 每区域：captureIsCapturable→captureFn→缩略图变化判定→readFn→ocrRuleMatches→ocrRuleStep；trigger 追加事件并写 timeline（rule_trigger + loss_change 两条）；unavailable 每区域 5 秒最多一条事件。worker 线程 Sleep 节拍，忙则 continue；队列用 CRITICAL_SECTION + 堆数组，stop 事件置位后 join、释放。

- [x] **Step 4: 验证通过**：`PASS: ocr_session`，全量绿；线程安全问题由代码评审把关。
- [x] **Step 5: Commit**

```bash
git add src/ocr_session.* src/ocr_worker.* tests/test_ocr_session.c scripts/run_tests.sh src/ocr_rules.h
git commit -m "feat: OCR session orchestration and single-worker poll loop"
```

### Task 7: OCR 配置读写（纯逻辑）

**Files:**
- Create: `src/ocr_config.h`, `src/ocr_config.c`
- Test: `tests/test_ocr_config.c`
- Modify: `scripts/run_tests.sh`（SRC=`src/ocr_config.c src/ocr_rules.c`）

**Interfaces:**
- Produces:
```c
typedef struct {
    char  exeName[128];
    char  windowTitle[256];
    int   intervalMs;
    BOOL  changeDetect;
    BOOL  enabled;
    size_t ruleCount;
    OcrRule rules[16];
} OcrConfig;
BOOL ocrConfigLoad(const char *path, OcrConfig *out, char err[MSG_BUFSIZE]);
BOOL ocrConfigSave(const char *path, const OcrConfig *cfg, char err[MSG_BUFSIZE]);
BOOL ocrConfigValidate(const OcrConfig *cfg, char err[MSG_BUFSIZE]); // 空关键词/越界档/帧<1/enabled 但 exe 空
```

- [x] **Step 1: 写失败测试**：save→load 往返两条规则（含中文关键词、norm 四位小数、DISAPPEAR、95）；损坏行跳过不致命；validate 拒绝空 exe+enabled、lossPercent 101、confirmFrames 0、空关键词。
- [x] **Step 2: 验证失败**：BUILD FAIL `ocr_config`。
- [x] **Step 3: 实现**：key=value 文本（与 config.txt 同目录 `ocr_config.txt`），解析容忍未知键。
- [x] **Step 4: 全量绿。**
- [x] **Step 5: Commit** `feat: OCR automation config persistence and validation`

### Task 8: 自动测试 UI 块（折叠区、规则行、参数、日志）

**Files:**
- Create: `src/ui_ocr.h`, `src/ui_ocr.c`
- Modify: `src/ui_simple.h/.c`（挂入第⑤块的接缝与外部切档接缝，见下）, `src/i18n.c`（ocr.* key 成对）, `scripts/run_tests.sh`（test_ui_simple 的 SRC 增 `src/ui_ocr.c src/ocr_config.c src/ocr_rules.c src/ocr_capture.c`）

**Interfaces:**
- Consumes: Task 2/7 类型；main.c 将提供 loss applier
- Produces:
```c
Ihandle *uiOcrCreate(Icallback actionCb);   // 折叠 Frame + 勾选 + 展开体；块内开始/停止走 actionCb
void uiOcrRetranslate(void);
void uiOcrSetAppContext(const char *exe);   // NULL=不限制应用：勾选被拒
typedef void (*OcrLossApplier)(int percent, const char *source);
void uiOcrSetLossApplier(OcrLossApplier fn);
BOOL uiOcrEnabled(void);                    // 勾选状态
BOOL uiOcrGetConfig(OcrConfig *out);        // 从控件收集
void uiOcrPopulate(const OcrConfig *cfg);   // 启动时从 ocr_config.txt 回填规则行/参数/窗口标题
void uiOcrSetPickedWindow(const CaptureWindow *win); // Task 9 本会话手选覆盖
const char *uiOcrSavedWindowTitle(void);    // 无手选时 Start 按此重解析
BOOL uiOcrHasPickedWindow(void);
HWND uiOcrPickedHwnd(void);
void uiOcrSetRunning(BOOL running);         // 运行时禁用编辑
void uiOcrAppendLog(const char *line);      // 滚动日志追加并沉底
void uiOcrSetCsvPath(const char *path);
void uiOcrShowError(const char *keyOrText); // 块内红字（如窗口重解析失败）
```
整个文件以 `#if defined(CLUMSY_OCR)` 包裹；禁用分支提供同名 no-op 桩（`uiOcrCreate` 返回 NULL），main.c 调用点用 `#if defined(CLUMSY_OCR)` 保护。
ui_simple 接缝：
```c
void uiSimpleAppendSection(Ihandle *child); // 在现有 4 块之后、按钮之前挂第⑤块
void uiSimpleApplyExternalLoss(int percent);// 设 g_percent+滑块 VALUE+标签+simpleSetLoss
```

- [ ] **Step 1: 写失败测试**（扩展 tests/test_ui_simple.c section 0）

`uiOcrCreate(NULL)` 后默认折叠/未启用；启用后能收集配置，非法行（空关键词、档位 120、帧数 0）让 `uiOcrGetConfig` 返回 FALSE；`uiOcrPopulate` 两条假规则后行数=2 且收集回的关键词/档位一致；`uiSimpleApplyExternalLoss(7)` 后 `uiSimpleLoss()==700` 且滑块 VALUE=7。
Expected before impl: 编译失败（符号不存在）→ RED。

- [ ] **Step 2: 实现 UI 主体**

IupFrame 折叠条（FlatToggle「启用自动测试（OCR）」），展开体：窗口只读行+「选择窗口…」按钮（Task 9 接）、「+ 添加区域（拖框）」按钮（Task 9 接，先占位禁用态可后续放开）、规则行容器（每行：名称/关键词/下拉/帧数/档位/删除，行数据存控件原生指针）、间隔 IupText + 变化检测 IupToggle、开始/停止（同一 toggle 回调，main 传入——uiOcrCreate 带 actionCb 参数，签名加一个 Icallback）、IupMultiLine 只读 6–8 行高（APPENDVALUE 后 SETPOS 末尾）、路径 FlatLabel 预留宽 + 「打开目录」FlatButton（explorer /select,）。i18n keys：`ocr.title/enable/window/choose/addRegion/name/keywords/edge.appear/edge.disappear/frames/loss/interval/changeDetect/start/stop/csvPath/openDir/enabled-blocked-app/error.windowStale/error.invalidRow/...`（zh/EN 成对，test_i18n 完整性把关）。文件尾部在同文件内写 `#else` 桩分支。

- [ ] **Step 3: 实现 ui_simple 两接缝**

Append 在流量图 Frame 之后、绿色按钮 vbox 之前；ExternalLoss 同时更新 g_percent、滑块控件 VALUE、百分比 label 与 simpleSetLoss（复用现有 label 更新静态函数）。

- [ ] **Step 4: 验证**：`scripts/run_tests.sh` 全绿（含 test_i18n 新键完整性）。
- [ ] **Step 5: 手动视觉（manual_ui  harness 加 ocr 占位模式）**

在 tests/manual_ui.c 挂入 `uiOcrCreate(NULL)` + `uiOcrPopulate`（静态假配置两条规则），截 zh/en 折叠态+展开态，确认无裁切、主窗口宽度不被规则行撑宽（内部滚动或换行由截图定）。
- [ ] **Step 6: Commit**

```bash
git add src/ui_ocr.* src/ui_simple.* src/i18n.c tests/test_ui_simple.c tests/manual_ui.c scripts/run_tests.sh
git commit -m "feat: collapsible OCR automation section with rule rows and live log"
```

### Task 9: 窗口选择与拖框（IUP 遮罩）

**Files:**
- Create: `src/ui_picker.h`, `src/ui_picker.c`
- Modify: `src/ui_ocr.c`（接按钮回调、选中窗口回填、拖完新增规则行并写入 norm）
- Manual: `tests/manual_ui.c`（picker 模式）

**Interfaces:**
- Consumes: `captureEnumWindowsForApp`, `captureMapNorm`
- Produces:
```c
BOOL uiPickerPickWindow(const char *exeName, CaptureWindow *out); // 模态 IupList 对话框
// 半透明 TOPMOST 覆盖目标窗口所在显示器，拖一个矩形，ESC/右键取消
BOOL uiPickerPickRect(HWND target, NormRect *out);
```

- [ ] **Step 1: 实现窗口选择对话框**（IupList 弹窗，标题 + 尺寸，确定/取消；无可见窗口时内联提示）。
- [ ] **Step 2: 实现拖框覆盖层**（无边框 IupDialog+canvas，定位到目标窗口所在显示器工作区；GDI 画半透明遮罩与橡皮筋；落点用 ScreenToClient 换算客户区比例；目标窗口本身在覆盖层下方仍可见）。
- [ ] **Step 3: 接回 ui_ocr**：窗口行显示标题；拖完以默认名「区域N」新增一行（edge=DISAPPEAR、frames=2、loss=95 默认仅为占位，由用户改）、norm 存进行控件用户数据；添加区域时 exe/window 未选 → 内联提示。
- [ ] **Step 4: 手动截图验收**：manual_ui 加 `pickwindow`/`pickrect` 模式，对记事本与一个 DirectX 示例窗口各拖一次，回主界面行显示正确。
- [ ] **Step 5: 全量绿 + Commit** `feat: target window picker and in-window region rubber-band`

### Task 10: 开始/停止接线与配置落盘

**Files:**
- Modify: `src/main.c`（uiStartCb/uiStopCb/languageCb）, `src/ui_ocr.c`
- Manual: `tests/manual_ui.c`

**Interfaces:**
- Consumes: Task 3/5/6/7/8/9 全部
- 行为（spec §4 步骤 7–9、§5 来源标记；持久化与重解析为用户 2026-10-25 决定）：
  - Start（OCR 启用时顺序）：收集+`ocrConfigValidate` → **解析目标 HWND**：本会话手选过（`uiOcrHasPickedWindow`）用 `uiOcrPickedHwnd`（先 `captureIsCapturable` 复检）；否则 `captureEnumWindowsForApp(exe)` + `captureResolveWindow(savedTitle)`：matchCount≠1 → `uiOcrShowError(ocr.error.windowStale)`、红字返回失败、**不建时间线、不挂 WinDivert** → `ocrConfigSave`；`ocrTimelineOpen`（路径回填 UI）；`ocrEngineCreate(modelsDir)` 失败 → 红字、**不挂 WinDivert**；建 OcrSession（真实 capture/read/now 注入）、`ocrSessionSeed` 初扫武装、`ocrWorkerStart`；首事件 `session_start` + `loss_change(source="initial_slider", loss=滑块当前%)`；`uiOcrSetRunning(TRUE)`。
  - OCR 未启用：路径与现状逐行一致（validate/resolve/save 全部跳过）。
  - Stop：`ocrWorkerStop`、destroy engine、`session_stop` + timeline close；`uiOcrSetRunning(FALSE)`；滑块恢复可编辑（uiSimpleSetRunning 已做）。
  - Loss applier（UI 线程事件回调）：`uiSimpleApplyExternalLoss(pct)` + 追加双语滚动日志 + timeline 已在 worker 事件里落盘（事件在 pump 时写 CSV，UI 只做显示/切档）。
  - 事件队列由 ui_ocr 内 IupTimer（100ms）draining；timer 在 init 创建、块折叠也存活。
  - 启动装载：main init 在 `uiOcrCreate` 后调 `ocrConfigLoad(<exe目录>/ocr_config.txt)`，成功则 `uiOcrPopulate`；文件不存在按全默认处理，不报错、不弹窗。应用下拉变化时仅更新 app context，不清空已填规则。

- [ ] **Step 1: 接线 main.c**（OCR 关闭时路径零变化：保持现有 Start/Stop 逐行可对照；重解析 0/歧义分支的纯逻辑已在 Task 4 单测钉死，端到端由 Task 12 关闭/多开窗口手动验收）。
- [ ] **Step 2: 错误路径单测级验证**：引擎创建失败用链接 seam（`ocrEngineCreate` 在 ui_ocr 通过弱引用函数指针 `g_engineFactory`，测试可置 fake 失败工厂）放到 test_ui_simple：start 前置失败时 applier 未被调用、返回失败。
- [ ] **Step 3: 手动联调（测试夹具）**

写 `tests/manual_ocr_fixture.c`（Win32 小窗：右下 Static「仓库」，按钮隐藏；中央按钮显示「重连」），编译运行；clumsy 选该夹具、两框两规则（消失→95、出现→50）、间隔 500ms、帧数 2，跑完整剧本。
Expected: 日志与 CSV 仅出现一次 0→95、一次 95→50；时间戳、source、ocr_text 完整；停止后 CSV 关闭、目录可打开。
- [ ] **Step 4: 全量绿 + 产品 debug 构建 + Commit** `feat: wire OCR automation into start/stop with config and timeline`

### Task 11: 打包、分发与构建同步

**Files:**
- Modify: `scripts/build-release.sh`（复制 models/ 与 RapidOCR 运行时 DLL/许可到 release 目录与 dist）, `build.zig`（按 Task 1 spike 结论：x64 登记全部新 .c 并链接 RapidOCR；x86 设 `CLUMSY_OCR=0` 且源清单排除 ocr_engine/ocr_session/ocr_worker/ocr_capture/ui_picker，仅保留 ui_ocr 桩）
- Create: dist 内许可说明更新（README-QuickStart 与 快速上手.txt 增补 OCR 块说明、PrintWindow-only 限制与模型许可）

- [ ] **Step 1: release 构建**：`SIGN=A scripts/build-release.sh` 成功，exe 子系统仍 Windows GUI，输出目录含模型文件与许可。
- [ ] **Step 2: 整包验证**：解压到新目录双击运行，模型缺失提示路径正确；记录包体积增量到 ledger。
- [ ] **Step 3: build.zig 双架构核对**：spike 结论为 zig cc 可链接则 `zig build` x64 通过且 OCR 块可见；不可行则 x64 也定义 `CLUMSY_OCR=0`（Ruling 已在 Task 1 记录）；x86 构建必须通过且无 OCR 块；grep 确认新文件在对应源清单内。
- [ ] **Step 4: Commit** `chore: package OCR models and runtime in release build`

### Task 12: 端到端验收与收尾

**Files:**
- Manual evidence: `docs/superpowers/specs/screenshots/{zh,en}/ocr-*.png`（≥4：折叠、展开配置、运行中日志 zh/en）

- [ ] **Step 1: 完整剧本回归**：Task 10 夹具跑 3 轮，CSV 事件每次恰好两条 loss_change（初始 + 触发）×规则数，无误触发；手动停后档位恢复可编辑。
- [ ] **Step 2: Review Focus 验收**：删模型启动（内联红字、不挂接）；150% DPI 拖框与运行中拖移窗口；最小化窗口记 unavailable；间隔设 200ms 跑 3 分钟 CPU/线程不堆积（任务管理器）；「不限制应用」下启用被拒；非法行被拒；**持久化重解析**：保存配置后重启，标题唯一时自动解析成功并在窗口行显示标题；关闭目标窗口（0 匹配）与开两个同名窗口（歧义）时点开始被拒、内联红字、不挂 WinDivert、不建 CSV；重开窗口后再次开始成功。
- [ ] **Step 3: 无 OCR 回归**：不勾选跑既有 §7 手测要点（开始/停止、95%/100%、定时恢复、tab 往返、语言切换），全量测试 16 TU 绿；x86 `zig build`（CLUMSY_OCR=0）通过且界面无 OCR 块。
- [ ] **Step 4: 稳定性**：夹具下 500ms 周期连续 10 分钟 Start/Stop 循环 5 次，句柄计数 delta ≤2（manual_leak 方式扩展或任务管理器基线对比，记录方法到 ledger）。
- [ ] **Step 5: 截图与 ledger 结论**；清理 spike/manual 临时文件（manual_ui/manual_leak/manual_ocr_fixture 与 build/spike-ocr，按既有约定验收后删）。
- [ ] **Step 6: 整分支最终评审**（superpowers finishing-a-development-branch）。

---

## Self-Review 结论

- **Spec 覆盖 + 用户 2026-10-25 四项决定**：§4 流程 → T8/9/10；§5 规则引擎 → T2/6（未武装不触发已入测试）；§6 抓帧 → T1/T4（v1 仅 PrintWindow，全黑走 unavailable，WGC 快跟进）；§7 OCR → T1/T5；§8 界面 → T8/9；§9 时间线 → T3/T10；§10 spike → T1；§12 验收 → T12；持久化+按标题重解析 → T7/T4/T8/T10/T12；x64-only 构建矩阵 → 全局约束 + T1 zig 探测 + T11。
- **类型一致**：OcrRule 在 T6 增 `NormRect norm` 字段（已在 T6 接口注明）；OcrSessionEvent 同时驱动 CSV（worker 内）与 UI applier（主线程），两路径不重复写 CSV；`uiOcrCreate(actionCb)` 签名 T8 定义、T8 手动 harness 与 T10 接线一致。
- **比例**：无产品代码成段转录；UI/抓帧等强视觉部分以行为+手动验收表达，纯逻辑全部 TDD。
