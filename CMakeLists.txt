cmake_minimum_required(VERSION 3.10.2)
project(server C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "-fPIC -no-pie -fno-PIE")

# thread network server
ADD_DEFINITIONS(-D_THREAD)

include_directories($ENV{HOME}/.local/include)
link_directories($ENV{HOME}/.local/lib)

add_executable(server thr_pool.c zf_log.c server.c)

target_link_libraries(server event event_pthreads pthread rt)