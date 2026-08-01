cmake_minimum_required(VERSION 3.16)

# Build only the native NebilixOS component and its ESP-IDF dependencies.
# This intentionally excludes any third-party components stored in components/.
set(COMPONENTS main nebilix_kernel nebilix_wifi nebilix_scripts nebilix_shell nebilix_remote)
set(PROJECT_VER "0.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nebilixos)
