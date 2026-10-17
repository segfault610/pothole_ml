cmake_minimum_required(VERSION 3.16)

# Shared drivers, detector and generated model live in ../components
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(pothole_collector)
