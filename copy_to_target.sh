#!/bin/bash

# Default target IP
TARGET_IP="${1:-canaan.local}"
TARGET_USER="root"

# Color output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=== Copy to Target Device ==="
echo "Target: ${TARGET_USER}@${TARGET_IP}"
echo ""

# Check if detector binary exists and copy it
DETECTOR_BIN="src/reference/ai_poc/k230_bin/driver_assistant_detector/driver_assistant_detector.elf"
if [ -f "$DETECTOR_BIN" ]; then
    echo -e "${GREEN}[]${NC} Found detector binary"
    echo "    Copying $DETECTOR_BIN..."
    scp -p "$DETECTOR_BIN" "${TARGET_USER}@${TARGET_IP}:/sharefs/driver_assistant_detector/"
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}     Successfully copied detector binary${NC}"
    else
        echo -e "${RED}     Failed to copy detector binary${NC}"
    fi
else
    echo -e "${YELLOW}[!]${NC} Detector binary not found: $DETECTOR_BIN"
fi

echo ""

# Check if frontend binary exists and copy it along with www directory
FRONTEND_BIN="src/reference/ai_poc/k230_bin/driver_assistant_front/driver_assistant_front"
FRONTEND_WWW="src/reference/ai_poc/driver_assistant_front/www"

if [ -f "$FRONTEND_BIN" ]; then
    echo -e "${GREEN}[]${NC} Found frontend binary"
    echo "    Copying $FRONTEND_BIN..."
    scp -p "$FRONTEND_BIN" "${TARGET_USER}@${TARGET_IP}:/sharefs/driver_assistant_detector/"
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}     Successfully copied frontend binary${NC}"
    else
        echo -e "${RED}     Failed to copy frontend binary${NC}"
    fi

    # Copy www directory if it exists
    if [ -d "$FRONTEND_WWW" ]; then
        echo "    Copying $FRONTEND_WWW directory..."
        scp -r -p "$FRONTEND_WWW" "${TARGET_USER}@${TARGET_IP}:/sharefs/driver_assistant_detector/"
        if [ $? -eq 0 ]; then
            echo -e "${GREEN}     Successfully copied www directory${NC}"
        else
            echo -e "${RED}     Failed to copy www directory${NC}"
        fi
    else
        echo -e "${YELLOW}    [!] WWW directory not found: $FRONTEND_WWW${NC}"
    fi
else
    echo -e "${YELLOW}[!]${NC} Frontend binary not found: $FRONTEND_BIN"
fi

echo ""
echo "=== Copy complete ==="