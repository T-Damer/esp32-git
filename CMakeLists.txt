cmake_minimum_required(VERSION 3.16)
project(esp32-git-tests C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

include(CTest)

add_library(esp32git STATIC
  src/object.cpp
  src/sha1.c
  src/zlib.c
  third_party/uzlib/tinflate.c
)
target_include_directories(esp32git PUBLIC include src third_party/uzlib)
# tinflate.c is compiled as C inside a C++ project.
set_source_files_properties(src/sha1.c src/zlib.c third_party/uzlib/tinflate.c
                            PROPERTIES LANGUAGE C)

add_executable(test_objects test/test_objects.c)
target_link_libraries(test_objects PRIVATE esp32git)
target_include_directories(test_objects PRIVATE src third_party/uzlib)

add_test(NAME objects COMMAND test_objects WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
