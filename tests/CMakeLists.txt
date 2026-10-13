# 单元测试：按文件名约定收集，一个 test_*.cpp 一个目标，无需改本文件。
# 只编入到 interact 为止的源码（不含 ui），全部以离屏模式运行（QT_QPA_PLATFORM=offscreen）。
find_package(Qt6 REQUIRED COMPONENTS Test)

# 测试二进制归到 build/tests/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

function(lycad_add_test target source)
    add_executable(${target} EXCLUDE_FROM_ALL
        ${source} ${LYCAD_HEADLESS_SOURCES})
    # 函数内 set 是局部作用域，函数返回即丢；用全局属性收集目标清单
    set_property(GLOBAL APPEND PROPERTY LYCAD_TEST_TARGETS ${target})
    target_include_directories(${target} PRIVATE "${LYCAD_SRC_ROOT}")
    target_include_directories(${target} SYSTEM PRIVATE ${LYCAD_DXFRW_INCLUDES})
    # 源码根目录：个别用例要读 sample/ 下的样例文件（找不到时用例自行跳过）
    target_compile_definitions(${target} PRIVATE
        LYCAD_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(${target}
        PRIVATE lycad_dxfrw Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test LyCAD::Warnings)
    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

file(GLOB LYCAD_TEST_SOURCES CONFIGURE_DEPENDS "test_*.cpp")

foreach(source IN LISTS LYCAD_TEST_SOURCES)
    get_filename_component(name "${source}" NAME_WE)
    lycad_add_test(${name} ${source})
endforeach()

# 聚合目标：ctest 不会自己编译缺失的二进制，跑测试前先构建它
get_property(LYCAD_TEST_TARGETS GLOBAL PROPERTY LYCAD_TEST_TARGETS)
add_custom_target(lycad_tests
    COMMENT "构建全部单元测试"
    DEPENDS ${LYCAD_TEST_TARGETS})
