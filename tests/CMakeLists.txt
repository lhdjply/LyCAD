# =============================================================================
#  LyCAD - 单元测试
#
#  测试覆盖三个层面：
#    test_geometry      纯几何：变换、包围盒、距离、圆弧与凸度多段线
#    test_document      文档模型：增删改、撤销重做、图层联动、选择集
#    test_dxf_roundtrip 文件层：DXF R12 / R2000 / 二进制三种形态的往返一致性
#
#  全部测试以离屏模式运行（QT_QPA_PLATFORM=offscreen），可在无显示环境的
#  CI 容器中执行。涉及 QWidget 的测试不需要，因为画布逻辑与几何逻辑已分离。
# =============================================================================
find_package(Qt6 REQUIRED COMPONENTS Test)

function(lycad_add_test target)
    add_executable(${target} ${target}.cpp)
    target_include_directories(${target} PRIVATE "${LYCAD_SRC_ROOT}")
    target_link_libraries(${target} PRIVATE
        LyCAD::Core LyCAD::Io LyCAD::Render LyCAD::Interact
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

lycad_add_test(test_geometry)
lycad_add_test(test_document)
lycad_add_test(test_dxf_roundtrip)
lycad_add_test(test_dwg_backend)
