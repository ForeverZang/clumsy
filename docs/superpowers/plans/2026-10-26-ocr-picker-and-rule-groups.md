# 微信式选窗 / 合并开始按钮 / 区域表 + 规则组（AND）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 ⑤ 自动测试块从「文字列表选窗 + 块内开始按钮 + 单区域规则行」重构为「微信式框选遮罩 + 唯一绿色开始按钮 + 固定高度区域/规则组两张表，组内多区域 AND 双保险」，并自动迁移旧配置。

**Architecture:** 纯逻辑层（ocr_rules）新增组类型、关键词匹配与组级合成 poll，复用现有 `OcrRuleState` 边沿状态机；配置层（ocr_config）换成 region/group 两层模型并兼容旧 `rule.N.*` 文件；会话层（ocr_session）按物理区域去重抓取、按组合成观测、seed 宽口径放行；UI 层新增 ui_ocr_groups 两张 IupMatrix + 模态组编辑对话框，ui_picker 重构为 FRAME/DRAG 两阶段遮罩（纯状态机先行，IUP 外壳做薄）。

**Tech Stack:** gnu11 C / Win32 / IUP 3.30 mingw6 / WinDivert 2.2.0 / RapidOCRLite；原生 assert 单测（无框架），`bash scripts/run_tests.sh` 16+1 个测试程序。

**Spec:** docs/superpowers/specs/2026-10-26-ocr-picker-and-rule-groups-design.md

## Global Constraints

- 安全铁律：无法归属到目标应用的包一律原样放行；OCR 全程只经 `uiSimpleApplyExternalLoss → simpleSetLoss`（双重 clamp），不碰过滤/shouldAffect/包路径。
- 仅 x64 mingw gcc；所有 gcc 命令经 `/c/msys64/usr/bin/bash -lc 'cd /d/Workspaces/claudeprojects/clumsy && <cmd>'` 运行；全量验证命令 `bash scripts/run_tests.sh`（结尾必须打印 `ran 17 test programs` 且无 FAIL，Task 5 起；Task 1-4 为 16）。
- 文件 ≤800 行、函数 ≤50 行；每个 malloc 检查返回值；C 风格沿用现有代码（有界 strncpy + 显式终止符，不用 C++）。
- 间隔默认 500ms、下限 200ms、上限 60000；区域上限 16、组上限 16；PrintWindow v1。
- UI 文案中英配对走 `tr()`；CSV 与 LOG 一律英文；i18n 占位符 zh/en 数量必须一致。
- 提交用 conventional commits（feat/fix/refactor/docs/test/chore）；**不加任何 attribution/Co-Authored-By 尾注**。
- 每个任务结束时全量测试为绿；提交只在绿树上进行。
- 不碰与本需求无关的已暂存改动：`src/elevate.c`、`i18n.c` 中 3 个 `error.elevate*` 键继续挂起；`.vscode/`、2026-10-19 未跟踪文件不动。
- 不 merge、不 push；最终只留本地提交。

## Review Focus

1. **HWND 选窗期间死亡/回收（最危险，反目标）**：FRAME 枚举后到点击锁定之间，HWND 可能被销毁并被另一进程复用。锁定前必须重新校验 `IsWindow + GetWindowThreadProcessId==枚举时 PID + 可见`，否则会把别的应用的窗口框进来。Task 6 用真实 Static 窗口加销毁窗口的单测钉死。
2. **手改配置的坏索引**：`group.j.member.k.region` 越界、重复、指向不存在区域，或组内 0 成员 —— 启动校验必须拒绝且错误点名组号。Task 2 单测。
3. **多成员触发文本的 CSV 转义**：拼接文本含逗号/引号/换行进 CSV 必须走 `ocrCsvEqCsvEscape`（RFC4180），列结构不得错位。Task 2 session 单测。
4. **删区域引发的组成员重索引**：删除中间区域后，后续区域下标平移，引用它们的组成员必须同步重映射；成员清空的组标红且阻止开始。Task 3 单测。
5. **RESIZE=NO 对话框的表高/重排**：两张表固定可视行高、表内滚动；拖入区域/展开折叠后对话框高度不得跳动，需要 native SetWindowPos refit（既有 g_layoutChanged 通道）。Task 7 物理清单肉眼验收。

---

### Task 1: 纯组逻辑 —— 类型、关键词匹配、组 poll 合成、边沿步进拆分

**Files:**
- Modify: `src/ocr_rules.h`, `src/ocr_rules.c`
- Test: `tests/test_ocr_rules.c`

**Interfaces:**
- Produces（后续任务全部依赖）:
  - 常量 `OCR_MAX_REGIONS 16`、`OCR_MAX_GROUPS 16`（定义在 ocr_rules.h；ocr_session.h 改为包含使用，删除其本地 `OCR_MAX_REGIONS` 定义）。
  - `typedef struct { int regionIndex; char keywords[256]; } OcrGroupMember;`
  - `typedef struct { OcrEdge edge; int confirmFrames; int lossPercent; size_t memberCount; OcrGroupMember members[OCR_MAX_REGIONS]; } OcrGroup;`（members 恒按 regionIndex 升序、去重）。
  - `typedef struct { BOOL available; const char *text; } OcrRegionObs;`（一个物理区域一个 tick 的观测；text 仅在 available 时有效）。
  - `BOOL ocrKeywordsMatch(const char *keywords, const char *rawText);` —— 从现有 `ocrRuleMatches` 抽出的纯匹配（归一化子串 OR）；`ocrRuleMatches` 变为转调，行为不变。
  - `BOOL ocrEdgeStep(OcrRuleState *st, OcrEdge edge, int confirmFrames, int lossPercent, OcrPoll poll, int *targetPercent);` —— 从现有 `ocrRuleStep` 抽出的主体；`ocrRuleStep` 变为转调，行为不变。
  - `OcrPoll ocrGroupPoll(const OcrGroup *group, const OcrRegionObs *obs, size_t obsCount);`

- [ ] **Step 1: 在 tests/test_ocr_rules.c 写失败测试**

