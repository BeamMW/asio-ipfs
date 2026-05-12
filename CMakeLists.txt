if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Windows")
    cmake_minimum_required (VERSION 3.21)
else ()
    cmake_minimum_required (VERSION 3.13)
endif()

project(asio-ipfs)
include(ExternalProject)

#
# Boost
#
set(BOOST_VERSION 1.62)
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS")
    # iOS Boost is typically distributed as a fat framework or a header
    # tree + a single combined static library, not as the per-component
    # libs that `find_package(... COMPONENTS coroutine)` expects. The
    # asio-ipfs C++ wrapper only uses header-only parts (asio, intrusive,
    # optional) plus boost::system, so we point the build straight at
    # the headers via BOOST_ROOT / BOOST_ROOT_IOS (the convention Beam's
    # iOS wallet already uses) and skip find_package entirely.
    if (DEFINED ENV{BOOST_ROOT_IOS} AND NOT BOOST_ROOT)
        set(BOOST_ROOT "$ENV{BOOST_ROOT_IOS}")
    endif()
    if (BOOST_ROOT AND NOT Boost_INCLUDE_DIR)
        if (EXISTS "${BOOST_ROOT}/include")
            set(Boost_INCLUDE_DIR "${BOOST_ROOT}/include")
        elseif (EXISTS "${BOOST_ROOT}/boost.framework/Headers")
            set(Boost_INCLUDE_DIR "${BOOST_ROOT}/boost.framework/Headers")
        else()
            set(Boost_INCLUDE_DIR "${BOOST_ROOT}")
        endif()
    endif()
    if (NOT Boost_INCLUDE_DIR)
        message(FATAL_ERROR
            "iOS build: Boost headers not found. Pass BOOST_ROOT (or set "
            "the BOOST_ROOT_IOS env var) pointing at an iOS Boost tree.")
    endif()
else()
    find_package(Boost ${BOOST_VERSION} REQUIRED COMPONENTS
        coroutine
    )
endif()

#
# Detect platform & arch. For Linux / Windows / macOS host builds we do
# not cross-compile, so CMAKE_SYSTEM_NAME == CMAKE_HOST_SYSTEM_NAME and
# CMAKE_SYSTEM_PROCESSOR == CMAKE_HOST_SYSTEM_PROCESSOR. iOS is the one
# exception: CMAKE_SYSTEM_NAME == "iOS" while the host stays on Darwin.
#
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
    set(LINUX TRUE)

    if ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
        set(LINUX64 TRUE)
        set(X64 TRUE)
    else()
         message(FATAL_ERROR "Unsupported host architecture ${CMAKE_SYSTEM_PROCESSOR}")
    endif()

elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Windows")
    set(WINDOWS TRUE)

    if ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "AMD64")
        set(WIN64 TRUE)
        set(X64 TRUE)
    else()
         message(FATAL_ERROR "Unsupported host architecture ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Darwin")
    set(DARWIN TRUE)
    if ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
        set(DARWIN64 TRUE)
        set(X64 TRUE)
    elseif ("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "arm64")
        set(DARWINARM64 TRUE)
        set(X64 TRUE)
    else()
         message(FATAL_ERROR "Unsupported host architecture ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS")
    # Cross-compile from a macOS host to an iOS device or simulator.
    # CMAKE_OSX_SYSROOT (iphoneos / iphonesimulator) determines the slice;
    # CMAKE_OSX_ARCHITECTURES (arm64 or x86_64) picks the target arch.
    set(IOS TRUE)

    if (NOT "${CMAKE_HOST_SYSTEM_NAME}" STREQUAL "Darwin")
        message(FATAL_ERROR "iOS cross-compilation requires a macOS host (got ${CMAKE_HOST_SYSTEM_NAME})")
    endif()

    if ("${CMAKE_OSX_SYSROOT}" MATCHES "iPhoneSimulator|iphonesimulator")
        set(IOS_SIMULATOR TRUE)
    else()
        set(IOS_DEVICE TRUE)
    endif()

    list(LENGTH CMAKE_OSX_ARCHITECTURES _ios_arch_count)
    if (_ios_arch_count GREATER 1)
        message(FATAL_ERROR
            "asio-ipfs iOS builds must target a single architecture per CMake "
            "invocation (got: ${CMAKE_OSX_ARCHITECTURES}). Build each slice "
            "separately and combine with xcodebuild -create-xcframework "
            "(see scripts/build-ios.sh).")
    endif()

    if ("${CMAKE_OSX_ARCHITECTURES}" STREQUAL "arm64")
        set(IOS_ARCH "arm64")
    elseif ("${CMAKE_OSX_ARCHITECTURES}" STREQUAL "x86_64")
        set(IOS_ARCH "x86_64")
    else()
        message(FATAL_ERROR "Unsupported iOS architecture '${CMAKE_OSX_ARCHITECTURES}' (expected arm64 or x86_64)")
    endif()
else()
    message(FATAL_ERROR "Unsupported host platform ${CMAKE_SYSTEM_NAME}")
endif()

#
# Download and install golang
#
set(GOROOT "${CMAKE_CURRENT_BINARY_DIR}/golang")
set(GOPATH "${CMAKE_CURRENT_BINARY_DIR}/go-workspace")

if (LINUX64)
    set(GOSRC       "https://dl.google.com/go/go1.16.10.linux-amd64.tar.gz")
    set(GOSRC_HASH  "414cd18ce1d193769b9e97d2401ad718755ab47816e13b2a1cde203d263b55cf")
    set(GOOS        "linux")
    set(GOARCH      "amd64")
endif()

if (DARWIN64)
    set(GOSRC       "https://dl.google.com/go/go1.16.10.darwin-amd64.tar.gz")
    set(GOSRC_HASH  "895a3fe6d720297ce16272f41c198648da8675bb244ab6d60003265c176b6c48")
    set(GOOS        "darwin")
    set(GOARCH      "amd64")
endif()

if (DARWINARM64)
    set(GOSRC       "https://dl.google.com/go/go1.16.10.darwin-arm64.tar.gz")
    set(GOSRC_HASH  "850970c6b381b9a3e6da969bf1baddb8fe003ed90315082e5cb3afbbc87812d0")
    set(GOOS        "darwin")
    set(GOARCH      "arm64")
endif()

if (WIN64)
    set(GOSRC       "https://dl.google.com/go/go1.16.10.windows-amd64.zip")
    set(GOSRC_HASH  "787c9afbcd7446874f79bffd20dc4219c3aeff6731bd03f09058bdd546c7eb87")
    set(GOOS        "windows")
    set(GOARCH      "amd64")
endif()

if (IOS)
    # iOS builds are cross-compiled from a macOS host. Pick the Go binary
    # matching the host arch, then set GOOS=ios / GOARCH for the target.
    # Go 1.16 was the first release with stable GOOS=ios; older toolchains
    # used GOOS=darwin with build tags. We download the host-matching
    # Darwin tarball — Go's cross-compile is driven by env vars at build
    # time, not by the toolchain download.
    if ("${CMAKE_HOST_SYSTEM_PROCESSOR}" STREQUAL "arm64")
        set(GOSRC       "https://dl.google.com/go/go1.16.10.darwin-arm64.tar.gz")
        set(GOSRC_HASH  "850970c6b381b9a3e6da969bf1baddb8fe003ed90315082e5cb3afbbc87812d0")
    else()
        set(GOSRC       "https://dl.google.com/go/go1.16.10.darwin-amd64.tar.gz")
        set(GOSRC_HASH  "895a3fe6d720297ce16272f41c198648da8675bb244ab6d60003265c176b6c48")
    endif()
    set(GOOS    "ios")
    if ("${IOS_ARCH}" STREQUAL "arm64")
        set(GOARCH "arm64")
    else()
        set(GOARCH "amd64")
    endif()
endif()

externalproject_add(golang
    URL ${GOSRC}
    URL_HASH SHA256=${GOSRC_HASH}
    CONFIGURE_COMMAND ""
    BUILD_COMMAND     ""
    INSTALL_COMMAND   ""
    SOURCE_DIR        ${GOROOT}
)

#
# Install tdm-gcc if on windows. Necessary for CGO compilation step.
# Visual studio compilers are not supported by Golang/CGO at the moment.
#
if (WINDOWS)
    set(TDMROOT "${CMAKE_CURRENT_BINARY_DIR}/tdmgcc")
    make_directory(${TDMROOT})

    externalproject_add(tdmgcca
        URL https://github.com/jmeubank/tdm-gcc-src/releases/download/v10.3.0-tdm64-1/gcc-10.3.0-tdm64-1-core.zip
        URL_HASH SHA256=6514c8c2e562d37f3174bad65f0e2498246c60b85a166bf36b124da55a1709fc
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_BINARY_DIR}/tdmgcca-src ${TDMROOT}
        SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/tdmgcca-src
    )

    externalproject_add(tdmgccb
        URL https://github.com/jmeubank/tdm-binutils-gdb/releases/download/v2.36.1-tdm64-1/binutils-2.36.1-tdm64-1.zip
        URL_HASH SHA256=e3e6ca2fd86874c25b5e69e85862b35489c0f381d14bec450274c902642ff22e
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_BINARY_DIR}/tdmgccb-src ${TDMROOT}
        SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/tdmgccb-src
        DEPENDS tdmgcca
    )

    externalproject_add(tdmgccc
        URL https://github.com/jmeubank/mingw-w64/releases/download/v8-git2021050601-gcc10-tdm64-1/mingw64runtime-v8-git2021050601-gcc10-tdm64-1.zip
        URL_HASH SHA256=2f9c7c79f51d968db8659b19dff6778fa504cb482315a2e950922ccb29da6e85
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_BINARY_DIR}/tdmgccc-src ${TDMROOT}
        SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/tdmgccc-src
        DEPENDS tdmgccb
    )

    externalproject_add(tdmgccd
        URL https://github.com/jmeubank/tdm-gcc-src/releases/download/v10.3.0-tdm64-1/gcc-10.3.0-tdm64-1-c++.zip
        URL_HASH SHA256=36d48e2a7697b90bf1375fd16b45f3a802fadab5c677b5dd63357fb1ee25187a
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_BINARY_DIR}/tdmgccd-src ${TDMROOT}
        SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/tdmgccd-src
        DEPENDS tdmgccc
    )

    externalproject_add(tdmgcce
        URL https://github.com/jmeubank/windows-default-manifest/releases/download/v6.4-x86_64_multi/windows-default-manifest-6.4-x86_64_multi.zip
        URL_HASH SHA256=d34b4d149242500cba357ec80fc729dd30f40e120ee022aee7d7af4e9da3a89f
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_BINARY_DIR}/tdmgcce-src ${TDMROOT}
        SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/tdmgcce-src
        DEPENDS tdmgccd
    )   
