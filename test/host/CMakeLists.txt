cmake_minimum_required(VERSION 3.16)
project(core_tests C)

set(CORE ${CMAKE_CURRENT_LIST_DIR}/../../components/core)
add_executable(core_tests test_main.c
    ${CORE}/pipeline.c ${CORE}/derived.c ${CORE}/record.c ${CORE}/histlog.c ${CORE}/history.c ${CORE}/alarm.c ${CORE}/csvlog.c ${CORE}/demo.c ${CORE}/sensor_codec.c)
target_include_directories(core_tests PRIVATE ${CORE}/include)
target_compile_options(core_tests PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined -g)
target_link_options(core_tests PRIVATE -fsanitize=address,undefined)
target_link_libraries(core_tests m)
