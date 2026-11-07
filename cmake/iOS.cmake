# SPDX-License-Identifier: GPL-3.0-or-later
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_SYSROOT iphoneos CACHE STRING "iOS device SDK")
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "iPhone architecture")
set(CMAKE_OSX_DEPLOYMENT_TARGET 17.4 CACHE STRING "Minimum iOS version")
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO CACHE STRING "Build unsigned unless a development team is supplied")