endif()

#
# Build libipfs-bindings. This is a wrapper around go-ipfs
# that produces a shared library [.dll + .lib + .h] on Windows
# and a static library [.a + .h] on other operating systems.
#
# Since CGO uses GCC and Beam itself uses MSVC the only way
# on Windows is to use a DLL. On other operating systems the
# story is much easier and we can use usual lib/.a and not
# overcomplicate our build & install process
#
set(BINDINGS_DIR     "${CMAKE_CURRENT_BINARY_DIR}/ipfs_bindings")
set(GOPATH_IPFS_DIR  "${GOPATH}/src/github.com/equalitie/asio-ipfs")

set(BINDINGS_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ipfs_bindings/ipfs_bindings.go
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ipfs_bindings/ipfs_node.go
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ipfs_bindings/ipfs_config.go
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ipfs_bindings/ipfs_cancel.go
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ipfs_bindings/ipfs_repo.go
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ipfs_bindings/ipfs_gc.go
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ipfs_bindings/ipfs_api.go
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ipfs_bindings/ipfs_mfs.go
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ipfs_bindings/ipfs_log.go
)

if (LINUX)
    set(BINDINGS_HEADER  "${BINDINGS_DIR}/libipfs-bindings.h")
    set(BINDINGS_LIBRARY "${BINDINGS_DIR}/libipfs-bindings.a")
    set(BINDINGS_OUTPUT  ${BINDINGS_HEADER} ${BINDINGS_LIBRARY})

    add_custom_command(
        OUTPUT  ${BINDINGS_OUTPUT}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/include/ipfs_error_codes.h
                ${BINDINGS_SRC}
                golang

        COMMAND ${CMAKE_COMMAND} -E make_directory ${BINDINGS_DIR}
            && ${CMAKE_COMMAND} -E make_directory ${GOPATH_IPFS_DIR}
            && ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/include ${GOPATH_IPFS_DIR}/include
            && ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/src ${GOPATH_IPFS_DIR}/src
            && ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/go.mod ${GOPATH_IPFS_DIR}
            && ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/go.sum ${GOPATH_IPFS_DIR}

        COMMAND cd ${GOPATH_IPFS_DIR}
             && export GOROOT=${GOROOT}
             && export GOPATH=${GOPATH}
             && export GOOS=${GOOS}
             && export GOARCH=${GOARCH}
             && export GOARM=${GOARM}
             && export CGO_ENABLED=1
             && export CC=${CMAKE_C_COMPILER}
             && export GO111MODULE=on
             && export GOGCCFLAGS="-m64 -mthreads -fno-caret-diagnostics -Qunused-arguments -fmessage-length=0 -gno-record-gcc-switches"
             && ${GOROOT}/bin/go build
                    --buildmode=c-archive
                    -ldflags "-s -w"
                    -modcacherw
                    -o ${BINDINGS_LIBRARY}
                    ./src/ipfs_bindings
    )

