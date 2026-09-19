cmake_minimum_required(VERSION 3.16)
# PROJECT_VER comes from version.txt (CI overwrites it with the release tag before building;
# local/dev builds keep whatever is committed there).
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(opensolecu)
