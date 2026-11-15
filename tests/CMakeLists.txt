# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2025 Arjen Hiemstra <ahiemstra@heimr.nl>

add_executable(parser_test parser_test.cpp)

target_compile_definitions(parser_test PRIVATE TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

target_link_libraries(parser_test PRIVATE cxx-rust-cssparser)

add_test(NAME parser_test COMMAND parser_test)
