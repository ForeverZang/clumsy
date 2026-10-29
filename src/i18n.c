// Central UI string table with live language switching. Log lines stay
// English by design; only UI text goes through tr().
#include <stdlib.h>
#include <string.h>

#include "i18n.h"

typedef struct {
    const char *key;
    const char *zh;
    const char *en;
} I18nEntry;

// Full UI vocabulary. All text is UTF-8 (main enables IUP UTF8MODE).
// Rows containing printf specifiers keep them in the same order and
// count in both languages (enforced by tests/test_i18n.c).
static const I18nEntry g_table[] = {
    // Window title and generic verbs
    {"app.title",       "游戏流量控制器", "Game Traffic Controller"},
    {"start",           "开始",           "Start"},
    {"stop",            "停止",           "Stop"},

    // Beginner: direction segmented control
    {"direction.all",   "全部",           "Both"},
    {"direction.in",    "流入（下载）",   "Inbound (download)"},
    {"direction.out",   "流出（上传）",   "Outbound (upload)"},

    // Section 1: target application
    {"section.app",     "① 目标应用",     "① Application"},
    {"app.all",         "不限制应用",     "All applications"},
    {"app.hint",        "只会影响选中的应用，其他程序正常上网",
                        "Only the selected app is affected; everything else stays online"},
    {"app.refresh",     "刷新",           "Refresh"},
    {"app.lineProduct", "%s — %s（%u 个进程）",
                        "%s — %s (%u processes)"},
    {"app.line",        "%s（%u 个进程）", "%s (%u processes)"},

    // Section 2: direction
    {"section.direction", "② 流量方向",   "② Direction"},

    // Section 3: network condition
    {"section.condition", "③ 网络状况",   "③ Network condition"},
    {"condition.kind",  "丢包 / 断网",    "Packet loss / disconnect"},
    {"preset.normal",   "正常 0%",        "Normal 0%"},
    {"preset.severe",   "严重丢包 95%",   "Severe 95%"},
    {"preset.offline",  "完全断网 100%",  "Offline 100%"},
    {"timer.enable",    "定时自动恢复",   "Auto-recover"},
    {"timer.seconds",   "秒",             "seconds"},
    {"timer.countdown", "剩余 %u 秒",     "%u s left"},
    {"condition.hint",  "拖到 100% 就像拔掉网线：网页立刻打不开；中间数值是网页刷半天、偶尔能打开",
                        "100% feels like an unplugged cable — pages fail instantly; mid values make pages crawl and sometimes load"},

    // Section 4: live traffic chart
    {"section.chart",       "④ 实时流量",       "④ Live traffic"},
    {"section.chart.app",   "④ %s 实时流量",    "④ %s live traffic"},
    {"chart.down",      "下载",           "Download"},
    {"chart.up",        "上传",           "Upload"},
    {"chart.before",    "开始前 ↓ %s　↑ %s", "Before ↓ %s  ↑ %s"},
    {"chart.now",       "现在 ↓ %s　↑ %s",   "Now ↓ %s  ↑ %s"},

    // Main action and status lines
    {"action.start",            "开始影响", "Start"},
    {"action.stop",             "停止影响", "Stop"},
    {"status.ready",            "已就绪，请选择应用并开始",
                                "Ready — pick an app and press Start"},
    {"status.selectApp",        "请先选择一个应用",
                                "Please select an application first"},
    {"status.affecting",        "正在影响 %s", "Affecting %s"},
    {"status.affectingCountdown", "正在影响 %s · 剩余 %u 秒",
                                "Affecting %s · %u s left"},
    {"status.stopped",          "已停止，应用恢复正常上网",
                                "Stopped — the app is back online"},
    {"error.needAdmin",         "需要管理员权限：请右键以管理员身份运行",
                                "Administrator rights required: right-click clumsy and choose \"Run as administrator\""},
    {"error.driver",            "WinDivert 驱动加载失败，请检查杀毒软件是否拦截",
                                "The WinDivert driver failed to load; check that antivirus is not blocking it"},
    {"error.generic",           "操作失败，请重试",
                                "The operation failed, please try again"},
    {"error.elevateRefused",    "需要管理员权限才能正常工作：请在弹出的 UAC 对话框中点击“是”，或以管理员身份重新运行",
                                "clumsy needs administrator rights: click Yes in the UAC prompt, or run as administrator"},
    {"error.elevateBadPath",    "无法获取自身路径，请将程序放到普通目录后重试",
                                "Failed to get the executable path. Move clumsy to a normal folder and try again"},
    {"error.elevateUnsupported","当前 Windows 版本过低，仅支持 Windows Vista 及以上版本",
                                "Unsupported Windows version. clumsy supports Windows Vista or above"},

    // Main-window tabs and advanced surface
    {"tab.basic",          "基础功能", "Basic"},
    {"advanced.toggle",     "高级功能", "Advanced"},
    {"advanced.hint",       "延迟 · 节流 · 乱序 · 重复 · 篡改 · RST · 限速 · 自定义过滤器",
                            "lag · throttle · out-of-order · duplicate · tamper · RST · bandwidth · custom filter"},
    {"advanced.filtering",  "过滤",     "Filtering"},
    {"advanced.functions",  "功能模块", "Functions"},
    {"advanced.presets",    "预设：",   "Presets:"},
    {"advanced.filterError", "过滤器语法有误，已忽略：%s",
                            "Invalid filter syntax, ignored: %s"},
    {"advanced.loopbackNotice",
        "注意：捕获本机回环（loopback）数据包时，不能包含流入条件。\n"
        "例如 'udp' 需写成 'udp and outbound' 才能生效，详见说明文档。",
        "NOTICE: When capturing localhost (loopback) packets, you CAN'T include inbound criteria.\n"
        "Filters like 'udp' need to be 'udp and outbound' to work. See readme for more info."},
    // Interim advanced-surface status lines; the beginner status keys
    // replace these once the simple window owns the status row.
    {"advanced.started",
        "已开始过滤，勾选下方功能模块即可生效。",
        "Started filtering. Enable functionalities to take effect."},
    {"advanced.stopped",
        "已停止。修改条件后点击开始可再次运行。",
        "Stopped. To begin again, edit criteria and click Start."},

    // Advanced module display names (English stays unchanged)
    {"mod.lag",        "延迟",     "Lag"},
    {"mod.drop",       "丢包",     "Drop"},
    {"mod.throttle",   "节流",     "Throttle"},
    {"mod.ood",        "乱序",     "Out of order"},
    {"mod.duplicate",  "重复",     "Duplicate"},
    {"mod.tamper",     "篡改",     "Tamper"},
    {"mod.reset",      "RST 重置", "Reset"},
    {"mod.bandwidth",  "限速",     "Bandwidth"},

    // Shared advanced control labels
    {"common.inbound",         "流入",          "Inbound"},
    {"common.outbound",        "流出",          "Outbound"},
    {"label.chance",           "概率（%）：",   "Chance(%):"},
    {"lag.delay",              "延迟（毫秒）：", "Delay(ms):"},
    {"throttle.timeframe",     "时间窗口（毫秒）：", "Timeframe(ms):"},
    {"throttle.dropThrottled", "丢弃被节流的包", "Drop Throttled"},
    {"tamper.redoChecksum",    "重算校验和",    "Redo Checksum"},
    {"reset.rstNext",          "立即 RST 下一个包", "RST next packet"},
    {"bandwidth.limit",        "限速（KB/s）：", "Limit(KB/s):"},
    {"duplicate.count",        "重复次数：",    "Count:"},

    // Section 5: OCR automated testing
    {"ocr.title",            "⑤ 自动测试（OCR 识图切档）",
                             "⑤ Automated testing (OCR)"},
    {"ocr.enable",           "启用自动测试（识别窗口文字，自动切换丢包率）",
                             "Enable automated testing (read window text and switch loss automatically)"},
    {"ocr.window",           "目标窗口：",   "Target window:"},
    {"ocr.choose",           "选择窗口…",    "Choose window…"},
    {"ocr.addRegion",        "+ 添加区域（在目标窗口上拖框）",
                             "+ Add region (drag a box on the target window)"},
    {"ocr.col.name",         "名称",         "Name"},
    {"ocr.col.keywords",     "关键词",       "Keywords"},
    {"ocr.kwHint",           "关键词可填多个，用英文逗号分隔，包含其中任一即命中",
                             "Comma-separated keywords; a substring match on any one triggers"},
    {"ocr.col.edge",         "触发条件",     "Trigger"},
    {"ocr.edge.appear",      "文字出现时",   "When text appears"},
    {"ocr.edge.disappear",   "文字消失时",   "When text disappears"},
    {"ocr.col.frames",       "确认帧",       "Frames"},
    {"ocr.col.loss",         "丢包%",        "Loss%"},
    {"ocr.delete",           "删除",         "Delete"},
    {"ocr.interval",         "识别间隔（毫秒，200–60000）：",
                             "OCR interval (ms, 200-60000):"},
    {"ocr.changeDetect",     "画面未变化时跳过识别",
                             "Skip OCR when the frame is unchanged"},
    {"ocr.csvPath",          "CSV 时间线：", "CSV timeline:"},
    {"ocr.openDir",          "打开目录",     "Open folder"},
    {"ocr.winNone",          "（尚未选择窗口）", "(no window selected yet)"},
    {"ocr.blockedApp",       "请先在上方选择一个具体应用：不能在“不限制应用”下启用自动测试",
                             "Pick a specific application above; automated testing cannot run with \"All applications\""},
    {"ocr.error.windowStale", "找不到目标窗口（可能已关闭或标题变化），请重新选择窗口",
                              "Target window not found (closed or renamed); choose the window again"},
    {"ocr.error.noWindows",   "该应用当前没有可见的顶层窗口，请确认程序已启动且窗口未最小化",
                              "This application has no visible top-level window; make sure it is running and not minimized"},
    {"ocr.picker.ok",         "确定", "OK"},
    {"ocr.picker.cancel",     "取消", "Cancel"},
    {"ocr.picker.untitled",   "（无标题窗口）", "(untitled window)"},
    {"ocr.picker.frameHint",  "点击本程序要识别的窗口（Esc 取消，右键取消）",
                              "Click the target window of this application (Esc to cancel)"},
    {"ocr.picker.dragHint",   "在亮框窗口内按住左键拖出识别区域，可连拖多个；右键取消当前框，Esc 退出",
                              "Drag one or more regions inside the highlighted window; right-click cancels the current drag, Esc exits"},
    {"ocr.picker.undo",       "↩ 撤销上一个框", "↩ Undo last box"},
    {"ocr.picker.repick",     "↖ 重选窗口", "↖ Choose another window"},
    {"ocr.picker.cancel2",    "✕ 取消", "✕ Cancel"},
    {"ocr.picker.done",       "✓ 完成（%u 个区域）", "✓ Done (%u regions)"},
    {"ocr.picker.sizeTip",    "%d × %d", "%d × %d"},
    {"ocr.error.windowDied",  "目标窗口在选择过程中关闭或最小化，请重新选择",
                              "The target window closed or minimized during selection; please re-pick"},
    {"ocr.region.title",     "识别区域（固定名称可双击改名）",
                             "OCR regions (double-click a name to rename)"},
    {"ocr.group.title",      "规则组（组内多区域为“且”关系）",
                             "Rule groups (regions in a group combine with AND)"},
    {"ocr.addGroup",         "+ 新建规则组", "+ New group"},
    {"ocr.col.rnum",         "#",            "#"},
    {"ocr.col.rname",        "名称",         "Name"},
    {"ocr.col.rpos",         "位置（归一化坐标）", "Position (normalized)"},
    {"ocr.col.gnum",         "组",           "Group"},
    {"ocr.col.gsummary",     "条件摘要",     "Condition summary"},
    {"ocr.groupName",        "组 %d",        "Group %d"},
    {"ocr.group.empty",      "（空组：启动前请双击编辑）",
                             "(empty group: double-click to edit before starting)"},
    {"ocr.group.dlgNew",     "新建规则组",   "New rule group"},
    {"ocr.group.dlgEdit",    "编辑规则组 %d", "Edit rule group %d"},
    {"ocr.group.members",    "勾选区域并填写该区域要识别的关键词（多个关键词用英文逗号分隔，任一命中即可）",
                             "Check regions and enter keywords for each (comma-separated; any one matches)"},
    {"ocr.group.kwHint",     "同组多个区域之间为“且”：所有区域同时命中时条件成立",
                             "Regions in a group combine with AND: the condition holds only when all match"},
    {"ocr.group.ok",         "确定",         "OK"},
    {"ocr.group.cancel",     "取消",         "Cancel"},
    {"ocr.delRegion.confirm","删除区域「%s」？引用它的规则组成员会一并移除。",
                             "Delete region \"%s\"? Group members referencing it are removed too."},
    {"ocr.limitRegions",     "最多 16 个识别区域", "At most 16 OCR regions"},
    {"ocr.limitGroups",      "最多 16 个规则组",   "At most 16 rule groups"},
    {"ocr.error.groupMembers", "请至少勾选一个区域", "Select at least one region"},
    {"ocr.error.groupKeywords", "每个勾选区域都要填写关键词",
                                "Every selected region needs keywords"},
    {"ocr.error.row",        "第 %u 行：%s", "Row %u: %s"},
    {"ocr.error.keywords",   "关键词不能为空", "keywords must not be empty"},
    {"ocr.error.frames",     "确认帧数需 ≥ 1", "confirm frames must be >= 1"},
    {"ocr.error.loss",       "丢包率必须在 0..100 之间",
                             "loss percent must be within 0..100"},
    {"ocr.error.number",     "请输入整数",   "an integer is required"},
    {"ocr.error.interval",   "识别间隔需为 200..60000 之间的整数毫秒",
                             "interval must be an integer between 200 and 60000 ms"},
    {"ocr.error.noRegions",  "请先添加至少一个识别区域",
                             "Add at least one OCR region first"},
    {"ocr.error.noGroups",   "请先新建至少一个规则组",
                             "Create at least one rule group first"},
    {"ocr.error.engine",     "OCR 引擎启动失败，请确认目录下 models 内模型文件完整",
                             "OCR engine failed to start; make sure the models folder next to the program is complete"},
    {"ocr.error.seed",       "初次识别没有任何区域可读取，请确认目标窗口未最小化且识别区域在窗口内",
                             "Initial OCR scan could read no region; make sure the window is not minimized and the regions are inside it"},
    {"ocr.error.timeline",   "无法创建 CSV 时间线文件",
                             "Cannot create the CSV timeline file"},
    {"ocr.error.worker",     "OCR 工作线程启动失败",
                             "Failed to start the OCR worker thread"},
    {"ocr.error.configSave", "注意：OCR 配置未能保存到磁盘",
                             "Warning: the OCR configuration could not be saved"},
    {"ocr.log.start",        "OCR 自动测试已开始，初始丢包率 %d%%",
                             "OCR automation started at %d%% loss"},
    {"ocr.log.stop",         "OCR 自动测试已停止（CSV 已落盘）",
                             "OCR automation stopped (CSV flushed to disk)"},
    {"ocr.log.loss",         "丢包率切换为 %d%%",
                             "Loss switched to %d%%"},
    {"ocr.log.gearNow",      "实时丢包率：%d%%",
                             "Live loss: %d%%"},
    {"ocr.log.gearIdle",     "实时丢包率：--",
                             "Live loss: --"},
    {"ocr.log.gearTag",      "丢包 %d%%",
                             "loss %d%%"},
    {"ocr.log.trigger",      "规则「%s」触发（识别文字：%s），丢包率 → %d%%",
                             "Rule \"%s\" triggered (OCR text: %s), loss -> %d%%"},
    {"ocr.log.unavailable",  "区域「%s」抓取不可用，本次跳过",
                             "Region \"%s\" is unavailable; skipped this poll"},
    {"ocr.log.emptyText",    "（无文字）", "(no text)"},
    {"ocr.log.scan.ok",      "识别区域 %s 识别成功：%s",
                             "Region %s recognized: %s"},
    {"ocr.log.scan.unchanged",
                             "识别区域 %s 画面未变化，沿用文字：%s",
                             "Region %s unchanged; reused text: %s"},
    {"ocr.log.scan.capture", "识别区域 %s 抓取画面失败，本次跳过",
                             "Region %s screen capture failed; skipped this cycle"},
    {"ocr.log.scan.blank",   "识别区域 %s 抓到空画面（锁屏或独占全屏？），本次跳过",
                             "Region %s got a blank frame (locked screen or exclusive fullscreen?); skipped"},
    {"ocr.log.scan.read",    "识别区域 %s 文字识别失败，本次跳过",
                             "Region %s OCR read failed; skipped this cycle"},
};

