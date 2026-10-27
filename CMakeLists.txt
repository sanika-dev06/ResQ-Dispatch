cmake_minimum_required(VERSION 3.21)
project(resq_backend LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/utf-8 /W3 /bigobj)
endif()

# Dependencies come from vcpkg (see vcpkg.json): Crow (HTTP) and SQLite3.
find_package(Crow CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG QUIET)
if(unofficial-sqlite3_FOUND)
  set(RESQ_SQLITE_TARGET unofficial::sqlite3::sqlite3)
else()
  find_package(SQLite3 REQUIRED)
  set(RESQ_SQLITE_TARGET SQLite::SQLite3)
endif()

add_executable(resq_server src/main.cpp)
target_link_libraries(resq_server PRIVATE Crow::Crow ${RESQ_SQLITE_TARGET})

if(WIN32)
  target_compile_definitions(resq_server PRIVATE _WIN32_WINNT=0x0A00 NOMINMAX)
  target_link_libraries(resq_server PRIVATE bcrypt ws2_32 mswsock)   # bcrypt = secure random numbers
endif()

# Optional unit/integration tests:  cmake -DRESQ_BUILD_TESTS=ON ...  then  ctest -C Release
option(RESQ_BUILD_TESTS "Build the test programs" OFF)
if(RESQ_BUILD_TESTS)
  enable_testing()
  add_executable(test_core tests/test_core.cpp)
  add_executable(test_service tests/test_service.cpp)
  target_link_libraries(test_service PRIVATE ${RESQ_SQLITE_TARGET})
  if(WIN32)
    target_link_libraries(test_core PRIVATE bcrypt)
    target_link_libraries(test_service PRIVATE bcrypt)
  endif()
  add_test(NAME core COMMAND test_core)
  add_test(NAME service COMMAND test_service WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
