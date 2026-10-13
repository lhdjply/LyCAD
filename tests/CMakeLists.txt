# 单元测试：按文件名约定收集，一个 test_*.cpp 一个目标，无需改本文件。
# 只编入到 interact 为止的源码（不含 ui），全部以离屏模式运行（QT_QPA_PLATFORM=offscreen）。
find_package(Qt6 REQUIRED COMPONENTS Test)

# 测试二进制归到 build/tests/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

function(lycad_add_test target source)
    # 源码走共享对象库，本目标只编自己的 test_*.cpp
    add_executable(${target} EXCLUDE_FROM_ALL
        ${source})
    # 函数内 set 是局部作用域，函数返回即丢；用全局属性收集目标清单
    set_property(GLOBAL APPEND PROPERTY LYCAD_TEST_TARGETS ${target})
    target_include_directories(${target} PRIVATE "${LYCAD_SRC_ROOT}")
    target_include_directories(${target} SYSTEM PRIVATE ${LYCAD_DXFRW_INCLUDES})
    # 源码根目录：个别用例要读 sample/ 下的样例文件（找不到时用例自行跳过）
    target_compile_definitions(${target} PRIVATE
        LYCAD_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    target_link_libraries(${target}
        PRIVATE lycad_headless lycad_dxfrw
                Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test LyCAD::Warnings)
    # -o 文件：每条消息都 fflush，ctest 超时强杀时文件里最后一行就是卡住的位置。
    # -o -,txt：同时保留 stdout，给 --output-on-failure 用。
    add_test(NAME ${target} COMMAND ${target}
        -o "${CMAKE_BINARY_DIR}/tests/${target}.qtest.txt,txt"
        -o -,txt)
    set_tests_properties(${target} PROPERTIES
        # QT_FORCE_STDERR_LOGGING：Windows 上 ctest 拉起的子进程没有控制台，Qt 会把
        # QtTest 的输出丢进调试器（OutputDebugString），日志里什么都看不到。Linux 无害。
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1"
        # 用例必须能挂死失败：Windows 上 test_block_insert 曾卡住 15 分钟以上，ctest
        # 默认不设超时会一路挂到 job 的 6 小时上限。正常一条只要几十毫秒。
        TIMEOUT 8)
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
