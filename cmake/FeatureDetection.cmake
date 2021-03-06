include(CheckCXXSourceCompiles)

set (TMP_FLAGS ${CMAKE_REQUIRED_FLAGS})
if (UNIX)
    set (CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -std=c++11")
endif()

CHECK_CXX_SOURCE_COMPILES("int main() { thread_local int a = 0; return a; }" HAS_THREAD_LOCAL)
if (${HAS_THREAD_LOCAL})
    add_definitions(-DHAS_THREAD_LOCAL)
endif()

set(CMAKE_REQUIRED_FLAGS ${TMP_FLAGS})
