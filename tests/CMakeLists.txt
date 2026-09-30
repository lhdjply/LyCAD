# =============================================================================
#  LyCAD - 单元测试
#
#  测试按关注点分组：
#    test_geometry        纯几何：变换、包围盒、距离、圆弧/凸度多段线、文字/填充/标注
#    test_document        文档模型：增删改、撤销重做、图层联动、选择集
#    test_dxf_roundtrip   文件层：DXF R12 / R2000 / 二进制三种形态的往返一致性
#    test_dwg_backend     DWG 后端适配：版本识别、编解码器选择策略、外部转换器链路
#    test_libdxfrw        内置解析器适配层：图元映射与**单位换算**（走公开接口）
#    test_libdxfrw_codec  libdxfrw-modify 解码器的不变量「只取决于当前文件的版本」
#                         （白盒测试，守住修改版的关键行为，见该文件头部说明）
#
#  测试只编入到 interact 为止的源码，不含 ui——画布逻辑与几何逻辑已经分离，
#  不需要 QWidget 主窗口。全部以离屏模式运行（QT_QPA_PLATFORM=offscreen），
#  因此可在无显示环境的 CI 容器中执行。
# =============================================================================
find_package(Qt6 REQUIRED COMPONENTS Test)

function(lycad_add_test target source)
    add_executable(${target} ${source} ${LYCAD_HEADLESS_SOURCES} ${LYCAD_DXFRW_OBJECTS})
    target_include_directories(${target} PRIVATE "${LYCAD_SRC_ROOT}")
    # libdxfrw 的头文件以 SYSTEM 方式引入：它自带 #pragma warning 与隐式拷贝构造等告警，
    # 与本项目无关，用 -isystem 交给编译器忽略，免得刷屏埋掉真正的问题。
    target_include_directories(${target} SYSTEM PRIVATE ${LYCAD_DXFRW_INCLUDES})
    target_link_libraries(${target}
        PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test LyCAD::Warnings)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

# 测试按文件名约定收集：一个 test_*.cpp 就是一个测试目标，目标名取文件名。
# 新增测试只需按约定命名文件，不必改本文件。
file(GLOB LYCAD_TEST_SOURCES CONFIGURE_DEPENDS "test_*.cpp")

foreach(source IN LISTS LYCAD_TEST_SOURCES)
    get_filename_component(name "${source}" NAME_WE)
    lycad_add_test(${name} ${source})
endforeach()
