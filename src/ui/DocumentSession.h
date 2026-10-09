// =============================================================================
//  LyCAD - ui/DocumentSession.h
//  文档会话：一个标签页对应的「文档 + 画布 + 工具管理器」组合。
//
//  多标签页拆分前，MainWindow 只有一份文档/画布/工具管理器；拆分后每个
//  标签页各持一套，三者必须成对出现、成对接线（画布把事件交给工具管理器、
//  工具管理器把预览画回画布），因此把它们收进一个会话对象统一创建，
//  MainWindow 只负责维护会话列表与「当前活动会话」指针。
//
//  界面上其余部件（命令行、图层/属性面板、功能区、状态栏）仍是全局唯一的，
//  通过 MainWindow 的活动会话快捷指针（m_document / m_canvas / m_toolManager）
//  访问当前标签页的文档与画布。
// =============================================================================
#pragma once

#include <QObject>

namespace lycad {

class Document;
class CadCanvas;
class ToolManager;

class DocumentSession : public QObject {
public:
    explicit DocumentSession(QObject* parent = nullptr);
    /// 画布不是本会话的 QObject 子对象（它被标签页容器认领、关闭标签页后
    /// 处于无父状态），必须在这里显式回收；文档与工具管理器由 Qt 父子机制清理
    ~DocumentSession() override;

    Document*    document = nullptr;   ///< 几何模型（会话私有）
    CadCanvas*   canvas   = nullptr;   ///< 画布（被 QTabWidget 认领为页面）
    ToolManager* tools    = nullptr;   ///< 命令与交互（会话私有）
};

} // namespace lycad
