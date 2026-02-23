cmake_minimum_required(VERSION 3.23)
project(snip_lite LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(snip_lite WIN32
  src/main.cpp
  src/snip_lite.rc
)

target_compile_definitions(snip_lite PRIVATE UNICODE _UNICODE)

target_link_libraries(snip_lite PRIVATE
  user32
  gdi32
  shell32
  ole32
  dwmapi

)

if (MSVC)
  target_compile_options(snip_lite PRIVATE /W4 /permissive- /EHsc)
endif()