新增测试函数，断言：
- `ocrKeywordsMatch("重 连, 仓库", "Online 仓 库")` 为 TRUE；空/纯分隔符关键词为 FALSE（与旧 ocrRuleMatches 用例同语义，转调后旧用例全保留）。
- 2 成员组（region 0 关键词「就绪」、region 1 关键词「仓库」）真值表，逐行断言：

  | obs[0] | obs[1] | poll |
  |---|---|---|
  | available+命中 | available+命中 | MATCHED |
  | available+命中 | available+未命中 | MISSED |
  | available+未命中 | available+命中 | MISSED |
  | unavailable（text=NULL） | available+命中 | UNAVAILABLE |
  | available+命中 | unavailable | UNAVAILABLE |

  越界 member.regionIndex（如 5 但 obsCount=2）按 UNAVAILABLE 处理。
- 组状态机走 `ocrEdgeStep`：DISAPPEAR 组，先 `ocrRuleArm(TRUE)`，MISSED×1 不触发、中间插 UNAVAILABLE 不计数（run 仍 1）、再 MISSED 触发且 `*targetPercent==95`；APPEAR 组同理 N=2。未 arm 时任何序列不触发（旧用例 12 的组版）。
- 旧 21 组用例一条不删。

- [ ] **Step 2: 运行测试确认失败**

Run: `bash scripts/run_tests.sh`
Expected: ocr_rules 编译失败（`OcrGroup` 未定义），其余程序现状通过。

- [ ] **Step 3: 实现**

在 ocr_rules.h 加上述类型/常量/原型；ocr_rules.c 中：抽 `ocrKeywordsMatch`（ocrRuleMatches 转调）；抽 `ocrEdgeStep`（ocrRuleStep 用 rule 三字段转调）；`ocrGroupPoll` 严格按真值表：先遍历成员，任一区域 `!available || regionIndex>=obsCount` → UNAVAILABLE；全部可用后对每个成员调 `ocrKeywordsMatch(member.keywords, obs[regionIndex].text)`，全部命中 MATCHED 否则 MISSED。

- [ ] **Step 4: 全量测试通过**

Run: `bash scripts/run_tests.sh`
Expected: `ran 16 test programs`，全 PASS。

- [ ] **Step 5: 提交**

```bash
git add src/ocr_rules.h src/ocr_rules.c tests/test_ocr_rules.c
git commit -m "feat: add pure AND-group matching and group poll synthesis"
```

---

### Task 2: 引擎端到端换模型 —— region/group 配置、迁移、会话组状态机（大头任务，含多个绿提交）

本任务是「规则组（AND）」的原子切片：配置结构一换，config/session/app/glue/ui 边界必须同时编译通过。UI 内部用一个明确的「行 ↔ 单成员组」边界适配保留旧规则行界面（多成员组在 Task 3 才有编辑器；现状不可能产生多成员组，迁移产物也全是单成员）。

**Files:**
- Modify: `src/ocr_config.h`, `src/ocr_config.c`, `src/ocr_session.h`, `src/ocr_session.c`, `src/ocr_app.c`, `src/ocr_glue.c`
- Modify（边界适配，行为不变）: `src/ui_ocr.c`, `src/ui_ocr.h`
- Modify（非套件夹具，必须随结构改完可编译）: `tests/manual_ocr_acceptance.c`, `tests/manual_ocr_e2e.c`
- Test: `tests/test_ocr_config.c`（重写扩充）, `tests/test_ocr_session.c`（重写为组模型，场景一一对应保留）, `tests/test_ui_simple.c`（0b/0c 段断言适配新结构，旧行 UI 仍在）

**Interfaces:**
- Consumes: Task 1 全部产物。
- Produces:
  - ocr_config.h：
    ```c
    #define OCR_CONFIG_GROUPS_MAX OCR_MAX_GROUPS
    typedef struct { char name[32]; NormRect norm; } OcrRegionDef;
    typedef struct {
        char exeName[128]; char windowTitle[256];
        int intervalMs; BOOL changeDetect; BOOL enabled;
        size_t regionCount; OcrRegionDef regions[OCR_MAX_REGIONS];
        size_t groupCount;  OcrGroup groups[OCR_CONFIG_GROUPS_MAX];
    } OcrConfig;
    BOOL ocrConfigValidate(const OcrConfig *cfg, char err[MSG_BUFSIZE]);
    BOOL ocrGroupValidate(const OcrGroup *g, size_t groupNumber1Based,
                          size_t regionCount, char err[MSG_BUFSIZE]);
    ```
    `ocrConfigValidateDrafts` 删除（新模型里裸区域天然惰性，无 draft 概念）。
  - ocr_session.h：
    ```c
    typedef struct {
        NormRect norm;
        unsigned char thumb[OCR_THUMB_W * OCR_THUMB_MAX_H];
        int thumbW, thumbH; BOOL hasThumb;
        char lastText[OCR_EVENT_TEXT]; BOOL hasLastText;
    } OcrSessionRegion;
    typedef struct { OcrGroup group; OcrRuleState state; UINT64 lastUnavailableMs; } OcrSessionGroup;
    /* OcrSession 字段：regionCount/OcrSessionRegion *regions、groupCount/OcrSessionGroup *groups，
       BOOL referenced[OCR_MAX_REGIONS]；evText 维度改 [OCR_MAX_GROUPS][OCR_EVENT_TEXT]。
       OcrSessionEvent 字段 regionIndex 改名 groupIndex。*/
    ```
  - 时间线身份：rule_trigger/loss_change/capture_unavailable 的 region 列为 `group <N>`（N 从 1），source 列为 `group:<N>`；edge/loss 取组值。
  - 会话语义（spec §5.1）：仅抓取 referenced[] 并集；每组每 tick 合成 poll；成员全可用首次才 arm（seed 或 pump）；同 tick 多组触发：rule_trigger 行全写，**只有表序第一个触发组**产生 loss_change 事件与 CSV 行并带档位；seed 只要被引用区域中 ≥1 个可读即成功，全部失败才 `ocr.error.seed`；目标死亡 tick 每组各出限流的 capture_unavailable。
  - 触发文本：成员 OCR 文本按 member 顺序以 ` | ` 拼接（截断到 255），经 `ocrCsvEscape` 写 CSV。
  - ui_ocr 边界适配（本任务临时层，Task 3 删除）：`collectRow` 产出 `regions[i]={name,norm}` + `groups[i]={edge,frames,loss,1×member{i,keywords}}`；populate 仅接受单成员组并按其唯一成员回填行（现状输入全部满足）。`uiOcrAddRegionForTest/RowBoxForTest` 语义不变。
  - glue：`cfg.groupCount==0` → `uiOcrShowError("ocr.error.noGroups")`；`runRegionName` 改 `runGroupName`，返回静态串 `group N`（1-based）。

