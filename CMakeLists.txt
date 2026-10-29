cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# LVGL scroll animations (page swipe snap, scroll_to): 200-400 ms by default, which made the
# page switch after releasing feel slow. Halved; read by lvgl/src/core/lv_obj_scroll.c.
idf_build_set_property(COMPILE_DEFINITIONS "SCROLL_ANIM_TIME_MIN=100" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "SCROLL_ANIM_TIME_MAX=200" APPEND)

project(tab5_env_monitor)
