# =============================================================================
#  LyCAD - 开发工具
#
#  这里的程序不随软件一起发布，只用于开发期验证：
#
#   lycad_sample     生成参考 DXF（供外部 CAD 工具交叉校验），
#                    也能读取并打印任意 DXF 的摘要。它只链接 core 与 io，
#                    因此顺带充当「下层不依赖界面层」的编译期契约检查——
#                    一旦 core/io 反向引入了渲染或界面依赖，这里会立刻链接失败。
#
#   lycad_ui_snapshot 在进程内渲染主窗口为 PNG，并用真实鼠标事件驱动交互链路。
#                    它按定义必须链接 ui，故不会削弱上面那个契约检查。
# =============================================================================
add_executable(lycad_sample dxf_sample_generator.cpp)
target_include_directories(lycad_sample PRIVATE "${LYCAD_SRC_ROOT}")
target_link_libraries(lycad_sample
    PRIVATE LyCAD::Core LyCAD::Io Qt6::Core Qt6::Gui LyCAD::Warnings
)

# 界面快照需要 QTest 来注入鼠标事件
find_package(Qt6 QUIET COMPONENTS Test)

if(TARGET Qt6::Test)
    add_executable(lycad_ui_snapshot ui_snapshot.cpp)
    target_include_directories(lycad_ui_snapshot PRIVATE "${LYCAD_SRC_ROOT}")
    target_link_libraries(lycad_ui_snapshot
        PRIVATE LyCAD::Ui Qt6::Test LyCAD::Warnings
    )
endif()
