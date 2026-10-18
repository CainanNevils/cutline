add_executable(mediabench mediabench/main.cpp mediabench/projects.cpp)
target_link_libraries(mediabench PRIVATE cutline_export cutline_engine cutline_audio cutline_media cutline_render Qt6::Gui psapi)
target_compile_definitions(mediabench PRIVATE CUTLINE_TESTMEDIA="${CMAKE_SOURCE_DIR}/testmedia")

add_executable(rendertest rendertest/main.cpp)
target_link_libraries(rendertest PRIVATE cutline_engine cutline_media cutline_render Qt6::Gui)
target_compile_definitions(rendertest PRIVATE CUTLINE_TESTMEDIA="${CMAKE_SOURCE_DIR}/testmedia"
    CUTLINE_GOLDEN="${CMAKE_SOURCE_DIR}/tests/golden")
add_test(NAME rendertest COMMAND rendertest)

add_executable(e2e e2e/main.cpp)
target_link_libraries(e2e PRIVATE cutline_export cutline_engine cutline_media Qt6::Gui)
target_compile_definitions(e2e PRIVATE CUTLINE_TESTMEDIA="${CMAKE_SOURCE_DIR}/testmedia"
    CUTLINE_GOLDEN="${CMAKE_SOURCE_DIR}/tests/golden")
add_test(NAME e2e COMMAND e2e)

add_executable(uibench uibench/main.cpp)
target_link_libraries(uibench PRIVATE cutline_ui)

add_executable(recoverytest recoverytest/main.cpp)
target_link_libraries(recoverytest PRIVATE cutline_ui)

add_executable(uishots uishots/main.cpp)
target_link_libraries(uishots PRIVATE cutline_ui)

add_executable(uitest uitest/main.cpp)
target_link_libraries(uitest PRIVATE cutline_ui)
target_compile_definitions(uitest PRIVATE CUTLINE_TESTMEDIA="${CMAKE_SOURCE_DIR}/testmedia")
add_test(NAME uitest_timeline COMMAND uitest --timeline)
add_test(NAME uitest_monkey COMMAND uitest --monkey 1500 1)
