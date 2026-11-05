-- zh-cn.lua — 简体中文（中国大陆）界面语言包。
--
-- 一个标准的 LightC++ Lua 插件示例：在沙箱里运行，通过 `ide` 的白名单
-- API，把主窗口的全部菜单标题和动作文本换成简体中文，并内置设置对话框
-- 的完整汉化表（见下方 settingsTexts）。
--
-- 机制说明：
--   * 脚本在自己的沙箱 Lua 5.4 状态里运行：内存有硬上限，执行有指令数
--     和时间预算；io / debug / package / load 均不可用，print 输出转投
--     插件日志。
--   * plugin 表声明元数据；onInit 在 IDE 完成插件加载时被调用。
--   * 设置对话框在 IDE 里是临时构造的，插件 API 目前只覆盖菜单/动作；
--     其汉化表按特性检测提交（契约见文件尾部说明）：当前构建未提供
--     ide.ui.setWidgetTexts(map) 时自动跳过并写日志，不影响其余汉化。

plugin = {
    name = "zh-CN 简体中文",
    version = "1.1.0",
    publisher = "LightCpp",
    description = "把 LightC++ 的界面文本换成简体中文（中国大陆），含设置对话框",
}

-- ---------------------------------------------------------------------------
-- 设置对话框汉化表（英文原文 → 简体中文）
--
-- 键 = src/LightCppApp/Source/SettingsDialog.cpp 中 tr() 的英文原文，值 =
-- 译文。覆盖窗口标题、导航、组框标题、表单标签、按钮、复选框、表头、
-- 特殊值文本与常用 tooltip；多行 tooltip 的键必须与源码里相邻字符串字面量
-- 拼接的结果逐字一致（含换行位置），源码改文案时需同步更新。
--
-- 有意不收的条目（勿添加）：
--   * "(built-in default)" —— fontSpecOf() 与字体联动用它做文本比较；
--     在 C++ 端真正走 tr() 翻译之前替换显示文本，会让"内置默认"占位被
--     当成真实字体名写进配置。
--   * 动态文案（弹窗标题、状态栏消息、打开后生成的列表条目，如
--     "(unnamed kit)"、"New Toolchain"、"New terminal"）——控件树里的
--     静态文本映射覆盖不到它们。
--   * 工具名（Clangd、Clang-Format、CMake、Ninja、GNU Make、MSBuild、
--     Console Pauser）与 "Kit"/"Kits" —— 按惯例保留原文。
-- ---------------------------------------------------------------------------

