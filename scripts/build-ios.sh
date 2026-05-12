#!/usr/bin/env bash
#
# build-ios.sh — build asio-ipfs (libipfs-bindings.a + libasio-ipfs.a) for
# iOS, producing one slice per (sdk, arch) target plus an XCFramework that
# bundles them together.
#
# Usage:
#   BOOST_ROOT_IOS=/path/to/boost ./scripts/build-ios.sh [slice...]
#
# Slices (default: all three):
#   device-arm64        iPhone / iPad device
#   simulator-arm64     Apple Silicon iOS simulator
#   simulator-x86_64    Intel iOS simulator
#
# Environment:
#   BOOST_ROOT_IOS         Required. Headers (and optional combined lib) for
#                          Boost built against the iOS SDK. Used by the
#                          asio-ipfs C++ wrapper; the Go bindings don't
#                          touch Boost at all.
#   IOS_DEPLOYMENT_TARGET  Default 13.0.
#   BUILD_DIR              Default ./build/ios.
#   GENERATOR              CMake generator. Default "Unix Makefiles".
#   JOBS                   Parallel build jobs. Default $(sysctl -n hw.ncpu).
#
# Output layout (under $BUILD_DIR):
#   <slice>/ipfs_bindings/libipfs-bindings.a   (Go c-archive)
#   <slice>/libasio-ipfs.a                     (C++ wrapper)
#   ipfs-bindings.xcframework/                 (combined, ready to drop in Xcode)
#   asio-ipfs.xcframework/                     (combined, ready to drop in Xcode)
#
# Prereqs:
#   - macOS host with Xcode + Command Line Tools (xcrun, lipo, xcodebuild)
#   - CMake 3.13+
#   - Internet (the CMake config downloads Go 1.16.10 darwin tarball)

set -euo pipefail

if [[ "${OSTYPE:-}" != darwin* ]]; then
    echo "build-ios.sh: this script must run on macOS" >&2
    exit 1
fi

if [[ -z "${BOOST_ROOT_IOS:-}" ]]; then
    echo "build-ios.sh: BOOST_ROOT_IOS must be set (Boost iOS headers)" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build/ios}"
IOS_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-13.0}"
GENERATOR="${GENERATOR:-Unix Makefiles}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

ALL_SLICES=(device-arm64 simulator-arm64 simulator-x86_64)
if [[ $# -eq 0 ]]; then
    SLICES=("${ALL_SLICES[@]}")
else
    SLICES=("$@")
fi

configure_and_build() {
    local slice="$1"
    local sdk arch
    case "$slice" in
        device-arm64)      sdk="iphoneos";       arch="arm64"  ;;
        simulator-arm64)   sdk="iphonesimulator"; arch="arm64"  ;;
        simulator-x86_64)  sdk="iphonesimulator"; arch="x86_64" ;;
        *) echo "build-ios.sh: unknown slice '$slice'" >&2; exit 1 ;;
    esac

    local slice_build="${BUILD_DIR}/${slice}"
    echo "==> configuring ${slice} (sdk=${sdk}, arch=${arch})"
    mkdir -p "${slice_build}"

    # CMake's built-in iOS toolchain kicks in when CMAKE_SYSTEM_NAME=iOS.
    # No external toolchain file required.
    cmake -S "${REPO_ROOT}" -B "${slice_build}" -G "${GENERATOR}" \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_SYSROOT="${sdk}" \
        -DCMAKE_OSX_ARCHITECTURES="${arch}" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET}" \
        -DIOS_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET}" \
        -DBOOST_ROOT="${BOOST_ROOT_IOS}"

    echo "==> building ${slice}"
    cmake --build "${slice_build}" --parallel "${JOBS}"
}

for slice in "${SLICES[@]}"; do
    configure_and_build "${slice}"
done

# Build the XCFrameworks. xcodebuild -create-xcframework requires one
# -library per (sdk, arch_set); for simulator we lipo arm64+x86_64 first
# if both slices were built.
build_xcframework() {
    local lib_name="$1"   # e.g. libipfs-bindings.a
    local out_name="$2"   # e.g. ipfs-bindings
    local headers_dir="${3:-}"

    local args=()
    local tmp_dir
    tmp_dir="$(mktemp -d)"
    trap 'rm -rf "${tmp_dir}"' RETURN

    # Device slice.
    if [[ -e "${BUILD_DIR}/device-arm64/ipfs_bindings/${lib_name}" ]]; then
        args+=(-library "${BUILD_DIR}/device-arm64/ipfs_bindings/${lib_name}")
    elif [[ -e "${BUILD_DIR}/device-arm64/${lib_name}" ]]; then
        args+=(-library "${BUILD_DIR}/device-arm64/${lib_name}")
    fi
    if [[ -n "${headers_dir}" && -d "${BUILD_DIR}/device-arm64/${headers_dir}" && ${#args[@]} -gt 0 ]]; then
        args+=(-headers "${BUILD_DIR}/device-arm64/${headers_dir}")
    fi

    # Simulator slice — combine arm64 + x86_64 if both were built.
    local sim_libs=()
    for sim_arch in arm64 x86_64; do
        local p1="${BUILD_DIR}/simulator-${sim_arch}/ipfs_bindings/${lib_name}"
        local p2="${BUILD_DIR}/simulator-${sim_arch}/${lib_name}"
        if   [[ -e "${p1}" ]]; then sim_libs+=("${p1}")
        elif [[ -e "${p2}" ]]; then sim_libs+=("${p2}")
        fi
    done
    if [[ ${#sim_libs[@]} -eq 1 ]]; then
        args+=(-library "${sim_libs[0]}")
    elif [[ ${#sim_libs[@]} -ge 2 ]]; then
        local fat="${tmp_dir}/${lib_name}"
        lipo -create -output "${fat}" "${sim_libs[@]}"
        args+=(-library "${fat}")
    fi

    local out_path="${BUILD_DIR}/${out_name}.xcframework"
    rm -rf "${out_path}"
    if [[ ${#args[@]} -eq 0 ]]; then
        echo "build-ios.sh: nothing to bundle for ${out_name}" >&2
        return 0
    fi
    echo "==> creating ${out_path}"
    xcodebuild -create-xcframework "${args[@]}" -output "${out_path}"
}

build_xcframework "libipfs-bindings.a" "ipfs-bindings" "ipfs_bindings"
build_xcframework "libasio-ipfs.a"     "asio-ipfs"     ""

echo
echo "Done. Drop these into your Xcode project / iOS BEAM build:"
echo "  ${BUILD_DIR}/ipfs-bindings.xcframework"
echo "  ${BUILD_DIR}/asio-ipfs.xcframework"
