// =============================================================================
//  LyCAD - interact/ToolManager.h
//  命令注册表与输入分发中枢。
//
//  ToolManager 同时承担两个角色，二者天然重合：
//    1. 画布的 CanvasInputHandler —— 决定每个事件交给哪个工具；
//    2. 命令注册表 —— 把命令行/工具栏的命令名映射到工具实例。
//
//  事件路由规则
//    · 有活动命令时，事件交给它；
//    · 没有活动命令时，事件交给「默认工具」（选择工具）。
//  因此「选择」不是特例分支，而只是恰好作为兜底的那个工具——
//  这消除了大量散布在各处的 if (无命令) 判断。
//
//  命令的取消语义遵循 CAD 惯例：任何新命令都会终止正在执行的命令，
//  Esc 与右键同样终止当前命令并回到选择状态。
// =============================================================================
#pragma once

#include "core/Types.h"
#include "interact/ToolContext.h"
#include "render/CanvasInputHandler.h"
#include "render/CanvasTypes.h"
#include "render/EntityRenderer.h"

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <vector>

class QKeyEvent;
class QPainter;

namespace lycad {

class Tool;
class Document;
class CadCanvas;

class ToolManager : public QObject, public CanvasInputHandler {
    Q_OBJECT

public:
    using ToolFactory = std::function<std::unique_ptr<Tool>(ToolContext&)>;

    explicit ToolManager(QObject* parent = nullptr);
    ~ToolManager() override;

    void setDocument(Document* document);
    void setCanvas(CadCanvas* canvas);

    ToolContext& context() noexcept { return m_context; }
    const ToolContext& context() const noexcept { return m_context; }

    // ---- 命令注册表 ----------------------------------------------------------
    void registerTool(const QString& command, const QStringList& aliases,
                      const QString& description, ToolFactory factory);

    bool hasCommand(const QString& name) const;
    QStringList commandNames() const;
    /// 形如 "LINE (L) — 绘制直线" 的帮助列表，供命令列表对话框使用
    QStringList commandHelp() const;
    /// 前缀补全，返回匹配的命令名
    QStringList complete(const QString& prefix, int limit = 16) const;

    // ---- 执行 ----------------------------------------------------------------
    /// 执行命令。返回 false 表示命令不存在（此时会输出提示）
    bool execute(const QString& command);

    /// 命令行输入：若当前有活动命令则把文本交给它，否则按命令处理。
    /// 支持直接输入坐标，此时交给活动工具。
    bool submitText(const QString& text);

    /// 取消当前命令（Esc / 右键）
    void cancel();

    /// 结束当前命令并回到默认工具（由工具调用）
    void finishCurrentTool();

    /// 重复上一条命令（空格 / 回车）
    bool repeatLastCommand();

    Tool* activeTool() const noexcept { return m_activeTool.get(); }
    Tool* defaultTool() const noexcept { return m_defaultTool.get(); }

    /// 是否有绘制/编辑命令正在执行
    bool isCommandRunning() const noexcept { return m_activeTool != nullptr; }

    QString lastCommandName() const noexcept { return m_lastCommand; }

    // ---- 交互模式 ------------------------------------------------------------
    bool orthoMode() const noexcept { return m_context.orthoMode; }
    void setOrthoMode(bool enabled);

    bool polarTracking() const noexcept { return m_context.polarTracking; }
    void setPolarTracking(bool enabled);

    bool gridSnap() const noexcept { return m_context.snap.grid; }
    void setGridSnap(bool enabled);

    SnapSettings& snapSettings() noexcept { return m_context.snap; }
    const SnapSettings& snapSettings() const noexcept { return m_context.snap; }

    // ---- 供 ToolContext 回调 --------------------------------------------------
    void postMessage(const QString& text);
    void notifyPromptChanged();

    // ---- CanvasInputHandler --------------------------------------------------
    bool handleMousePress(const CanvasEvent& event) override;
    bool handleMouseMove(const CanvasEvent& event) override;
    bool handleMouseRelease(const CanvasEvent& event) override;
    bool handleMouseDoubleClick(const CanvasEvent& event) override;
    bool handleKeyPress(QKeyEvent* event) override;

    void paintOverlay(QPainter& painter, const EntityRenderer::DrawContext& context) override;
    QString promptText() const override;
    Qt::CursorShape cursorShape() const override;

signals:
    void promptChanged(const QString& text);
    void commandMessage(const QString& text);
    void commandStarted(const QString& command);
    void commandEnded(const QString& command);
    void modeChanged();

private:
    struct CommandEntry {
        QString     name;
        QStringList aliases;
        QString     description;
        ToolFactory factory;
    };

    void registerBuiltinTools();
    void ensureDefaultTool();
    Tool* eventTarget() const;
    const CommandEntry* findCommand(const QString& name) const;

    ToolContext m_context;
    std::vector<CommandEntry> m_commands;

    std::unique_ptr<Tool> m_defaultTool;
    std::unique_ptr<Tool> m_activeTool;

    QString m_lastCommand;
    Document* m_document = nullptr;
    CadCanvas* m_canvas = nullptr;
};

} // namespace lycad
