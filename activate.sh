#!/bin/bash

# ESP-IDF Environment Activation Script
# This script activates the ESP-IDF v5.5.2 development environment
# Run with: source ./activate.sh

# Colors for output
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

# ESP-IDF path
ESP_IDF_PATH="$HOME/.espressif/v5.5.2/esp-idf"

echo -e "${YELLOW}ESP32 Audio Player - ESP-IDF Environment Setup${NC}"
echo "=============================================="

# Check if ESP-IDF exists
if [ ! -d "$ESP_IDF_PATH" ]; then
    echo -e "${RED}Error: ESP-IDF not found at $ESP_IDF_PATH${NC}"
    echo "Please install ESP-IDF v5.5.2 first."
    return 1 2>/dev/null || exit 1
fi

# Check if export.sh exists
if [ ! -f "$ESP_IDF_PATH/export.sh" ]; then
    echo -e "${RED}Error: export.sh not found in $ESP_IDF_PATH${NC}"
    echo "ESP-IDF installation may be incomplete."
    return 1 2>/dev/null || exit 1
fi

echo -e "ESP-IDF Path: ${GREEN}$ESP_IDF_PATH${NC}"

# Source the ESP-IDF environment
echo "Activating ESP-IDF environment..."
source "$ESP_IDF_PATH/export.sh"

# Check if activation was successful
if command -v idf.py &> /dev/null; then
    echo -e "${GREEN}✅ ESP-IDF environment activated successfully!${NC}"

    # Display version information
    echo ""
    echo "Environment Information:"
    echo "----------------------"
    echo -e "ESP-IDF Version: ${GREEN}$(idf.py --version 2>/dev/null || echo 'Unknown')${NC}"
    echo -e "Python: ${GREEN}$(python --version 2>/dev/null || echo 'Unknown')${NC}"
    echo -e "IDF Path: ${GREEN}$IDF_PATH${NC}"
    echo -e "IDF Tools Path: ${GREEN}$IDF_TOOLS_PATH${NC}"

    echo ""
    echo "Available Commands:"
    echo "- idf.py build        # Build the project"
    echo "- idf.py flash        # Flash to device"
    echo "- idf.py monitor      # Monitor serial output"
    echo "- idf.py flash monitor# Flash and monitor"
    echo "- idf.py clean        # Clean build files"
    echo "- idf.py menuconfig   # Configure project"

else
    echo -e "${RED}❌ ESP-IDF environment activation failed!${NC}"
    echo "Please check your ESP-IDF installation."
    return 1 2>/dev/null || exit 1
fi

# Set project-specific environment variables
export PROJECT_DIR="$(pwd)"
export PROJECT_NAME="esp32_audioplayer"

echo ""
echo -e "${GREEN}Ready for ESP32 development!${NC}"
echo "Project Directory: $PROJECT_DIR"

# Optional: Change to project directory if we're not already there
if [ "$(basename $(pwd))" != "esp32_audioplayer" ] && [ -d "esp32_audioplayer" ]; then
    echo "Changing to project directory..."
    cd esp32_audioplayer
fi