- [ ] **Step 1: 重写 test_ocr_config.c（RED）**

保留 missing-file 用例；roundtrip/corrupt/validate 全部改新结构并新增：
- 新格式 roundtrip：2 区域 2 组（组 1 双成员引用 0、1；组 2 单成员），保存文本含 `region.0.name/x/y/w/h`、`group.0.edge/frames/loss`、`group.0.member.0.region/keywords`、`group.0.member.1.*`；reload 后逐字段断言（含中文关键词、4 位小数 norm）。
- **迁移**：写旧格式文件（`ruleCount=2` + 两套 `ruleN.*`，其中 rule1 无 name）→ load 后 regionCount==2、groupCount==2、每组 memberCount==1、member.regionIndex 对应、keywords/edge/confirm→confirmFrames/loss/norm 一致；无名规则区域名兜底为 `Region 2`；随后 save 再 load，文件里不再出现 `rule` 键且数据不变。
- 容忍解析：未知键跳过、`group.9.*` 越界跳过、`group.0.member.0.region=notnum` 跳过该成员。
- 校验：组 0 成员 → 拒绝且 err 含 `group 1`；成员 regionIndex 越界/重复 → 拒绝（Review Focus #2）；成员关键词空/纯分隔符 → 拒绝；frames<1、loss 越界 → 拒绝；被引用区域 norm 退化（w<0.01 / >1）→ 拒绝；未被任何组引用的区域 norm 退化 → 允许（不抓取）；enabled+空 exe → 拒绝，disabled 允许；groupCount==0 本身允许（UI 层另有门槛）。

- [ ] **Step 2: 运行确认失败**

Run: `bash scripts/run_tests.sh`
Expected: ocr_config 编译失败（OcrConfig 无 regions/groups 字段）。

- [ ] **Step 3: 实现 ocr_config 新模型 + 旧格式迁移（GREEN）**

解析器内保留一套临时旧规则累加器（仅解析期栈上数组），见到任何 `rule.N.*` 即标记 legacy；load 结尾：legacy 模式下把旧规则逐条转为 region+单成员组（name 空兜底 `Region %u`，i+1）；新键 `region.i.*`/`group.j.*`/`group.j.member.k.*` 直接落结构；count 从最大下标派生（ruleCount/regionCount/groupCount 行只做上限钳制）。writer 只写新格式。`ocrGroupValidate` 为单一组校验实现，`ocrConfigValidate` 遍历区域 norm + 各组调用它。

Run: `bash scripts/run_tests.sh`
Expected: ocr_config PASS；其他程序编译失败（预期，继续后续步骤）。

- [ ] **Step 4: 重写 test_ocr_session.c 为组模型（RED）**

测试夹具：`g_reg[]` 物理区域假不变；helper 改 `addGroup(s, groupIndex, OcrEdge, loss, frames, memberIndices[], keywords[], physicalX[])`,物理区域 norm 用现有 x/wfrac 机制保证假 capture 收到不同 RECT。旧 (a)-(k) 场景全部保留、改单成员组表达；断言事件字段用 `ev[i].groupIndex`。新增：
- **AND 真值表**：组 {0「就绪」,1「仓库」}，seed 双中 armed PRESENT；随后 tick（中,不中）→ 不触发；（不可用,中）→ capture_unavailable 限流且不触发；连续 2 tick 任一不中 → 触发 95。
- **并集抓取**：3 个区域、只有区域 2 被引用 → 一轮 seed+pump 后 `g_reg[0].captures==0 && g_reg[1].captures==0 && g_reg[2].captures>0`。
- **宽口径 seed（R2）**：2 成员组，seed 时区域 0 可读、区域 1 capture 失败 → seed 返回 TRUE、组未 armed；之后两区域首个同时可读 tick：都中→armed PRESENT，不触发。
- **同 tick 两组**：组 0（95）与组 1（50）同一 pump 触发 → rule_trigger==2、loss_change==1，且唯一 loss_change 档位为 95、groupIndex==0；CSV 两行 rule_trigger 都在、loss_change 只一行。
- **CSV 身份与转义（Review Focus #3）**：单成员触发文本造一个含英文逗号和引号的串（假 read 返回 `a,b"c`），断言 CSV 中该字段被双引号包裹且内部引号双写；行前缀断言 `rule_trigger,group 1,` 与 source 列 `group:1`。
- 旧 unavailable 限流/blank-frame/identity-gate/change-detect/worker 两场景按组模型复述（identity-gate 死亡 tick 对每组各出 unavailable）。

- [ ] **Step 5: 实现 ocr_session 组管线（GREEN）**

`readPhysicalRegion(s, idx, text)` 沿用现有 readRegion 全部逻辑（blank 帧/缩略图/change gate）；pump：alive 闸门 → 只对 referenced 区域读取产出 `OcrRegionObs[regionCount]`（UNCHANGED 无 lastText 按 unavailable；有 lastText 用缓存文本 available）；逐组成 poll、未 armed 且全可用时 `ocrRuleArm(poll==MATCHED)`、`ocrEdgeStep`；触发收集时记录本 pump 首触发组，只有它追加 loss_change + 时间线 loss_change；rule_trigger 与 capture_unavailable 每组都写。seed 同样只扫 referenced，全失败才 FALSE。writeTimeline 改签名接 `(groupIndex, const OcrGroup*, text, loss)`。

- [ ] **Step 6: 改 ocr_app / ocr_glue / ui_ocr 边界适配，让全树编译并绿**

