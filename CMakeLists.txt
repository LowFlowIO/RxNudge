cmake_minimum_required(VERSION 3.16.0)
# Single source of the firmware version: the VERSION file. It sets PROJECT_VER here
# (embedded in the app descriptor that the OTA check reads), and scripts/pio_version.py
# passes the same value to the code as MT_FW_VERSION.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION" MT_VERSION LIMIT_COUNT 1)
set(PROJECT_VER "${MT_VERSION}")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rxnudge)
