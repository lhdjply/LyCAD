// =============================================================================
//  LyCAD - ui/DocumentSession.cpp
// =============================================================================
#include "ui/DocumentSession.h"

#include "core/Document.h"
#include "interact/ToolManager.h"
#include "render/CadCanvas.h"

namespace lycad {

DocumentSession::DocumentSession(QObject* parent)
    : QObject(parent)
{
    // 三者的接线与单文档时代 MainWindow 构造函数里完全一致：
    // 画布持有文档负责「画」，工具管理器持有文档与画布负责「改」，
    // 画布的输入事件统一路由给工具管理器。
    document = new Document(this);
    // 画布是 QWidget，父对象只能是窗体层级——传入空指针，addTab 把它
    // 放进标签页容器时会被 QTabWidget 认领，生命周期仍随会话结束
    canvas   = new CadCanvas(nullptr);
    canvas->setDocument(document);

    tools = new ToolManager(this);
    tools->setDocument(document);
    tools->setCanvas(canvas);
    canvas->setInputHandler(tools);

    // 栅格吸附间距与画布的网格设置取同一口径
    tools->snapSettings().gridSpacing = canvas->display().gridSpacing;
}

DocumentSession::~DocumentSession()
{
    delete canvas;   // 见头文件中的说明：画布不在 Qt 父子链上，手动回收
}

} // namespace lycad