- ocr_app：静态数组改 `OcrSessionRegion regions[OCR_MAX_REGIONS]; OcrSessionGroup groups[OCR_MAX_GROUPS];`；buildSession 按 cfg 填 norm、拷贝组、计算 referenced[]。
- ocr_glue：runCfg 字段改名访问；groupCount==0 门槛 + `ocr.error.noGroups`；`runGroupName` 返回 `group N`。
- ui_ocr.c/.h：collect/populate 按「行 ↔ 单成员组」适配（接口签名 uiOcrGetConfig/Populate 不变）；删除 `ocrConfigValidateDrafts` 调用，改调 `ocrConfigValidate`；`drafts[]` 相关逻辑删除（裸区域现在合法，空关键词只可能存在于组成员上，Start 严格校验自然拒绝）。
- test_ui_simple 0b/0c：cfg 构造改新结构（单成员组）；断言改 got.groupCount/regions/groups；headless app 段同步。
- manual_ocr_acceptance.c / manual_ocr_e2e.c：cfg 构造改新结构（2 区域 + 2 单成员组）。
- i18n：本步骤新增键 `ocr.error.noGroups`（zh「请先新建至少一个规则组」/ en「create at least one rule group」）。

Run: `bash scripts/run_tests.sh`
Expected: `ran 16 test programs` 全 PASS。

- [ ] **Step 7: 手工编译两个真机夹具确认可编译**

Run: `/c/msys64/usr/bin/bash -lc 'cd /d/Workspaces/claudeprojects/clumsy && gcc -std=gnu11 -DCLUMSY_OCR=1 -Iexternal/WinDivert-2.2.0-A/include -Iexternal/iup-3.30_Win64_mingw6_lib/include -Iexternal/RapidOCRLite/include -Isrc -c tests/manual_ocr_acceptance.c -o /dev/null -fsyntax-only && gcc ... tests/manual_ocr_e2e.c -fsyntax-only'`（缺头时按报错补 -I，不链接）
Expected: 无 error。

- [ ] **Step 8: 分提交**

```bash
git add src/ocr_config.h src/ocr_config.c tests/test_ocr_config.c
git commit -m "feat: persist regions and AND-groups with legacy rule migration"
git add src/ocr_session.h src/ocr_session.c src/ocr_app.c tests/test_ocr_session.c
git commit -m "feat: capture referenced regions once and step per-group AND state"
git add src/ocr_glue.c src/ui_ocr.c src/ui_ocr.h src/i18n.c tests/test_ui_simple.c tests/manual_ocr_acceptance.c tests/manual_ocr_e2e.c
git commit -m "refactor: adapt OCR glue and rule rows to the region/group model"
```

---

### Task 3: 区域表 + 规则组表 + 组编辑对话框（G3/G4/D3/D4）

**Files:**
- Create: `src/ui_ocr_groups.h`, `src/ui_ocr_groups.c`
- Modify: `src/ui_ocr.c`（删除 Task 2 的行适配层与 OcrRow/g_ruleBox/addRuleRow/collectRow 等，体内嵌入 groups 模块；get/populate 直接走 groups 模型；addRegionCb 经 `uiOcrGroupsAppendRegions`）, `src/ui_ocr.h`（seam 调整）, `src/i18n.c`, `scripts/run_tests.sh`（ui_simple 的 SRC 加 `src/ui_ocr_groups.c`）
- Test: `tests/test_ui_simple.c`（0b 段重写为表/模型断言）

**Interfaces:**
- Consumes: Task 2 的 OcrConfig/OcrRegionDef/OcrGroup/ocrGroupValidate。
- Produces:
  ```c
  Ihandle *uiOcrGroupsCreate(void);              // 返回 vbox：区域 Frame(+Matrix+添加按钮) + 组 Frame(+Matrix+新建按钮)
  void    uiOcrGroupsRetranslate(void);
  void    uiOcrGroupsSetRunning(BOOL running);   // 冻结两表两按钮
  void    uiOcrGroupsGetModel(OcrRegionDef *r, size_t *rc, OcrGroup *g, size_t *gc);
  BOOL    uiOcrGroupsSetModel(const OcrRegionDef*, size_t, const OcrGroup*, size_t);
  BOOL    uiOcrGroupsAppendRegions(const NormRect *rects, size_t count); // 默认名 Region N（序号接现有最大值+1）
  const NormRect *uiOcrGroupsRegionRects(size_t *count);
  /* 纯摘要（语言随当前 tr）：R1「就绪」 且 R2「仓库」 / R1 "ready" and R2 "warehouse"；
     每个成员取关键词首个逗号前片段；总长超 48 字符尾部加 …。*/
  void uiOcrGroupsSummary(const OcrGroup *g, char *out, size_t cap);
  /* 测试 seams */
  Ihandle *uiOcrRegionsMatrixForTest(void);
  Ihandle *uiOcrGroupsMatrixForTest(void);
  size_t uiOcrGroupsRegionCountForTest(void);
  size_t uiOcrGroupsGroupCountForTest(void);
  BOOL   uiOcrGroupsCommitGroupForTest(const OcrGroup *draft, long editRow); // editRow=-1 新建；OK 按钮同路径
  void   uiOcrGroupsDeleteRegionForTest(size_t i);
  void   uiOcrGroupsRenameRegionForTest(size_t i, const char *name);
  void   uiOcrGroupsSetConfirmSuppressedForTest(BOOL suppress); // 删区域 IupAlarm 在测试中静默（默认按确认）
  ```
