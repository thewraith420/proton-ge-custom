#!/bin/bash
# ProtonGE-RSEQ Installer Script
# Builds and installs ProtonGE with RSEQ slice extension support

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
STEAM_COMPAT_DIR="$HOME/.steam/steam/compatibilitytools.d"
INSTALL_NAME="GE-Proton-RSEQ"
TARBALL="$SCRIPT_DIR/proton-ge-rseq.tar.gz"

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== ProtonGE-RSEQ Installer ===${NC}"
echo ""

# Check if build directory exists
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${RED}Error: Build directory not found at $BUILD_DIR${NC}"
    echo "Run configure first before installing."
    exit 1
fi

# Build
echo -e "${YELLOW}Building ProtonGE-RSEQ...${NC}"
if command -v ccache &> /dev/null; then
    echo "Using ccache for faster compilation"
    CC="ccache gcc" CXX="ccache g++" make -C "$BUILD_DIR" -j$(nproc)
else
    make -C "$BUILD_DIR" -j$(nproc)
fi

if [ $? -ne 0 ]; then
    echo -e "${RED}Build failed!${NC}"
    exit 1
fi

echo -e "${GREEN}Build completed successfully!${NC}"
echo ""

# Create tarball
echo -e "${YELLOW}Creating tarball...${NC}"
cd "$BUILD_DIR"
tar -czf "$TARBALL" -C dist .
cd "$SCRIPT_DIR"

TARBALL_SIZE=$(du -h "$TARBALL" | cut -f1)
echo -e "${GREEN}Tarball created: $TARBALL ($TARBALL_SIZE)${NC}"
echo ""

# Check if Steam compatibilitytools directory exists
if [ ! -d "$STEAM_COMPAT_DIR" ]; then
    echo -e "${YELLOW}Creating Steam compatibility tools directory...${NC}"
    mkdir -p "$STEAM_COMPAT_DIR"
fi

# Install
INSTALL_DIR="$STEAM_COMPAT_DIR/$INSTALL_NAME"
echo -e "${YELLOW}Installing to $INSTALL_DIR...${NC}"

# Backup old installation if it exists
if [ -d "$INSTALL_DIR" ]; then
    BACKUP_DIR="${INSTALL_DIR}.backup.$(date +%Y%m%d_%H%M%S)"
    echo -e "${YELLOW}Backing up existing installation to $BACKUP_DIR${NC}"
    mv "$INSTALL_DIR" "$BACKUP_DIR"
fi

# Create fresh installation directory
mkdir -p "$INSTALL_DIR"

# Extract
echo -e "${YELLOW}Extracting files...${NC}"
tar -xzf "$TARBALL" -C "$INSTALL_DIR"

echo -e "${GREEN}Installation complete!${NC}"
echo ""
echo -e "${GREEN}=== Summary ===${NC}"
echo "Installed to: $INSTALL_DIR"
echo "Tarball: $TARBALL ($TARBALL_SIZE)"
echo ""
echo -e "${YELLOW}Next steps:${NC}"
echo "1. Close Steam completely"
echo "2. Restart Steam"
echo "3. In ESO properties, select '$INSTALL_NAME' as compatibility tool"
echo "4. Launch ESO"
echo ""
echo -e "${YELLOW}Monitor RSEQ stats with:${NC}"
echo "sudo watch -n 1 'cat /sys/kernel/debug/rseq/stats | grep -E \"sgrant|syield|sexpir|srevok\"'"
echo ""