static Lang g_lang = LANG_EN;

typedef struct {
    Ihandle *control;
    char    *attr;
    char    *key;
} Binding;

static Binding *g_bindings = NULL;
static size_t   g_bindingCount = 0;

// Keys already reported missing, so each is LOG'd exactly once.
static char   **g_missing = NULL;
static size_t   g_missingCount = 0;

Lang i18nDetect(void) {
    LANGID id = GetUserDefaultUILanguage();
    return PRIMARYLANGID(id) == LANG_CHINESE ? LANG_ZH : LANG_EN;
}

void i18nSetLang(Lang lang) {
    g_lang = lang;
}

Lang i18nGetLang(void) {
    return g_lang;
}

static const I18nEntry *findEntry(const char *key) {
    if (key == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(g_table) / sizeof(g_table[0]); i++) {
        if (strcmp(g_table[i].key, key) == 0) {
            return &g_table[i];
        }
    }
    return NULL;
}

static BOOL missingAlreadyLogged(const char *key) {
    for (size_t i = 0; i < g_missingCount; i++) {
        if (strcmp(g_missing[i], key) == 0) {
            return TRUE;
        }
    }
    char *copy = _strdup(key);
    if (copy == NULL) {
        return TRUE; // fail quiet: logging must never break the UI
    }
    char **grown = realloc(g_missing, sizeof(char *) * (g_missingCount + 1));
    if (grown == NULL) {
        free(copy);
        return TRUE;
    }
    g_missing = grown;
    g_missing[g_missingCount++] = copy;
    return FALSE;
}