elseif (DARWIN)
    set(BINDINGS_HEADER  "${BINDINGS_DIR}/libipfs-bindings.h")
    set(BINDINGS_LIBRARY "${BINDINGS_DIR}/libipfs-bindings.a")
    set(BINDINGS_OUTPUT  ${BINDINGS_HEADER} ${BINDINGS_LIBRARY})

    add_custom_command(
        OUTPUT  ${BINDINGS_OUTPUT}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/include/ipfs_error_codes.h
                ${BINDINGS_SRC}
                golang

        COMMAND ${CMAKE_COMMAND} -E make_directory ${BINDINGS_DIR}
            && ${CMAKE_COMMAND} -E make_directory ${GOPATH_IPFS_DIR}
            && ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/include ${GOPATH_IPFS_DIR}/include
            && ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/src ${GOPATH_IPFS_DIR}/src
            && ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/go.mod ${GOPATH_IPFS_DIR}
            && ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/go.sum ${GOPATH_IPFS_DIR}

        COMMAND cd ${GOPATH_IPFS_DIR}
             && export GOROOT=${GOROOT}
             && export GOPATH=${GOPATH}
             && export GOOS=${GOOS}
             && export GOARCH=${GOARCH}
             && export GOARM=${GOARM}
             && export CGO_ENABLED=1
             && export CC=clang
             && export GO111MODULE=on
             && export GOGCCFLAGS="-m64 -fno-caret-diagnostics -Qunused-arguments -fmessage-length=0 -gno-record-gcc-switches"
             && ${GOROOT}/bin/go build
                    --buildmode=c-archive
                    -ldflags "-s -w"
                    -modcacherw
                    -o ${BINDINGS_LIBRARY}
                    ./src/ipfs_bindings
    )
