# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

# Custom partition table for secure boot status
idf_build_set_property(PARTITION_TABLE_CSV_PATH "main/partitions_singleapp.csv")

project(FloraLink)
