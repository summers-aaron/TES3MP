#!/bin/bash -ex

free -m

env GENERATOR='Unix Makefiles' CONFIGURATION=Release CI/build_googletest.sh
GOOGLETEST_DIR="$(pwd)/googletest/build"

mkdir build
cd build

# Set up compilers
if [ ! -z "${MATRIX_CC}" ]; then
    eval "${MATRIX_CC}"
fi

export RAKNET_ROOT=~/CrabNet

${ANALYZE} cmake .. \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_CXX_COMPILER="${CXX}" \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DBUILD_OPENCS=OFF \
    -DUSE_SYSTEM_TINYXML=1 \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DBINDIR=/usr/games \
    -DCMAKE_BUILD_TYPE="None" \
    -DBUILD_UNITTESTS=TRUE \
    -DUSE_SYSTEM_TINYXML=TRUE \
    -DCMAKE_INSTALL_PREFIX="/usr" \
    -DBINDIR="/usr/games" \
    -DCMAKE_BUILD_TYPE="DEBUG" \
    -DGTEST_ROOT="${GOOGLETEST_DIR}" \
    -DGMOCK_ROOT="${GOOGLETEST_DIR}" \
    -DRakNet_LIBRARY_RELEASE=~/CrabNet/lib/libRakNetLibStatic.a \
    -DRakNet_LIBRARY_DEBUG=~/CrabNet/lib/libRakNetLibStatic.a
