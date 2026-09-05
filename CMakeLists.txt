cmake_minimum_required(VERSION 3.10)

project(senseshock)
set(CMAKE_CXX_STANDARD 17)

set(SOURCES
    usbhid.cpp
    main.cpp
)

add_executable(senseshock ${SOURCES})

# Install rules for packaging
install(TARGETS senseshock
    RUNTIME DESTINATION bin)

# CPack RPM packaging configuration
set(CPACK_PACKAGE_NAME "senseshock")
set(CPACK_PACKAGE_VENDOR "senseshock")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "senseshock HID utility")
set(CPACK_PACKAGE_VERSION_MAJOR "1")
set(CPACK_PACKAGE_VERSION_MINOR "0")
set(CPACK_PACKAGE_VERSION_PATCH "0")
set(CPACK_PACKAGE_RELEASE "1")
set(CPACK_GENERATOR "RPM")
# Install prefix used by the package (combined with DESTINATION)
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

include(CPack)