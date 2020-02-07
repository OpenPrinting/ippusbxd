cmake_minimum_required(VERSION 2.6)
project(ippusbxd)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -o2 -g -std=c99 -Wall -Wextra -pedantic -pedantic-errors")


# Compiler specific configuration
if (${CMAKE_C_COMPILER_ID} STREQUAL "GNU") 
    # Nothing
elseif( ${CMAKE_C_COMPILER_ID} STREQUAL "Clang") 
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Weverything -Wno-documentation -Wno-padded")
    # Compiler cannot detect that our logging functions really do use string
    # literals. So be careful to never-ever call printf with a user provided
    # format string since you will not get a warning. Coverity is smart enough
    # on this matter.
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-format-nonliteral")
endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR})

# Pthreads
find_package(Threads REQUIRED)

# Avahi
find_package(AVAHICOMMON REQUIRED)
find_package(AVAHICLIENT REQUIRED)

# Libusb
find_package(LIBUSB REQUIRED)
include_directories(${LIBUSB_INCLUDE_DIR})

# Libxml-2.0
find_package(LibXml2 REQUIRED)
set(LIBXML2_LIBRARIES ${LIBXML2_LIBRARY})
include_directories(${LIBXML2_INCLUDE_DIR})
unset(LIBXML2_LIBRARY)

find_package(LibXml2 REQUIRED)


# Libcurl
find_package(CURL REQUIRED)

add_executable(ippusbxd
ippusbxd.c
http.c
tcp.c
usb.c
logging.c
options.c
dnssd.c
)
target_link_libraries(ippusbxd ${CMAKE_THREAD_LIBS_INIT})
target_link_libraries(ippusbxd ${LIBUSB_LIBRARIES})
target_link_libraries(ippusbxd ${AVAHICOMMON_LIBRARIES})
target_link_libraries(ippusbxd ${AVAHICLIENT_LIBRARIES})
target_link_libraries(ippusbxd ${LIBXML2_LIBRARIES})
target_link_libraries(ippusbxd ${CURL_LIBRARIES})

add_executable(esclusbxd
esclusbxd.c
capabilities.c
logging.c
options.c
)
target_link_libraries(esclusbxd ${CMAKE_THREAD_LIBS_INIT})
target_link_libraries(esclusbxd ${AVAHICOMMON_LIBRARIES})
target_link_libraries(esclusbxd ${AVAHICLIENT_LIBRARIES})
target_link_libraries(esclusbxd ${LIBXML2_LIBRARIES})
target_link_libraries(esclusbxd ${CURL_LIBRARIES})