elseif (IOS)
    set(BINDINGS_HEADER  "${BINDINGS_DIR}/libipfs-bindings.h")
    set(BINDINGS_LIBRARY "${BINDINGS_DIR}/libipfs-bindings.a")
    set(BINDINGS_OUTPUT  ${BINDINGS_HEADER} ${BINDINGS_LIBRARY})

    # Pick the right Xcode SDK + clang `-target` triple for the slice we're
    # building. Go's cgo treats CC as a single command string, so we pack
    # the host clang path plus -isysroot / -target into one CC value and
    # resolve it at configure time (xcrun is always available alongside
    # Xcode, which is a hard prerequisite for any iOS build).
    if (IOS_SIMULATOR)
        set(IOS_SDK_NAME "iphonesimulator")
        set(IOS_TARGET_SUFFIX "-simulator")
    else()
        set(IOS_SDK_NAME "iphoneos")
        set(IOS_TARGET_SUFFIX "")
    endif()

    if (NOT IOS_DEPLOYMENT_TARGET)
        if (CMAKE_OSX_DEPLOYMENT_TARGET)
            set(IOS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}")
        else()
            set(IOS_DEPLOYMENT_TARGET "13.0")
        endif()
    endif()

    set(IOS_CLANG_TARGET
        "${IOS_ARCH}-apple-ios${IOS_DEPLOYMENT_TARGET}${IOS_TARGET_SUFFIX}")

    execute_process(
        COMMAND xcrun --sdk ${IOS_SDK_NAME} --show-sdk-path
        OUTPUT_VARIABLE IOS_SDK_PATH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _xcrun_sdk_rc
    )
    execute_process(
        COMMAND xcrun --sdk ${IOS_SDK_NAME} --find clang
        OUTPUT_VARIABLE IOS_CLANG_BIN
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _xcrun_clang_rc
    )
    if (NOT _xcrun_sdk_rc EQUAL 0 OR NOT _xcrun_clang_rc EQUAL 0
            OR NOT IOS_SDK_PATH OR NOT IOS_CLANG_BIN)
        message(FATAL_ERROR
            "Could not locate Xcode SDK ${IOS_SDK_NAME} via xcrun. "
            "Install Xcode and run xcode-select --install before configuring.")
    endif()

    set(IOS_CC "${IOS_CLANG_BIN} -target ${IOS_CLANG_TARGET} -isysroot ${IOS_SDK_PATH}")

    add_custom_command(
        OUTPUT  ${BINDINGS_OUTPUT}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/include/ipfs_error_codes.h
                ${BINDINGS_SRC}
                golang

        COMMAND ${CMAKE_COMMAND} -E make_directory ${BINDINGS_DIR}
            && ${CMAKE_COMMAND} -E make_directory ${GOPATH_IPFS_DIR}
            && ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/include ${GOPATH_IPFS_DIR}/include
            && ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/src ${GOPATH_IPFS_DIR}/src
            && ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/go.mod ${GOPATH_IPFS_DIR}
            && ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/go.sum ${GOPATH_IPFS_DIR}

        COMMAND ${CMAKE_COMMAND} -E chdir ${GOPATH_IPFS_DIR}
                ${CMAKE_COMMAND} -E env
                    "GOROOT=${GOROOT}"
                    "GOPATH=${GOPATH}"
                    "GOOS=${GOOS}"
                    "GOARCH=${GOARCH}"
                    "CGO_ENABLED=1"
                    "GO111MODULE=on"
                    "CC=${IOS_CC}"
                    "CXX=${IOS_CC}"
                    ${GOROOT}/bin/go build
                        --buildmode=c-archive
                        -ldflags "-s -w"
                        -modcacherw
                        -o ${BINDINGS_LIBRARY}
                        ./src/ipfs_bindings
    )
elseif(WIN64)
    set(BINDINGS_HEADER  "${BINDINGS_DIR}/ipfs-bindings.h")
    set(BINDINGS_LIBRARY "${BINDINGS_DIR}/ipfs-bindings.lib")
    set(BINDINGS_DLL     "${BINDINGS_DIR}/ipfs-bindings.dll")
    set(BINDINGS_DEF     "${BINDINGS_DIR}/ipfs-bindings.def")
    set(BINDINGS_OUTPUT  ${BINDINGS_HEADER} ${BINDINGS_DLL} ${BINDINGS_LIBRARY})

    add_custom_command(
        OUTPUT ${BINDINGS_OUTPUT}       
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/include/ipfs_error_codes.h
                ${BINDINGS_SRC}
                golang tdmgcce

        COMMAND ${CMAKE_COMMAND} -E make_directory ${BINDINGS_DIR}        
             && ${CMAKE_COMMAND} -E make_directory ${GOPATH_IPFS_DIR}
             && ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/include ${GOPATH_IPFS_DIR}/include
             && ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/src ${GOPATH_IPFS_DIR}/src
             && ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/go.mod ${GOPATH_IPFS_DIR}
             && ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/go.sum ${GOPATH_IPFS_DIR}

        COMMAND cd ${GOPATH_IPFS_DIR}       
             && ${CMAKE_COMMAND} -E env 
                "GOROOT=${GOROOT}" 
                "GOPATH=${GOPATH}"
                "GOOS=${GOOS}"
                "GOARCH=${GOARCH}"
                "CGO_ENABLED=1"
                "CC=${TDMROOT}/bin/gcc.exe"
                "GO111MODULE=on"
                "GOTESTMSVC=1"
                "GOGCCFLAGS=-m64 -mthreads -fno-caret-diagnostics -Qunused-arguments -fmessage-length=0 -gno-record-gcc-switches"
                ${GOROOT}/bin/go build
                    --buildmode=c-shared
                    -ldflags "-s -w"
                    -o ${BINDINGS_DLL}
                    -modcacherw
                    ./src/ipfs_bindings
        COMMAND cd ${BINDINGS_DIR}
             && ${TDMROOT}/x86_64-w64-mingw32/bin/gendef.exe ipfs-bindings.dll
             && lib /def:${BINDINGS_DEF} /machine:x64 
    )
