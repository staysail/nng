#
# Copyright 2017 Garrett D'Amore <garrett@damore.org>
# Copyright 2017 Capitar IT Group BV <info@capitar.com>
#
# This software is supplied under the terms of the MIT License, a
# copy of which should be located in the distribution where this
# file was obtained (LICENSE.txt).  A copy of the license may also be
# found online at https://opensource.org/licenses/MIT.
#

# Search for ZeroTier - its likely that we could do this with a git
# submodule.
#
# We use the libzerotiercore.a library, which is unfortunately a C++ object
# even though it exposes only public C symbols.  It would be extremely
# helpful if libzerotiercore didn't make us carry the whole C++ runtime
# behind us.  The user must specify the location of the ZeroTier source
# tree (dev branch for now, and already compiled please) by setting the
# NNG_ZEROTIER_SOURCE macro.
if (NNG_ENABLE_ZEROTIER)
    find_library(NNG_LIBZTCORE zerotiercore PATHS ${NNG_ZEROTIER_SOURCE})
    set(CMAKE_REQUIRED_INCLUDES ${NNG_ZEROTIER_SOURCE}/include)
    set(NNG_REQUIRED_LIBRARIES ${NNG_REQUIRED_LIBRARIES} ${NNG_LIBZTCORE} c++)
    set(NNG_REQUIRED_INCLUDES ${NNG_REQUIRED_INCLUDES} ${NNG_ZEROTIER_SOURCE}/include)
    nng_check_sym(ZT_Node_join ZeroTierOne.h NNG_HAVE_ZEROTIER)
    if ((NOT NNG_HAVE_ZEROTIER))
        message (FATAL_ERROR "Cannot find ZeroTier components")
    endif()
    message(STATUS "Found ZeroTier at ${NNG_LIBZTCORE}")
endif()

set(LIBZT "/Users/garrett/Projects/ZeroTierSDK/build/darwin/libzt.a")
set(LIBZT_INC "/Users/garrett/Projects/ZeroTierSDK/include")

set(NNZTAGENT_SRCS
        nnztagent_getopt.hpp
        nnztagent_main.cpp
)

# NB: I'm using my personal github repo because the upstream is busted at
# the moment.
include(ExternalProject)
ExternalProject_Add(libzt
    SOURCE_DIR libzt
    CONFIGURE_COMMAND echo "Nothing to see here."
    GIT_REPOSITORY https://github.com/gdamore/libzt.git
    BUILD_IN_SOURCE 1
    BUILD_COMMAND make STACK_PICO=1 static_lib
    INSTALL_COMMAND cp build/${CMAKE_SYSTEM_NAME}/libzt.a ${CMAKE_CURRENT_BINARY_DIR}
)

if (CMAKE_VERSION VERSION_LESS "3.1") 
    # MSVC is basically C++ by default.  For all others we just try with
    # the -std flag.  This may not work everywhere.  If this is a problem,
    # upgrade to a newer version of cmake that understands your platform
    # and compiler.
    if (not CMAKE_CXX_COMPILER_ID strequal "MSVC")
        set(CMAKE_CXX_FLAGS "-std=c++11 ${CMAKE_CXX_FLAGS}")
    endif()
else()
    set(CMAKE_CXX_STANDARD 11)
    set(CMAKE_CXX_STANDARD_REQUIRED on)
    set(CMAKE_CXX_EXTENSIONS off)
endif()

add_executable(nnztagent ${NNZTAGENT_SRCS})