- 表规格：两张 IupMatrix；区域表可视 4 行、组表可视 3 行（NUMCOL_VISIBLE/NUMLIN_VISIBLE 或固定 RASTERSIZE 行高 24px + 表内滚动），READONLY_CB 仅名称列/编辑控件可改；列：区域表 `R# | 名称 | 位置 | 删除`（位置列文本 `0.12, 0.86 0.23×0.09` 两位小数比例，只读），组表 `组 | 条件摘要 | 触发 | 帧 | 丢包% | 删除`；删除列渲染 `删除`，CLICK_CB 点该列执行删除；空组摘要行 FGCOLOR 红、文本取 `tr("ocr.group.empty")`。
- 组编辑对话框（D4，IupPopup 模态）：上半每区域一行 `☑ R# 名称 | 关键词 IupText（未勾选时 ACTIVE=NO）`，表内滚动；下半 edge IupList(DROPDOWN)、frames、loss 三个 IupText；草稿是栈上 OcrGroup 副本，取消不提交；OK 调 `ocrGroupValidate` + 勾选≥1 + 每个勾选行关键词非空，失败对话框内红字不关闭；提交按勾选行序构造 members（天然升序去重）。双击组行 / 回车打开编辑既有组。
- 删区域：IupAlarm 一次确认（文案 ocr.delRegion.confirm）；删除后后续区域下标平移，所有组的 member.regionIndex 重映射、去重；变空的组保留为 0 成员空组（标红，validate 阻止开始）。
- 达 16/16 上限：对应添加按钮 ACTIVE=NO 且 TIP 提示。
- i18n 新增键（zh/en 各一，占位符对齐）：
  `ocr.region.title` 识别区域 / Regions；`ocr.col.rnum` 区域 / Region；`ocr.col.rname` 名称 / Name；`ocr.col.rpos` 位置（相对窗口） / Position (relative)；
  `ocr.group.title` 规则组（双击行编辑） / Rule groups (double-click to edit)；`ocr.col.gnum` 组 / Group；`ocr.col.gsummary` 条件 / Condition；`ocr.addGroup` + 新建规则组 / + New group；
  `ocr.group.empty` 空组：无区域 / Empty group: no regions；`ocr.group.dlgEdit` 编辑组 %u / Edit group %u；`ocr.group.dlgNew` 新建规则组 / New group；
  `ocr.group.members` 勾选区域并分别填写关键词（组内为「且」关系） / Select regions and set each one's keywords (all must match)；`ocr.group.kwHint` 关键词（逗号分隔，包含任一即命中） / Keywords (comma-separated, any match)；
  `ocr.group.ok` 确定 / OK；`ocr.group.cancel` 取消 / Cancel；`ocr.error.groupMembers` 第 %u 组为空：请至少勾选一个区域 / group %u needs at least one region；`ocr.error.groupKeywords` 第 %u 组有勾选区域未填关键词 / group %u has a selected region without keywords；
  `ocr.delRegion.confirm` 删除区域「%s」？\n引用它的规则组将同步移除该成员。 / Delete region "%s"?\nGroups using it lose that member.（IupAlarm 按钮用现有 ocr.picker.ok/cancel 即可）；
  `ocr.limitRegions` 区域数量已达上限（16） / Region limit (16) reached；`ocr.limitGroups` 规则组数量已达上限（16） / Group limit (16) reached；
  `ocr.groupName` 组 %u / Group %u。

- [ ] **Step 1: 在 test_ui_simple.c 写失败断言（RED）**

0b 段重写（保留 enable gate / folding / 窗口选择 / picker 几何等与表无关的全部断言）：
- create 后两张 matrix 存在，NUMLIN 初始 0；区域表可视行高固定（断言 RASTERSIZE 高度或 NUMLIN_VISIBLE==4/3，不随条目增长）。
- `uiOcrGroupsSetModel`：2 区域 + 2 组（组 0 双成员）→ counts/matrix NUMLIN 正确；组表摘要单元文本：zh 下 `strstr(summary, "R1") && strstr(summary,"且") && strstr(summary,"R2")`；en 下切语言后含 `and`。
- GetConfig 往返：model→get 出的 OcrConfig 与输入逐字段一致；含双成员组。
- appendRegions([r0,r1])：区域数 +2，名 `Region 1`/`Region 2`，norm 一致；再 SetModel 空后序号不要求复用。
- **重索引（Review Focus #4）**：3 区域、组引用 {0,2}；删除区域 1 后组成员变为 {0,1} 且关键词跟随原区域 2；删除区域 0 后组变 0 成员；GetConfig 出的 cfg 此时 `ocrConfigValidate` 失败且 err 含 `group 1`。
- rename seam 后 GetConfig 名称生效。
- CommitGroupForTest：有效草稿新建成功（16 组后第 17 次返回 FALSE 且按钮 ACTIVE=NO）；0 成员/空关键词/坏 frames/坏 loss 返回 FALSE 且错误 label TITLE 非空（同一 g_errorLabel 通道）。
- populate 16 区域后添加按钮 ACTIVE=NO，TIP 为 ocr.limitRegions 文本。
- running=TRUE：两 matrix 与两添加按钮 ACTIVE=NO；FALSE 恢复。
- 旧 addRegionForTest seam 保留（转调 AppendRegions 单个）；RowBoxForTest/StartButton 相关断言在本任务保持（按钮 Task 4 才删）。

- [ ] **Step 2: 运行确认失败**

Run: `bash scripts/run_tests.sh`
Expected: ui_simple 编译失败（ui_ocr_groups.h 不存在）。

- [ ] **Step 3: 实现 ui_ocr_groups 并接入 ui_ocr（GREEN）**

权威模型为文件内静态数组（regions/groups + counts），matrix VALUE_CB 从模型渲染；所有变更先改模型再 IupUpdate 矩阵。ui_ocr.c 删除 OcrRow 整套行机制与 Task 2 适配层，体内 window 行下方插入 `uiOcrGroupsCreate()` 返回根；get/populate 透传模型；addRegionCb 拿到单个 norm 后调 AppendRegions 并触发 g_layoutChanged 一次。run_tests.sh ui_simple SRC 行加 `src/ui_ocr_groups.c`。非 OCR  stub 段在 ui_ocr_groups.c 同样提供空 stub（头文件被 ui_ocr.h 包含时 CLUMSY_OCR 关闭路径要编译）。

Run: `bash scripts/run_tests.sh`
Expected: 16/16 PASS。

- [ ] **Step 4: 调试构建无新警告**

Run: `/c/msys64/usr/bin/bash -lc 'cd /d/Workspaces/claudeprojects/clumsy && SIGN=A scripts/build-debug.sh 2>&1 | tail -5'`
Expected: `done: build/mingw64-debug/clumsy.exe`，无新增 warning（既有 4 个 strncpy 除外）。

- [ ] **Step 5: 提交**

```bash
git add src/ui_ocr_groups.h src/ui_ocr_groups.c src/ui_ocr.c src/ui_ocr.h src/i18n.c scripts/run_tests.sh tests/test_ui_simple.c
git commit -m "feat: fixed-height region and group tables with group edit dialog"
```

---

### Task 4: 合并开始按钮 —— ⑤ 块内白按钮删除（G2/D2）