endif()

add_custom_target(ipfs-bindings-build ALL
    DEPENDS ${BINDINGS_OUTPUT}
)

if (BEAM_SIGN_PACKAGE AND WINDOWS)
    add_custom_command(
        TARGET ipfs-bindings-build POST_BUILD
        COMMAND ${SIGNTOOL_EXECUTABLE} sign ${SIGNTOOL_OPTIONS} ${BINDINGS_DLL}
    )
endif()

if (WINDOWS)
    add_library(ipfs-bindings SHARED IMPORTED GLOBAL
        DEPENDS ipfs-bindings-build 
    )

    set_target_properties(ipfs-bindings PROPERTIES
        IMPORTED_LOCATION ${BINDINGS_DLL}
        IMPORTED_IMPLIB ${BINDINGS_LIBRARY}
    )

    # IMPORTED_RUNTIME_ARTIFACTS available only in 3.21
    install(IMPORTED_RUNTIME_ARTIFACTS ipfs-bindings RUNTIME DESTINATION .)
    # install(FILES ${BINDINGS_DLL} DESTINATION BIN)
endif()

if (LINUX OR DARWIN OR IOS)
    add_library(ipfs-bindings STATIC IMPORTED GLOBAL
        DEPENDS ipfs-bindings-build
    )

    set_property(TARGET ipfs-bindings PROPERTY
        IMPORTED_LOCATION ${BINDINGS_LIBRARY}
    )
endif()

if (DARWIN OR IOS)
    # go-ipfs links Apple system crypto + CFNetwork via its keychain bridge;
    # bring those in for both macOS and iOS slices.
    target_link_libraries(ipfs-bindings
        INTERFACE
            "-framework CoreFoundation"
            "-framework Security")
endif()

#
# asio-ipfs itself
#
file(GLOB sources "./src/*.cpp")

add_library(asio-ipfs 
    STATIC ${sources}
)

set_target_properties(asio-ipfs
    PROPERTIES LINKER_LANGUAGE CXX
)

if (WINDOWS)
    set_target_properties(asio-ipfs
        PROPERTIES IPFS_DLL ${BINDINGS_DLL}
    )
endif()

add_dependencies(asio-ipfs 
    ipfs-bindings
    ipfs-bindings-build
)

target_include_directories(asio-ipfs
    PRIVATE ${BINDINGS_DIR}
    PRIVAte "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(asio-ipfs
    PUBLIC ipfs-bindings
)

if (IOS)
    # iOS callers supply Boost via BOOST_ROOT_IOS (headers only — Boost on
    # iOS is typically a single combined framework). Drop the COMPONENTS
    # link so the build doesn't require a separately-built boost_coroutine.
    if (Boost_INCLUDE_DIR)
        target_include_directories(asio-ipfs PUBLIC ${Boost_INCLUDE_DIR})
    endif()
else()
    target_link_libraries(asio-ipfs
        PUBLIC Boost::boost
        INTERFACE ${Boost_LIBRARIES}
    )
endif()
