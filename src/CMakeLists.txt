# =============================================================================
#  LyCAD - src 汇总
#
#  依赖方向（严格单向，无循环）：
#
#      core  ──>  io  ──>  render  ──>  interact  ──>  ui
#       ^                  |
#       └──────────────────┴───────────── 所有模块均可直接依赖 core
# =============================================================================

set(LYCAD_SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

add_subdirectory(core)
add_subdirectory(io)
add_subdirectory(render)
add_subdirectory(interact)
add_subdirectory(ui)

# ---------------------------------------------------------------- 可执行程序
qt_add_executable(lycad
    main.cpp
)

target_link_libraries(lycad PRIVATE LyCAD::Ui)

set_target_properties(lycad PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE    ON
    OUTPUT_NAME      lycad
)

# ---------------------------------------------------------------- 安装规则
include(GNUInstallDirs)
install(TARGETS lycad
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    BUNDLE  DESTINATION "."
)
