cmake_minimum_required(VERSION 3.16)
project(esp32-git-tests C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

include(CTest)

add_library(esp32git STATIC
  src/io.cpp
  src/object.cpp
  src/repo.cpp
  src/history.cpp
  src/sync_file.cpp
  src/pkt.cpp
  src/pack.cpp
  src/http.cpp
  src/sha1.c
  src/zlib.c
  third_party/uzlib/tinflate.c
)
target_include_directories(esp32git PUBLIC include src third_party/uzlib)

find_package(CURL QUIET)

foreach(t objects commit sync fs_port)
  add_executable(test_${t} test/test_${t}.c)
  target_link_libraries(test_${t} PRIVATE esp32git)
  target_include_directories(test_${t} PRIVATE src third_party/uzlib)
  add_test(NAME ${t} COMMAND test_${t} WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endforeach()

if(CURL_FOUND)
  add_executable(test_http test/test_http.cc)
  target_link_libraries(test_http PRIVATE esp32git CURL::libcurl)
  target_include_directories(test_http PRIVATE src third_party/uzlib)
  add_test(NAME http COMMAND test_http WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