const char *tr(const char *key) {
    if (key == NULL) {
        return "";
    }
    const I18nEntry *entry = findEntry(key);
    if (entry != NULL) {
        return g_lang == LANG_ZH ? entry->zh : entry->en;
    }
    if (!missingAlreadyLogged(key)) {
        LOG("i18n: missing key '%s'", key);
    }
    return key;
}

static char *dupString(const char *s) {
    char *copy = _strdup(s != NULL ? s : "");
    if (copy == NULL) {
        LOG("i18n: out of memory binding attribute");
    }
    return copy;
}

void i18nBind(Ihandle *control, const char *attr, const char *key) {
    if (control == NULL || attr == NULL || key == NULL) {
        return;
    }
    Binding *grown = realloc(g_bindings,
                             sizeof(Binding) * (g_bindingCount + 1));
    if (grown == NULL) {
        LOG("i18n: out of memory binding control");
        return;
    }
    g_bindings = grown;
    g_bindings[g_bindingCount].control = control;
    g_bindings[g_bindingCount].attr = dupString(attr);
    g_bindings[g_bindingCount].key = dupString(key);
    if (g_bindings[g_bindingCount].attr == NULL
        || g_bindings[g_bindingCount].key == NULL) {
        free(g_bindings[g_bindingCount].attr);
        free(g_bindings[g_bindingCount].key);
        return;
    }
    g_bindingCount++;
    // Apply immediately: controls created without an explicit value (e.g.
    // IupFrame titles set only via the binding) must render in the current
    // language before the first i18nRetranslate() call.
    IupStoreAttribute(control, attr, tr(key));
}

void i18nRetranslate(void) {
    for (size_t i = 0; i < g_bindingCount; i++) {
        IupSetAttribute(g_bindings[i].control, g_bindings[i].attr,
                        tr(g_bindings[i].key));
    }
}

size_t i18nTableCount(void) {
    return sizeof(g_table) / sizeof(g_table[0]);
}

void i18nTableEntry(size_t index, const char **key,
                    const char **zh, const char **en) {
    if (index >= i18nTableCount()) {
        *key = *zh = *en = "";
        return;
    }
    *key = g_table[index].key;
    *zh = g_table[index].zh;
    *en = g_table[index].en;
}
