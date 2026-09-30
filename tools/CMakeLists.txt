# =============================================================================
#  LyCAD - 开发工具
#
#  这里的程序不随软件一起发布，只用于开发期验证。它们编译的源码范围各不相同，
#  恰好也是各自的角色说明：
#
#   lycad_sample      生成参考 DXF（供外部 CAD 工具交叉校验），也能读取并打印
#                     任意 DXF 的摘要。它**只编入 core 与 io**，因此顺带充当
#                     「下层不依赖界面层」的编译期契约检查——一旦 core/io 反向
#                     依赖了渲染或界面，它会立刻因符号缺失而链接失败。
#
#   lycad_icon_sheet  把 resources/icons 下的全部图标渲染成一张总览图，
#                     供人工核对配色与各尺寸下的清晰度。
#
#   lycad_ui_snapshot 在进程内渲染主窗口为 PNG，并用真实鼠标事件驱动交互链路。
#                     后两者需要完整功能，因此编入全部源码。
# =============================================================================

add_executable(lycad_sample
    dxf_sample_generator.cpp
    ${LYCAD_CORE_IO_SOURCES}
)
target_include_directories(lycad_sample PRIVATE "${LYCAD_SRC_ROOT}")
target_link_libraries(lycad_sample PRIVATE Qt6::Core Qt6::Gui LyCAD::Warnings)

add_executable(lycad_icon_sheet
    icon_sheet.cpp
    ${LYCAD_SOURCES}
    ${LYCAD_RESOURCES}
)
target_include_directories(lycad_icon_sheet PRIVATE "${LYCAD_SRC_ROOT}")
target_link_libraries(lycad_icon_sheet PRIVATE ${LYCAD_QT_LIBS} LyCAD::Warnings)
set_target_properties(lycad_icon_sheet PROPERTIES AUTORCC ON)

# 界面快照需要 QTest 来注入鼠标事件
find_package(Qt6 QUIET COMPONENTS Test)

if(TARGET Qt6::Test)
    add_executable(lycad_ui_snapshot
        ui_snapshot.cpp
        ${LYCAD_SOURCES}
        ${LYCAD_RESOURCES}
    )
    target_include_directories(lycad_ui_snapshot PRIVATE "${LYCAD_SRC_ROOT}")
    target_link_libraries(lycad_ui_snapshot
        PRIVATE ${LYCAD_QT_LIBS} Qt6::Test LyCAD::Warnings)
    set_target_properties(lycad_ui_snapshot PROPERTIES AUTORCC ON)
endif()