**Files:**
- Modify: `src/ui_ocr.c`, `src/ui_ocr.h`, `src/main.c`, `src/i18n.c`, `tests/test_ui_simple.c`, `tests/manual_ui.c`, `tests/manual_ocr_diag.c`

**Interfaces:**
- Produces: `Ihandle *uiOcrCreate(void);`（actionCb 参数删除）；删除 seam `uiOcrStartButtonForTest`；删除 i18n 键 `ocr.start`/`ocr.stop`；`uiOcrSetRunning` 只冻结/解冻配置区，不再操作任何按钮标题。绿色主按钮文案永远「开始影响/停止影响」（main.c 现状已是，不动文案逻辑）。

- [ ] **Step 1: 改测试（RED）**

test_ui_simple：删除 startBtn 变量、FLAT_ACTION==fakeActionCb 断言、ACTIVE/ocr.stop/ocr.start 标题断言；保留 running 冻结 toggle/body/按钮的断言（去掉 startBtn 两行）。0c headless 段不动。

- [ ] **Step 2: 确认失败**

Run: `bash scripts/run_tests.sh`
Expected: ui_simple 编译失败（uiOcrCreate 参数数量 / seam 不存在的报错出现在改完 Step 3 之前亦可，先看到因 ocr.start 断言失败的编译错误即可）。

- [ ] **Step 3: 实现**

ui_ocr.c：删 g_startBtn/syncStartActive/g_actionCb/startRow 及全部引用；enableCb 成功路径不再 syncStartActive；uiOcrCreate(void)；uiOcr.h 同步；main.c `uiOcrCreate((Icallback)uiToggleActionCb)` → `uiOcrCreate()`。i18n.c 删两行键。manual_ui.c:669 与 manual_ocr_diag.c:140 调用改无参；diag 删 start 控件相关 stage（它的真实使命是验证 enable 闸门，stage C 改为直接调 `uiOcrGlueStart(0)` 断言被拒/通过，不再取按钮 HWND）。

- [ ] **Step 4: 全绿 + 夹具语法编译**

Run: `bash scripts/run_tests.sh` → 16/16 PASS。
另对 tests/manual_ui.c、tests/manual_ocr_diag.c 跑 `-fsyntax-only`（参数同 Task 2 Step 7，加 `-Iexternal/RapidOCRLite/include`）。

- [ ] **Step 5: 提交**

```bash
git add src/ui_ocr.c src/ui_ocr.h src/main.c src/i18n.c tests/test_ui_simple.c tests/manual_ui.c tests/manual_ocr_diag.c
git commit -m "refactor: remove in-block start button, green bar is the only start"
```

---

### Task 5: 选窗遮罩纯状态机（G1 的可测内核）

**Files:**
- Create: `src/ui_picker_state.h`, `src/ui_picker_state.c`, `tests/test_picker_state.c`
- Modify: `scripts/run_tests.sh`（新测试程序 picker_state，`SRC="src/ui_picker_state.c"`，不需要 IUP/OCR 宏；加入 TESTS 列表）

**Interfaces:**
- Produces:
  ```c
  typedef enum { PICKER_PHASE_FRAME=0, PICKER_PHASE_DRAG=1 } PickerPhase;
  typedef struct {
      BOOL hadWindow; size_t winCount;
      long hover, locked;                 // -1 = none
      size_t keepCount;                  // 进入前已有区域数（始终保留）
      size_t rectCount;
      NormRect rects[OCR_MAX_REGIONS];   // [0,keepCount) 旧区域，其后为本会话新框
      PickerPhase phase;
  } PickerMachine;
  void uiPickerStateInit(PickerMachine *m, BOOL hadWindow, size_t winCount,
                         const NormRect *existing, size_t existingCount);
  void uiPickerStateSetHover(PickerMachine *m, long winIndex); // 越界 => -1
  BOOL uiPickerStateLock(PickerMachine *m, long winIndex);     // 仅 FRAME；成功后进 DRAG
  void uiPickerStateAutoLock(PickerMachine *m);                // 仅 winCount==1
  void uiPickerStateReframe(PickerMachine *m);                 // R1：回 FRAME，丢弃本会话新框
  BOOL uiPickerStateAddRect(PickerMachine *m, const NormRect *r); // 仅 DRAG 且 locked>=0；总数<=16
  BOOL uiPickerStateUndo(PickerMachine *m);                    // 弹一个本会话新框；无则 FALSE
  BOOL uiPickerStateCanFinish(const PickerMachine *m);         // locked>=0 且（hadWindow 或新框>=1）
  size_t uiPickerStateSessionCount(const PickerMachine *m);    // rectCount-keepCount
  ```

- [ ] **Step 1: 写 tests/test_picker_state.c（RED）**

逐行为钉死：
- init(hadWindow=FALSE, winCount=2)：phase FRAME、locked=-1、CanFinish FALSE。
- SetHover(1)/越界；Lock(1) → phase DRAG、locked=1；DRAG 后再 Lock 返回 FALSE。
- AutoLock：winCount=1 init 后 locked=0、phase DRAG；winCount=3 调 AutoLock 不改变状态。
- AddRect×2（keepCount=0）→ SessionCount=2、CanFinish TRUE；第 17 个（含 existing 凑满 16）返回 FALSE。
- hadWindow=TRUE、existing 2 个进 DRAG-direct：SessionCount=0 时 CanFinish 仍 TRUE（R3）；hadWindow=FALSE 同样 0 新框则 FALSE。
- Undo：弹到只剩旧区域；再 Undo FALSE；旧区域内容不被改变。
- Reframe（R1）：phase FRAME、locked=-1、rectCount 回到 keepCount、旧区域值不变；重新 Lock 后 AddRect 编号从 keepCount 起。
- 结尾打印 `OK: picker_state`。

- [ ] **Step 2: 运行确认失败**

Run: `bash scripts/run_tests.sh`
Expected: picker_state BUILD FAIL。

- [ ] **Step 3: 实现并全绿**

纯 C、无 IUP、无 Win32 依赖（除 NormRect 类型经 ocr_rules.h）。run_tests.sh 加条目（注意 FLAGS 默认含 IUP include 无妨，不链接 libiup 即可）。