local settingsTexts = {
    -- 顶部：窗口标题与作用域行
    ["Settings"] = "设置",
    ["Edit:"] = "编辑：",
    ["Global settings"] = "全局设置",
    ["Project settings"] = "项目设置",
    ["The global layer lives in config/ next to the executable; the project layer overlays it from <project>/config."]
        = "全局层位于可执行文件旁的 config/ 目录；项目层从 <项目>/config 覆盖其上。",

    -- 导航（General/Toolchains/Kits 中 "Kits" 保留原文）
    ["General"] = "常规",
    ["Toolchains"] = "工具链",

    -- General 页 · 默认值
    ["Defaults"] = "默认值",
    ["Default kit"] = "默认 Kit",
    ["The kit selected on start-up. Kits are defined on the Kits page."]
        = "启动时选用的 Kit。Kit 在“Kits”页中定义。",
    ["Default build configuration"] = "默认构建配置",
    ["The build configuration (Debug/Release/...) selected on start-up."]
        = "启动时选用的构建配置（Debug/Release 等）。",
    ["Browse..."] = "浏览...",
    ["Git path"] = "Git 路径",
    ["Shared git, used when the active kit does not define its own."]
        = "共享的 Git；当前 Kit 未自带 Git 时使用。",
    ["Clangd path"] = "Clangd 路径",
    ["Shared clangd used for syntax checks and completion."]
        = "共享的 clangd，用于语法检查与补全。",
    ["Clang-Format path"] = "Clang-Format 路径",
    ["Shared clang-format used by the formatting commands."]
        = "共享的 clang-format，供格式化命令使用。",

    -- General 页 · 字体 / 文件 / 常规 / 快捷键 / 大文件 / 语言服务 / 格式化
    ["Fonts"] = "字体",
    ["Toolbar"] = "工具栏",
    ["Editor"] = "编辑器",
    ["Output"] = "输出",
    ["Files"] = "文件",
    ["New-file encoding"] = "新建文件编码",
    ["Detection priority"] = "检测优先级",
    ["Detection order, one name per line (modelled; the detector's actual order is fixed).\nKnown names: UTF-8, GB18030, GBK, Big5, Shift-JIS, EUC-KR, UTF-16LE, UTF-16BE, Windows-1252, ISO-8859-1"]
        = "检测顺序，每行一个名称（仅作记录；检测器的实际顺序是固定的）。\n可用名称：UTF-8、GB18030、GBK、Big5、Shift-JIS、EUC-KR、UTF-16LE、UTF-16BE、Windows-1252、ISO-8859-1",
    ["Line ending"] = "换行符",
    ["Check that the compilers exist at start-up"] = "启动时检查编译器是否存在",
    ["When enabled, start-up verifies the default kit's compilers and offers to open the settings when one is missing."]
        = "启用后，启动时会验证默认 Kit 的编译器，缺失时提示打开设置。",
    ["Program Output"] = "程序输出",
    ["Program output"] = "程序输出方式",
    ["How the built program runs.\nProgram Output: its output is captured in the IDE's Program Output channel, with an input field for stdin.\nConsole Pauser: it runs in its own console window via ConsolePauser.exe (copied next to the program when missing).\nThe other entries come from the editable terminal list below; an embedded terminal captures the output in the Program Output channel, an external one opens its own console window."]
        = "构建出的程序如何运行。\n程序输出：输出被捕获到 IDE 的“程序输出”通道，并提供 stdin 输入框。\nConsole Pauser：经 ConsolePauser.exe 在独立控制台窗口中运行（缺失时会复制到程序旁）。\n其余条目来自下方可编辑的终端列表：嵌入式终端把输出捕获到“程序输出”通道，外部终端打开自己的控制台窗口。",
    ["Command + program path"] = "命令 + 程序路径",
    ["Embedded"] = "嵌入式",
    ["Terminals the program can run in. The command starts the shell (e.g. \"cmd.exe /k\"). Embedded terminals run in the Program Output channel: the program's path is typed in as the first line of input and the input field stays live for stdin. Non-embedded terminals open their own console and get the program path as a command-line argument."]
        = "程序可运行的终端。命令用于启动 shell（如 \"cmd.exe /k\"）。嵌入式终端在“程序输出”通道中运行：程序路径作为第一行输入键入，输入框保持可用以支持 stdin。非嵌入式终端打开自己的控制台，并把程序路径作为命令行参数传入。",
    ["Terminals"] = "终端",
    ["Delete the selected terminal"] = "删除选中的终端",
    ["Shortcuts"] = "快捷键",
    ["Syntax Check (clangd)"] = "语法检查（clangd）",
    ["Toggles Tools > Syntax Check. Clear to disable the shortcut."]
        = "切换“工具 > 语法检查”。清空即禁用该快捷键。",
    ["Format Document (clang-format)"] = "格式化文档（clang-format）",
    ["Format Document. Clear to keep the built-in shortcuts."]
        = "格式化文档。清空则保留内置快捷键。",
    ["Large-file limits"] = "大文件限制",
    ["no limit"] = "无限制",
    ["Disable syntax highlighting above"] = "超过此大小禁用语法高亮",
    ["Files larger than this get no syntax highlighting. 0 = no limit."]
        = "大于此大小的文件不做语法高亮。0 = 无限制。",
    ["Disable clangd indexing above"] = "超过此大小禁用 clangd 索引",
    ["Files larger than this are not pushed to clangd's index. 0 = no limit."]
        = "大于此大小的文件不推送给 clangd 索引。0 = 无限制。",
    ["Disable clangd parsing above"] = "超过此大小禁用 clangd 解析",
    ["Files larger than this are not parsed by clangd. 0 = no limit."]
        = "大于此大小的文件不由 clangd 解析。0 = 无限制。",
    ["Language services"] = "语言服务",
    ["Enable clangd by default for C/C++ files"] = "默认对 C/C++ 文件启用 clangd",
    ["Completion"] = "补全",
    ["Diagnostics"] = "诊断",
    ["Navigation (go to definition)"] = "导航（转到定义）",
    ["Default completion backend"] = "默认补全后端",
    ["Engine that answers code-completion requests.\nclangd / ccls are language servers; libclang parses the file\nin-process. When the chosen engine cannot answer, the request\nis retried the number of times set below."]
        = "响应代码补全请求的引擎。\nclangd / ccls 是语言服务器；libclang 在进程内解析文件。\n所选引擎无法响应时，按下方设置的次数重试。",
    ["no retry"] = "不重试",
    ["Completion retry count"] = "补全重试次数",
    ["How many times a failed completion request is retried\n(server missing, crashed, or returned nothing)."]
        = "补全请求失败时的重试次数\n（服务器缺失、崩溃或无返回）。",
    ["Enable clang-format"] = "启用 clang-format",
    ["Format on save"] = "保存时格式化",
    ["Formatting"] = "格式化",
    ["Global style"] = "全局样式",
    ["Default style content"] = "默认样式内容",
    ["Content for a generated .clang-format (reserved; not consumed yet)."]
        = "生成的 .clang-format 的内容（预留；暂未使用）。",

    -- 工具链页
    ["Toolchains are stored in toolchains.json of the edited layer. Kits reference them by id."]
        = "工具链保存在当前编辑层的 toolchains.json 中，Kit 通过 ID 引用它们。",
    ["Identify Folder..."] = "识别文件夹...",
    ["Pick a folder and every known tool in it (compilers, debugger, clangd, ...) is entered into the selected toolchain."]
        = "选择一个文件夹，其中的已知工具（编译器、调试器、clangd 等）会自动填入所选工具链。",
    ["Add"] = "添加",
    ["Remove"] = "移除",
    ["Check"] = "检查",
    ["Verify that every named tool exists on disk and report what is missing."]
        = "验证每个已命名的工具是否都存在于磁盘上，并报告缺失项。",
    ["Toolchain"] = "工具链",
    ["Id"] = "ID",
    ["Name"] = "名称",
    ["C compiler"] = "C 编译器",
    ["C++ compiler"] = "C++ 编译器",
    ["Debugger"] = "调试器",
    ["Absolute path to the executable; empty = unset."]
        = "可执行文件的绝对路径；留空表示未设置。",

    -- Kit 页
    ["Kits are stored in kits.json of the edited layer."]
        = "Kit 保存在当前编辑层的 kits.json 中。",
    ["Duplicate"] = "创建副本",
    ["Enabled"] = "启用",
    ["Default"] = "默认",
    ["Fallback default when defaults.defaultKitId does not name an enabled kit; the first kit marked Default wins."]
        = "当 defaults.defaultKitId 未指向已启用的 Kit 时的后备默认项；第一个标记为“默认”的 Kit 生效。",
    ["Inherit toolchain"] = "继承工具链",
    ["Fill this kit's tools from the toolchain selected below. Fields the kit names itself still win."]
        = "用下方选择的工具链填充本 Kit 的工具；Kit 自身填写的字段仍然优先。",
    ["The toolchain this kit inherits, as defined on the Toolchains page."]
        = "本 Kit 继承的工具链，在“工具链”页中定义。",
    ["Build tools"] = "构建工具",
    ["Leave empty to use the compiler's default."] = "留空则使用编译器默认值。",
    ["C++ standard"] = "C++ 标准",
    ["Compile flags"] = "编译标志",
    ["Kit-global compile flags, one per line."] = "Kit 级编译标志，每行一条。",
    ["Link flags"] = "链接标志",
    ["Kit-global link flags, one per line."] = "Kit 级链接标志，每行一条。",
    ["Kit-specific git; empty uses the shared default."]
        = "Kit 专用的 Git；留空使用共享默认值。",
    ["Git arguments"] = "Git 参数",
    ["Extra git arguments, one per line."] = "额外的 Git 参数，每行一条。",
    ["Clang-Format style"] = "Clang-Format 样式",
    ["Clangd fallback style"] = "Clangd 后备样式",
    ["Build configurations"] = "构建配置",
    ["Inherit kit flags"] = "继承 Kit 标志",
    ["When unchecked, this configuration replaces the kit's flags instead of extending them."]
        = "未勾选时，此配置将替换而非追加 Kit 的标志。",
    ["One flag per line."] = "每行一条标志。",
    ["Build tool arguments (used when the project defines none)"]
        = "构建工具参数（项目未定义时使用）",
    ["One argument per line."] = "每行一个参数。",
}

