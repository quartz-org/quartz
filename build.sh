#!/bin/bash
# =============================================================================
# Quartz Build Script
# =============================================================================
# This script builds Quartz with configuration from build.conf
# Usage: ./build.sh [options]
#   --clean         Clean build directory before building
#   --rebuild-jit   Force regeneration of JIT stencils
#   --help          Show this help message

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG_FILE="${SCRIPT_DIR}/build.conf"

# =============================================================================
# Color Output
# =============================================================================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

info() { echo -e "${BLUE}[INFO]${NC} $1"; }
success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARNING]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# =============================================================================
# Default Configuration
# =============================================================================
BUILD_TYPE="Release"
JIT_ENABLED="true"
JIT_THRESHOLD="100"
JIT_DEBUG="false"
LTO_ENABLED="true"
PGO_ENABLED="false"
NATIVE_ARCH="false"
EXTENSIONS="system_io,system_collection,system_runtime"
VM_STACK_SIZE="65536"
VM_DEBUG_TRACE="false"
VM_INSTRUCTION_CACHE="true"
VM_COMPUTED_GOTOS="true"
HEAP_INITIAL_MB="64"
HEAP_MAX_MB="0"
PLATFORM=""
ARCH=""
BUILD_DIR="build"
INSTALL_PREFIX="/usr/local"

# =============================================================================
# Parse Configuration File
# =============================================================================
parse_config() {
    if [[ -f "$CONFIG_FILE" ]]; then
        info "Reading configuration from $CONFIG_FILE"
        while IFS='=' read -r key value; do
            # Skip comments and empty lines
            [[ "$key" =~ ^#.*$ ]] && continue
            [[ -z "$key" ]] && continue
            
            # Remove leading/trailing whitespace
            key=$(echo "$key" | xargs)
            value=$(echo "$value" | xargs)
            
            # Skip if no value
            [[ -z "$value" ]] && continue
            
            # Export configuration variables
            case "$key" in
                BUILD_TYPE) BUILD_TYPE="$value" ;;
                JIT_ENABLED) JIT_ENABLED="$value" ;;
                JIT_THRESHOLD) JIT_THRESHOLD="$value" ;;
                JIT_DEBUG) JIT_DEBUG="$value" ;;
                LTO_ENABLED) LTO_ENABLED="$value" ;;
                PGO_ENABLED) PGO_ENABLED="$value" ;;
                NATIVE_ARCH) NATIVE_ARCH="$value" ;;
                EXTENSIONS) EXTENSIONS="$value" ;;
                VM_STACK_SIZE) VM_STACK_SIZE="$value" ;;
                VM_DEBUG_TRACE) VM_DEBUG_TRACE="$value" ;;
                VM_INSTRUCTION_CACHE) VM_INSTRUCTION_CACHE="$value" ;;
                VM_COMPUTED_GOTOS) VM_COMPUTED_GOTOS="$value" ;;
                HEAP_INITIAL_MB) HEAP_INITIAL_MB="$value" ;;
                HEAP_MAX_MB) HEAP_MAX_MB="$value" ;;
                PLATFORM) PLATFORM="$value" ;;
                ARCH) ARCH="$value" ;;
                BUILD_DIR) BUILD_DIR="$value" ;;
                INSTALL_PREFIX) INSTALL_PREFIX="$value" ;;
            esac
        done < "$CONFIG_FILE"
    else
        warn "Configuration file not found at $CONFIG_FILE, using defaults"
    fi
}

# =============================================================================
# Detect Platform and Architecture
# =============================================================================
detect_platform() {
    if [[ -z "$PLATFORM" ]]; then
        case "$(uname -s)" in
            Linux*)  PLATFORM="linux" ;;
            Darwin*) PLATFORM="macos" ;;
            MINGW*|MSYS*|CYGWIN*) PLATFORM="windows" ;;
            *) error "Unsupported platform: $(uname -s)" ;;
        esac
    fi
    info "Platform: $PLATFORM"
}

detect_arch() {
    if [[ -z "$ARCH" ]]; then
        case "$(uname -m)" in
            x86_64|amd64) ARCH="x86_64" ;;
            aarch64|arm64) ARCH="aarch64" ;;
            *) error "Unsupported architecture: $(uname -m)" ;;
        esac
    fi
    info "Architecture: $ARCH"
}

