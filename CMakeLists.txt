cmake_minimum_required(VERSION 3.13)

project(redis-starter-cpp)

file(GLOB_RECURSE SOURCE_FILES src/*.cpp)

set(CMAKE_CXX_STANDARD 23) # Enable the C++23 standard
set(THREADS_PREFER_PTHREAD_FLAG ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

if(APPLE)
  find_path(ASIO_INCLUDE_DIR asio.hpp PATHS "/opt/homebrew/Cellar")

  add_library(asio INTERFACE)
  target_include_directories(asio INTERFACE ${ASIO_INCLUDE_DIR})
  target_compile_definitions(asio INTERFACE ASIO_STANDALONE)

  add_library(asio::asio ALIAS asio)
else()
  find_package(asio CONFIG REQUIRED)
endif()

add_executable(redis ${SOURCE_FILES})

target_link_libraries(redis PRIVATE asio asio::asio)
target_link_libraries(redis PRIVATE Threads::Threads)