local function countEntries(t)
    local n = 0
    for _ in pairs(t) do
        n = n + 1
    end
    return n
end

-- ---------------------------------------------------------------------------
-- 设置对话框汉化的提交逻辑（契约提案）
--
-- 期望 IDE 提供：ide.ui.setWidgetTexts(map) —— map 为 { 英文原文 = 译文 }；
-- IDE 遍历设置对话框的控件树，把命中的可见文本（标题/标签/按钮/条目/
-- 表头/特殊值文本，tooltip 是否覆盖由 C++ 端决定）替换为译文。
-- 若最终契约采用其他名称或形态，只需调整本函数，settingsTexts 不用动。
-- ---------------------------------------------------------------------------

local function applySettingsTexts(ui)
    if type(ui.setWidgetTexts) ~= "function" then
        ide.log("设置对话框汉化：当前 IDE 未提供 ide.ui.setWidgetTexts(map)，"
                .. countEntries(settingsTexts) .. " 条译文已备好、暂不生效"
                .. "（等待 C++ 端支持）")
        return
    end
    ui.setWidgetTexts(settingsTexts)
    ide.log("设置对话框汉化：已提交 " .. countEntries(settingsTexts) .. " 条译文")
end

function plugin.onInit()
    local ui = ide.ui

    -- 顶层与子菜单标题。
    local menus = {
        menuFile     = "文件(&F)",
        menuEdit     = "编辑(&E)",
        menuFolding  = "折叠(&F)",
        menuProject  = "项目(&P)",
        menuTools    = "工具(&T)",
        menuDebug    = "调试(&D)",
        menuView     = "视图(&V)",
        menuGit      = "Git(&G)",
        menuPlugins  = "插件(&P)",
        menuHelp     = "帮助(&H)",
    }
    for id, title in pairs(menus) do
        ui.setMenuTitle(id, title)
    end

    -- 菜单与工具栏动作文本。id 是 IDE 文档化的 objectName；未匹配的 id
    -- 会在插件日志里提示（例如 IDE 版本变更），不会造成任何破坏。
    local actions = {
        -- 文件
        actionNew             = "新建(&N)",
        actionOpen            = "打开(&O)...",
        actionOpenFolder      = "打开文件夹(&F)...",
        actionSave            = "保存(&S)",
        actionSaveAll         = "全部保存(&L)",
        actionExit            = "退出(&X)",
        -- 编辑 / 折叠 / 格式化
        actionFormat          = "格式化文档(&F)",
        actionFormatFolder    = "格式化文件夹(&D)...",
        actionFormatProject   = "格式化项目文件(&J)",
        actionFold            = "折叠(&F)",
        actionUnfold          = "展开(&U)",
        actionFoldAll         = "全部折叠(&A)",
        actionUnfoldAll       = "全部展开(&L)",
        actionSettings        = "设置(&S)...",
        -- 项目
        actionOpenProject     = "打开项目(&J)...",
        actionConfigureProject = "配置项目(&C)",
        actionBuildProject    = "构建项目(&B)",
        actionRunProject      = "运行项目(&J)",
        actionCloseProject    = "关闭项目(&C)",
        -- 工具
        actionSyntaxCheck     = "语法检查(&S)",
        actionReloadSettings  = "重新加载设置(&R)",
        actionOpenConfigDir   = "打开配置文件夹(&C)",
        actionCompile         = "编译当前文件(&E)",
        actionCompileRun      = "编译并运行(&R)",
        actionRun             = "运行(&R)",
        actionStopRun         = "停止程序(&S)",
        -- 调试
        actionToggleBreakpoint = "切换断点(&B)",
        actionDebugStart      = "开始调试(&D)",
        actionDebugStop       = "停止调试(&D)",
        actionDebugContinue   = "继续(&C)",
        actionDebugStepOver   = "单步跳过(&O)",
        actionDebugStepInto   = "单步进入(&I)",
        actionDebugStepOut    = "单步跳出(&U)",
        -- 视图
        actionToggleExplorer  = "资源管理器面板(&E)",
        actionToggleOutput    = "输出面板(&O)",
        -- Git
        actionGitRefresh      = "刷新状态(&R)",
        actionGitStageAll     = "暂存所有更改(&A)",
        actionGitUnstageAll   = "取消暂存所有更改(&U)",
        actionGitCommit       = "提交(&C)...",
        actionGitLog          = "显示日志(&L)",
        actionGitDiff         = "对比工作区(&W)",
        actionGitDiffStaged   = "对比暂存区(&S)",
        actionGitInit         = "初始化仓库(&I)",
        -- 帮助
        actionAbout           = "关于 LightC++(&A)...",
    }
    for id, text in pairs(actions) do
        ui.setActionText(id, text)
    end

    -- 设置对话框汉化（见上方契约说明；当前构建未支持时安全跳过）。
    applySettingsTexts(ui)

    ide.log("简体中文语言包已加载：" ..
            tostring(ui.menuTitle("menuFile")) ..
            " / " .. tostring(ui.actionText("actionNew")))
end
