# SPDX-License-Identifier: Apache-2.0
#
# hal_ag32/soc CMake glue — entry point for the top-level
# add_subdirectory(soc) in CMakeLists.txt.
#
# Single SoC today (agrv2k under vendor agm); add another
# add_subdirectory() here when a future AgRV variant joins.

add_subdirectory(agm)
