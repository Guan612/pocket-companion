# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.5.2")
project(xiaozhi)

# Nomi 板子：全工程按体积优化（-Os），LVGL 的软件渲染也跟着慢；翻页时整屏重画全靠 CPU，
# 单独给 LVGL 开 -O2（固件大约多 100 多 KB，这块板子的 app 分区够用）
if(CONFIG_BOARD_TYPE_SPOTPEAR_ESP32S3_MAX35)
    idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
    target_compile_options(${lvgl_lib} PRIVATE -O2)
endif()
