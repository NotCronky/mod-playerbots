# Included by the core's modules/CMakeLists.txt after the `modules` target exists.
#
# The core links the MySQL client library privately into its `database` target,
# so its include directories do not reach module code. PlayerbotsDatabase.cpp
# needs the complete MySQLPreparedStatement type (which pulls in mysql.h) and
# Playerbots.cpp uses ER_BAD_DB_ERROR from mysqld_error.h. Linking the imported
# `mysql` target here propagates those include directories to the module build.
target_link_libraries(modules
  PRIVATE
    mysql)

# The bot panel's protocol (src/Script/Panel/PanelProtocol.cpp) is std-only, so its tests build without the core.
# Defined at the end of the root directory's configuration, once the core has fetched googletest.
if (BUILD_TESTING)
    function(define_playerbots_panel_tests)
        set(PB_PATH "${CMAKE_SOURCE_DIR}/modules/mod-playerbots")
        add_executable(playerbots_panel_tests
            "${PB_PATH}/src/Script/Panel/PanelProtocol.cpp"
            "${PB_PATH}/t/TestPanelProtocol.cpp"
        )
        target_include_directories(playerbots_panel_tests PRIVATE "${PB_PATH}/src/Script/Panel")
        target_link_libraries(playerbots_panel_tests gtest_main)
        set_target_properties(playerbots_panel_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    endfunction()

    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL define_playerbots_panel_tests)
endif()