# =============================================================================
# Generate JIT Stencils
# =============================================================================
generate_jit_stencils() {
    if [[ "$JIT_ENABLED" != "true" ]]; then
        info "JIT disabled, skipping stencil generation"
        return 0
    fi
    
    local STENCIL_DIR="${SCRIPT_DIR}/jit/stencils"
    local STENCIL_HEADER="${SCRIPT_DIR}/jit/stencils_${ARCH}.h"
    local STENCIL_OBJ="${STENCIL_DIR}/stencils_${ARCH}.o"
    
    # Select appropriate stencil source based on architecture
    local STENCIL_SRC
    if [[ "$ARCH" == "aarch64" ]]; then
        STENCIL_SRC="${STENCIL_DIR}/stencils_aarch64.c"
    else
        STENCIL_SRC="${STENCIL_DIR}/stencils.c"
    fi
    
    # Check if regeneration is needed
    if [[ -f "$STENCIL_HEADER" && -f "$STENCIL_SRC" ]]; then
        if [[ "$STENCIL_HEADER" -nt "$STENCIL_SRC" && "$FORCE_JIT_REBUILD" != "true" ]]; then
            info "JIT stencils are up to date"
            return 0
        fi
    fi
    
    info "Generating JIT stencils for $ARCH..."
    
    # Ensure directories exist
    mkdir -p "$STENCIL_DIR"
    
    if [[ ! -f "$STENCIL_SRC" ]]; then
        error "Stencil source not found: $STENCIL_SRC"
    fi
    
    # Determine compiler and flags based on architecture
    local CC="${CC:-gcc}"
    local CFLAGS="-O2 -fno-stack-protector -fno-pie -fno-asynchronous-unwind-tables"
    CFLAGS="$CFLAGS -fno-exceptions"
    
    if [[ "$ARCH" == "x86_64" ]]; then
        CFLAGS="$CFLAGS -mno-red-zone"
    elif [[ "$ARCH" == "aarch64" ]]; then
        # ARM64 specific flags
        CFLAGS="$CFLAGS -fomit-frame-pointer"
        # On macOS, use clang
        if [[ "$PLATFORM" == "macos" ]]; then
            CC="clang"
        fi
    fi
    
    # Compile stencils
    info "Compiling stencils with: $CC $CFLAGS"
    $CC $CFLAGS -c "$STENCIL_SRC" -o "$STENCIL_OBJ"
    
    # Build the stencil extractor if needed
    local EXTRACTOR="${SCRIPT_DIR}/jit/extract_stencils"
    local EXTRACTOR_SRC="${SCRIPT_DIR}/jit/extract_stencils.cpp"
    local CXX="${CXX:-g++}"
    
    # On macOS use clang++
    if [[ "$PLATFORM" == "macos" ]]; then
        CXX="clang++"
    fi
    
    if [[ ! -f "$EXTRACTOR" || "$EXTRACTOR_SRC" -nt "$EXTRACTOR" ]]; then
        info "Building stencil extractor..."
        $CXX -std=c++17 -O2 -o "$EXTRACTOR" "$EXTRACTOR_SRC"
    fi
    
    # Extract stencils using C++ extractor
    info "Extracting machine code from stencils..."
    "$EXTRACTOR" "$ARCH"
    
    success "JIT stencils generated: $STENCIL_HEADER"
}

# =============================================================================
# Generate Extensions Configuration
# =============================================================================
generate_extensions_config() {
    local CONFIG_JSON="${SCRIPT_DIR}/extensions/config.json"
    
    info "Generating extensions configuration..."
    
    # Convert comma-separated list to JSON array
    local EXT_ARRAY=""
    IFS=',' read -ra EXT_LIST <<< "$EXTENSIONS"
    for ext in "${EXT_LIST[@]}"; do
        ext=$(echo "$ext" | xargs)  # Trim whitespace
        if [[ -n "$EXT_ARRAY" ]]; then
            EXT_ARRAY="$EXT_ARRAY,"
        fi
        EXT_ARRAY="$EXT_ARRAY
        \"$ext\""
    done
    
    cat > "$CONFIG_JSON" << EOF
{
    "extensions": [$EXT_ARRAY
    ]
}
EOF
    
    success "Extensions config written to $CONFIG_JSON"
}

