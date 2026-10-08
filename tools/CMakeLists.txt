# 开发工具（不随软件发布，仅开发期验证）：
#   lycad_sample_generator 生成/读取参考 DXF/DWG；只编 core+io，兼作「下层不依赖上层」的链接期检查
#   lycad_icon_sheet      把全部图标渲染成总览图
#   lycad_ui_snapshot     离屏渲染主窗口为 PNG，驱动交互链路做界面验证
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")

add_executable(lycad_sample_generator EXCLUDE_FROM_ALL
    sample_generator.cpp
    ${LYCAD_CORE_IO_SOURCES}
    ${LYCAD_DXFRW_OBJECTS}
)
target_include_directories(lycad_sample_generator PRIVATE "${LYCAD_SRC_ROOT}")
target_include_directories(lycad_sample_generator SYSTEM PRIVATE ${LYCAD_DXFRW_INCLUDES})
target_link_libraries(lycad_sample_generator PRIVATE Qt6::Core Qt6::Gui LyCAD::Warnings)
# 样例默认写入仓库的 sample/ 目录：由构建系统注入绝对路径，
# 生成器在任何工作目录下运行都能落到同一处（可用命令行参数覆盖）。
target_compile_definitions(lycad_sample_generator PRIVATE
    LYCAD_SAMPLE_DIR="${CMAKE_SOURCE_DIR}/sample")

add_executable(lycad_icon_sheet EXCLUDE_FROM_ALL
    icon_sheet.cpp
    ${LYCAD_SOURCES}
    ${LYCAD_RESOURCES}
    ${LYCAD_DXFRW_OBJECTS}
)
target_include_directories(lycad_icon_sheet PRIVATE "${LYCAD_SRC_ROOT}")
target_include_directories(lycad_icon_sheet SYSTEM PRIVATE ${LYCAD_DXFRW_INCLUDES})
target_link_libraries(lycad_icon_sheet PRIVATE ${LYCAD_QT_LIBS} LyCAD::Warnings)
set_target_properties(lycad_icon_sheet PROPERTIES AUTORCC ON)

# 界面快照需要 QTest 注入鼠标事件
find_package(Qt6 QUIET COMPONENTS Test)

if(TARGET Qt6::Test)
    add_executable(lycad_ui_snapshot EXCLUDE_FROM_ALL
        ui_snapshot.cpp
        ${LYCAD_SOURCES}
        ${LYCAD_RESOURCES}
        ${LYCAD_DXFRW_OBJECTS}
    )
    target_include_directories(lycad_ui_snapshot PRIVATE "${LYCAD_SRC_ROOT}")
    target_include_directories(lycad_ui_snapshot SYSTEM PRIVATE ${LYCAD_DXFRW_INCLUDES})
    target_link_libraries(lycad_ui_snapshot
        PRIVATE ${LYCAD_QT_LIBS} Qt6::Test LyCAD::Warnings)
    set_target_properties(lycad_ui_snapshot PROPERTIES AUTORCC ON)
endif()

add_custom_target(lycad_tools
    COMMENT "构建全部开发工具"
    DEPENDS lycad_sample_generator lycad_icon_sheet
            $<$<TARGET_EXISTS:lycad_ui_snapshot>:lycad_ui_snapshot>)
