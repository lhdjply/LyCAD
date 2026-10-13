// =============================================================================
//  LyCAD - main.cpp
//  程序入口。
//
//  这里只做三件事：初始化应用元信息、注册文件编解码器、装配并显示主窗口。
//  任何业务逻辑都不应出现在本文件中——它应当短到无需注释即可读明白。
// =============================================================================
#include "io/CadFile.h"
#include "ui/IconLibrary.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QStringList>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);

    QApplication::setApplicationName(QStringLiteral("LyCAD"));
    QApplication::setApplicationDisplayName(QStringLiteral("LyCAD"));
    // 版本号由构建系统以 LYCAD_VERSION 宏注入：唯一来源是仓库根的 VERSION 文件
    //（读取见 CMakeLists.txt 开头，注入见 add_compile_definitions(LYCAD_VERSION)），
    // 此处不写字面量。
    QApplication::setApplicationVersion(QStringLiteral(LYCAD_VERSION));
    QApplication::setOrganizationName(QStringLiteral("LyCAD"));
    QApplication::setOrganizationDomain(QStringLiteral("lycad.local"));

    // 应用图标：窗口标题栏、任务栏、Alt+Tab 与后续弹出的对话框都用它。
    // 在应用级设置一次即可，不必逐个窗口设置。图标不随主题换色。
    QApplication::setWindowIcon(lycad::IconLibrary::applicationIcon());

    // 编解码器注册是幂等的；在此显式调用一次，
    // 使得 --help 等信息不依赖主窗口的构造顺序。
    lycad::registerBuiltinCodecs();

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("LyCAD —— 基于 Qt6 的二维 CAD 绘图软件，支持 DXF / DWG 图形文件。"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(
        QStringLiteral("file"),
        QStringLiteral("启动后要打开的图形文件（可给多个，每个占一个标签页；可省略）"));

    parser.process(application);

    lycad::MainWindow window;
    window.show();

    for (const QString& path : parser.positionalArguments()) {
        window.openFile(path);
    }

    return application.exec();
}