# =============================================================================
# Build with CMake
# =============================================================================
build_cmake() {
    local BUILD_PATH="${SCRIPT_DIR}/${BUILD_DIR}"
    
    info "Building Quartz (${BUILD_TYPE})..."
    
    # Create build directory
    mkdir -p "$BUILD_PATH"
    cd "$BUILD_PATH"
    
    # Prepare CMake flags
    local CMAKE_FLAGS="-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
    CMAKE_FLAGS="$CMAKE_FLAGS -DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX}"
    
    # JIT configuration
    if [[ "$JIT_ENABLED" == "true" ]]; then
        CMAKE_FLAGS="$CMAKE_FLAGS -DQZ_JIT_ENABLED=ON"
        CMAKE_FLAGS="$CMAKE_FLAGS -DQZ_JIT_THRESHOLD=${JIT_THRESHOLD}"
    else
        CMAKE_FLAGS="$CMAKE_FLAGS -DQZ_JIT_ENABLED=OFF"
    fi
    
    # JIT debug
    if [[ "$JIT_DEBUG" == "true" ]]; then
        CMAKE_FLAGS="$CMAKE_FLAGS -DQZ_JIT_DEBUG=ON"
    fi
    
    # LTO
    if [[ "$LTO_ENABLED" == "true" ]]; then
        CMAKE_FLAGS="$CMAKE_FLAGS -DQZ_LTO_ENABLED=ON"
    fi
    
    # Native architecture optimizations
    if [[ "$NATIVE_ARCH" == "true" ]]; then
        CMAKE_FLAGS="$CMAKE_FLAGS -DQZ_NATIVE_ARCH=ON"
    fi
    
    # VM configuration
    CMAKE_FLAGS="$CMAKE_FLAGS -DQZ_VM_STACK_SIZE=${VM_STACK_SIZE}"
    
    if [[ "$VM_DEBUG_TRACE" == "true" ]]; then
        CMAKE_FLAGS="$CMAKE_FLAGS -DQZ_VM_DEBUG_TRACE=ON"
    fi
    
    if [[ "$VM_INSTRUCTION_CACHE" == "true" ]]; then
        CMAKE_FLAGS="$CMAKE_FLAGS -DQZ_VM_INSTRUCTION_CACHE=ON"
    fi

    if [[ "$VM_COMPUTED_GOTOS" == "true" ]]; then
        CMAKE_FLAGS="$CMAKE_FLAGS -DQZ_VM_COMPUTED_GOTOS=ON"
    fi

    # Memory configuration
    CMAKE_FLAGS="$CMAKE_FLAGS -DQZ_HEAP_INITIAL_MB=${HEAP_INITIAL_MB}"
    CMAKE_FLAGS="$CMAKE_FLAGS -DQZ_HEAP_MAX_MB=${HEAP_MAX_MB}"
    
    # Architecture for JIT
    CMAKE_FLAGS="$CMAKE_FLAGS -DQZ_ARCH=${ARCH}"
    
    # Run CMake
    info "Running CMake with flags: $CMAKE_FLAGS"
    cmake $CMAKE_FLAGS ..
    
    # Determine parallel jobs
    local JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    
    # Build
    info "Building with $JOBS parallel jobs..."
    cmake --build . -j "$JOBS"
    
    cd "$SCRIPT_DIR"
    
    success "Build complete!"
}

# =============================================================================
# Clean Build
# =============================================================================
clean_build() {
    local BUILD_PATH="${SCRIPT_DIR}/${BUILD_DIR}"
    
    if [[ -d "$BUILD_PATH" ]]; then
        info "Cleaning build directory: $BUILD_PATH"
        rm -rf "$BUILD_PATH"
        success "Build directory cleaned"
    fi
}

# =============================================================================
# Print Configuration
# =============================================================================
print_config() {
    echo ""
    echo "==================================="
    echo "Quartz Build Configuration"
    echo "==================================="
    echo "BUILD_TYPE:          $BUILD_TYPE"
    echo "JIT_ENABLED:         $JIT_ENABLED"
    echo "JIT_THRESHOLD:       $JIT_THRESHOLD"
    echo "JIT_DEBUG:           $JIT_DEBUG"
    echo "LTO_ENABLED:         $LTO_ENABLED"
    echo "NATIVE_ARCH:         $NATIVE_ARCH"
    echo "EXTENSIONS:          $EXTENSIONS"
    echo "VM_STACK_SIZE:       $VM_STACK_SIZE"
    echo "VM_INSTRUCTION_CACHE: $VM_INSTRUCTION_CACHE"
    echo "PLATFORM:            $PLATFORM"
    echo "ARCH:                $ARCH"
    echo "BUILD_DIR:           $BUILD_DIR"
    echo "==================================="
    echo ""
}

# =============================================================================
# Show Help
# =============================================================================
show_help() {
    echo "Quartz Build Script"
    echo ""
    echo "Usage: $0 [options]"
    echo ""
    echo "Options:"
    echo "  --clean         Clean build directory before building"
    echo "  --rebuild-jit   Force regeneration of JIT stencils"
    echo "  --config-only   Only generate configuration, don't build"
    echo "  --help          Show this help message"
    echo ""
    echo "Configuration is read from build.conf"
}

# =============================================================================
# Main
# =============================================================================
main() {
    local DO_CLEAN=false
    local FORCE_JIT_REBUILD=false
    local CONFIG_ONLY=false
    
    # Parse command line arguments
    while [[ $# -gt 0 ]]; do
        case $1 in
            --clean)
                DO_CLEAN=true
                shift
                ;;
            --rebuild-jit)
                FORCE_JIT_REBUILD=true
                shift
                ;;
            --config-only)
                CONFIG_ONLY=true
                shift
                ;;
            --help)
                show_help
                exit 0
                ;;
            *)
                error "Unknown option: $1"
                ;;
        esac
    done
    
    info "Starting Quartz build..."
    
    # Parse configuration
    parse_config
    
    # Detect platform and architecture
    detect_platform
    detect_arch
    
    # Print configuration
    print_config
    
    # Clean if requested
    if [[ "$DO_CLEAN" == "true" ]]; then
        clean_build
    fi
    
    # Generate JIT stencils
    export FORCE_JIT_REBUILD
    generate_jit_stencils
    
    # Generate extensions configuration
    generate_extensions_config
    
    # Build unless config-only
    if [[ "$CONFIG_ONLY" != "true" ]]; then
        build_cmake
        
        echo ""
        success "Quartz built successfully!"
        echo ""
        echo "Binary: ${BUILD_DIR}/quartz"
        echo "Library: ${BUILD_DIR}/libqz-core.so"
        echo ""
    fi
}

main "$@"