Run: `bash scripts/run_tests.sh`
Expected: `ran 17 test programs` 全 PASS。

- [ ] **Step 4: 提交**

```bash
git add src/ui_picker_state.h src/ui_picker_state.c tests/test_picker_state.c scripts/run_tests.sh
git commit -m "feat: add pure FRAME/DRAG picker state machine"
```

---

### Task 6: 微信式 FRAME/DRAG 遮罩 + 入口接线（G1/D1/R1/R3）

**Files:**
- Modify: `src/ui_picker.h`, `src/ui_picker.c`, `src/ui_ocr.c`, `src/ui_ocr.h`, `src/i18n.c`, `tests/test_ui_simple.c`, `tests/manual_ui.c`
- Test: `tests/test_ui_simple.c`（choose-policy 真实窗口段扩展）

**Interfaces:**
- Deletes: `uiPickerPickWindow`、`uiPickerPickRect`、`uiPickerAcceptChooserForTest`、`uiPickerChooserHwndForTest`（文字列表整段删除）。
- Keeps: `uiPickerDragToNorm`、`uiPickerSetDragForTest`/`uiPickerCompleteDragForTest`（语义随新遮罩保留）、`uiPickerOverlayHwndForTest`。
- Produces:
  ```c
  typedef enum { UI_PICKER_OK=0, UI_PICKER_CANCEL=1, UI_PICKER_WINDOW_DIED=2 } UiPickerResult;
  UiPickerResult uiPickerPick(const char *exeName,
                              BOOL startFrame,       // TRUE=选择窗口入口；FALSE=+添加区域直达 DRAG
                              BOOL hadWindow,        // R3：FALSE 时完成门要求≥1 新框
                              const CaptureWindow *initialWindow, // !startFrame 时必非 NULL
                              const NormRect *existing, size_t existingCount,
                              CaptureWindow *outWindow,
                              NormRect *outRects, size_t *outRectCount);
  // 调用方先枚举；0 窗口不进遮罩（沿用 ocr.error.noWindows 行内红字）。
  /* 锁定前的归属复检（Review Focus #1），纯函数：*/
  BOOL uiPickerWindowStillOwned(HWND hwnd, DWORD pid);
  ```
- 遮罩行为按 spec §4.1：全屏压暗、单窗口自动锁定、多窗口悬停（窗内全亮+蓝边+顶部 exe — 标题标签；他程序窗口不亮不可点）、DRAG 亮窗口+旧区域橙色虚线+连拖多框+微信尺寸提示+底部工具条（撤销/重选/取消/完成（N））；Esc 整单取消；右键：FRAME 取消整单、DRAG 取消当前未完成拖框；窗口中途死亡/最小化 → 返回 UI_PICKER_WINDOW_DIED；HWND 与新框仅 Done 时写出。v1 单显示器（鼠标所在显示器 work area）。
- 接线（ui_ocr.c）：
  - 选择窗口按钮：枚举 0 → `ocr.error.noWindows`；≥1 → `uiPickerPick(exe, TRUE, g_hasPicked, g_hasPicked?&win:NULL, modelRects...)`；OK → 提交 outWindow（刷新窗口行，0 新框也提交）+ 仅把 `[existingCount..)` 新框 AppendRegions；WINDOW_DIED → 清 picked 状态并显示 `ocr.error.windowDied`。
  - 添加区域按钮：需 g_hasPicked 且 `captureIsCapturable`（否则 windowStale）；`uiPickerPick(exe, FALSE, TRUE, &win, modelRects...)`，OK 追加新框。
  - `uiOcrChooseWindow` 保留为「枚举+0/1 直取」的无 UI 策略函数（既有测试与 manual_ui 使用）；多窗口不再弹 IupList（该路径现实中只被按钮 UI 走到，改为遮罩；策略函数多窗口可返回 FALSE？—— 保持其现有语义会复活列表，故多窗口分支改调 uiPickerPick 不现实（无 FRAME 自动化夹具）。裁决：uiOcrChooseWindow 保留 0/1 策略，n>1 时返回 FALSE（调用方仅测试与 manual_ui 手动场景），生产按钮改用新的 uiOcrChooseAndPick 流程；在 ledger 记录此裁决。）
- 新 i18n 键：`ocr.picker.frameHint` 点击本程序要识别的窗口（Esc 取消，右键取消） / Click the target window of this application (Esc to cancel)；`ocr.picker.dragHint` 改文案：在亮框窗口内按住左键拖出识别区域，可连拖多个；右键取消当前框，Esc 退出 / Drag one or more regions inside the highlighted window; right-click cancels the current drag, Esc exits；`ocr.picker.undo` ↩ 撤销上一个框 / ↩ Undo last box；`ocr.picker.repick` ↖ 重选窗口 / ↖ Choose another window；`ocr.picker.cancel2` ✕ 取消 / ✕ Cancel（避开既有 ocr.picker.cancel 给 IupAlarm 复用）；`ocr.picker.done` ✓ 完成（%u 个区域） / ✓ Done (%u regions)；`ocr.picker.sizeTip` %d × %d / %d × %d；`ocr.error.windowDied` 目标窗口在选择过程中关闭或最小化，请重新选择 / The target window closed or minimized during selection; please re-pick。

- [ ] **Step 1: 写失败测试（RED）**

test_ui_simple 真实窗口段新增（不跑遮罩 UI，测纯复检与入口策略）：
- 创建本进程可见 Static 窗口：`uiPickerWindowStillOwned(hwnd, GetCurrentProcessId())` TRUE；`DestroyWindow` 后 FALSE；另用 `GetCurrentProcessId()+1` 之类不可能 pid 对活窗口断言 FALSE（系统空闲进程 pid 0 更稳妥：对活窗口传 pid=0 断言 FALSE，自身窗口 pid 必非 0）。
- 状态机以外的遮罩参数门禁：initialWindow=NULL 且 startFrame=FALSE → UI_PICKER_CANCEL（或 FALSE 风格失败），不得弹任何窗（headless 下能返回即可）。
- 旧 chooser seam 引用全部删除；编译通过即证明列表死亡。

- [ ] **Step 2: 确认失败**

Run: `bash scripts/run_tests.sh`
Expected: ui_simple 编译失败（uiPickerWindowStillOwned/uiPickerPick 未定义）。

