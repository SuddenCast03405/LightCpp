-- zh-cn.lua — 简体中文（中国大陆）界面语言包。
--
-- 一个标准的 LightC++ Lua 插件示例：在沙箱里运行，通过 `ide.ui` 的白名单
-- API，把主窗口的全部菜单标题和动作文本换成简体中文。
--
-- 机制说明：
--   * 脚本在自己的沙箱 Lua 5.4 状态里运行：内存有硬上限，执行有指令数
--     和时间预算；io / debug / package / load 均不可用，print 输出转投
--     插件日志。
--   * plugin 表声明元数据；onInit 在 IDE 完成插件加载时被调用。

plugin = {
    name = "zh-CN 简体中文",
    version = "1.0.0",
    publisher = "LightCpp",
    description = "把 LightC++ 的界面文本换成简体中文（中国大陆）",
}

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

    ide.log("简体中文语言包已加载：" ..
            tostring(ui.menuTitle("menuFile")) ..
            " / " .. tostring(ui.actionText("actionNew")))
end