- [ ] **Step 3: 实现遮罩与接线（GREEN）**

ui_picker.c：删 ListState 整段与旧 Overlay 单阶段流程；新 Overlay 持有 PickerMachine + 枚举 wins 数组 + FRAME/DRAG 两套绘制与输入：
- FRAME：canvas BUTTON_CB 命中测试用 `WindowFromPoint`（WS_EX_TRANSPARENT 遍历）找到点下 HWND，在枚举 wins 中按 hwnd 匹配，锁定前调 `uiPickerWindowStillOwned(hwnd, win.pid)`，不符则忽略点击；hover 经 MOTION_CB 同样匹配并 InvalidateRect；单窗口 init 后 AutoLock 直接进 DRAG。
- DRAG：绘制锁定窗口亮区（按 GetClientRect+ClientToScreen 算屏幕矩形）、旧区域橙虚线、新框蓝边、尺寸 tooltip；release 走 uiPickerDragToNorm（clamp 客户区），成功入机；工具条为贴底独立子控件行（IupHbox 4 个 FlatButton + 完成钮按 CanFinish 切 ACTIVE），回调直接驱动 machine Undo/Reframe/完成/取消。
- 每次动作前校验锁定窗 `captureIsCapturable`，失败 → finish(DIED)。
- 出口拷贝 machine：outWindow=wins[locked]（重选后为新锁），outRects=machine.rects 全量、count=rectCount（调用方靠 existingCount 区分新旧）。
- 测试 seam：uiPickerSetDragForTest/CompleteDragForTest 接到 machine AddRect；uiPickerOverlayHwndForTest 保留。
ui_ocr.c：按上面接线改 chooseWindowCb/addRegionCb，existing 矩形从 uiOcrGroupsRegionRects 取。

Run: `bash scripts/run_tests.sh`
Expected: 17/17 PASS。

- [ ] **Step 4: build-debug 通过**

Run: `SIGN=A scripts/build-debug.sh`（msys bash），Expected: 无 error 无新 warning。

- [ ] **Step 5: manual_ui 真机预览冒烟（不阻塞自动流程，结果记入 ledger）**

用既有 picker mode 启动 manual_ui（参数契约：argv 模式名 `picker` 系列；若模式参数与新流程不符，同步更新其模式实现到新 uiPickerPick 调用），自己用 SendInput 或人工截图留档到 docs/superpowers/specs/screenshots/ocr-t8/（若当时环境无法交互则跳过并在 ledger 标注，交给 Task 7 物理清单）。

- [ ] **Step 6: 提交**

```bash
git add src/ui_picker.h src/ui_picker.c src/ui_ocr.c src/ui_ocr.h src/i18n.c tests/test_ui_simple.c tests/manual_ui.c docs/superpowers/specs/screenshots/
git commit -m "feat: WeChat-style window framing and multi-region drag overlay"
```

---

### Task 7: 文档、全量验证、发布包与整支评审

**Files:**
- Modify: `docs/superpowers/specs/2026-10-25-ocr-physical-checklist.md`（新增三组验收项）, `packaging/README-QuickStart.txt`（⑤ 块说明与截图清单更新）
- 视 Task 6 Step 5 情况补充 `tests/manual_ui.c` picker 模式。

- [ ] **Step 1: 物理验收清单新增三类条目**

1. 遮罩分支：0 窗口行内红字；单窗口直进拖框；多窗口悬停高亮+点击锁定（重点验证他程序窗口不可点）；连拖多框/撤销/重选（旧框保留、新框丢弃）/Esc/右键/完成门（首次无框禁用、改指 0 框可完成）；拖框中关目标窗 → 红字提示。
2. 组 AND 真机：R1+R2 双保险，挡住/误判其中一个区域一个识别间隔不切档，连续 N 帧才切；同 tick 两组取表序第一组档位，CSV 每个触发组各一行。
3. 迁移：用 0.3.1 的 ocr_config.txt（rule.N.*）启动 → 区域表/组表各 N 行、行为不变；开始一次后文件落新格式。
另加：多显示器只覆盖当前显示器；展开状态添加区域对话框不跳动（Review Focus #5）。

- [ ] **Step 2: Quick Start 更新**

⑤ 小节改写：选窗交互（框选→拖框→工具条）、区域表+规则组表用法（双击组编辑、组内 AND 语义、删区域联动）、绿色按钮唯一入口；列出需要替换/新增的截图文件名（截图本身由真机验收时补）。

- [ ] **Step 3: 全量验证**

Run: `bash scripts/run_tests.sh` → 17/17；
`SIGN=A scripts/build-debug.sh` 无新警告；
`SIGN=A scripts/build-release.sh` 成功；
确认版本号（src 中 CLUMSY_VERSION；本次为 0.4.0 新功能分支——执行时先 grep 现状值，若仍 0.3.1，按 chore 提交 bump 到 0.4.0，package 名随之）。
`SIGN=A scripts/package-release.sh` → zip 生成；unzip -l 核对条目数与 0.3.1 一致结构；strings 抽查版本号。

- [ ] **Step 4: 整支全新评审**

用 Agent 派 code-reviewer（fresh context）对 `git merge-base master HEAD..HEAD` 全量 diff 评审，喂入本计划路径、spec 路径、Review Focus 5 条与 ledger 的 Ruling 行。CRITICAL/HIGH 当场按 TDD 修复（RED→GREEN→全量绿），MEDIUM/LOW 记入最终报告。

- [ ] **Step 5: 清理与收尾（不删未跟踪的他人物）**

停止 brainstorming visual companion 后台服务；删除本会话 scratch（build/i18n-hunk1.patch、scratch cap.ps1、diag 临时 BMP/PNG）；保留 docs 截图。最终 ledger 汇总所有 Ruling 与遗留 minor。**不 merge、不 push。**

- [ ] **Step 6: 提交文档与版本**

```bash
git add docs/superpowers/specs/2026-10-25-ocr-physical-checklist.md packaging/README-QuickStart.txt
git commit -m "docs: physical checklist and quick start for picker and rule groups"
# 如做了版本 bump：
git commit -m "chore: bump version to 0.4.0" -- <涉及文件>
```
