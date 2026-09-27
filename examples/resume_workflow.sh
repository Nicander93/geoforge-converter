#!/bin/bash
# GeoForge Converter P3 Resume Feature - Example Workflow
# This script demonstrates the block-level resume functionality

set -e

# Configuration
CONVERTER="geoforge-converter"
INPUT_DIR="${INPUT_DIR:-./test_data/osgb_project}"
OUTPUT_DIR="${OUTPUT_DIR:-./output/tiles}"
LON="${LON:-120.0}"
LAT="${LAT:-30.0}"
ALT="${ALT:-0.0}"

echo "=== GeoForge Converter Resume Workflow Demo ==="
echo ""
echo "Input:  $INPUT_DIR"
echo "Output: $OUTPUT_DIR"
echo "Origin: lon=$LON, lat=$LAT, alt=$ALT"
echo ""

# Function to check if converter is available
check_converter() {
    if ! command -v $CONVERTER &> /dev/null; then
        echo "ERROR: $CONVERTER not found in PATH"
        echo "Please build and install the converter first."
        exit 1
    fi
}

# Function to count blocks by status
count_blocks() {
    local manifest="$OUTPUT_DIR/block_manifest.json"
    if [ -f "$manifest" ]; then
        echo "Block Status Summary:"
        jq -r '.blocks | group_by(.status) | .[] | "\(.[0].status): \(length)"' "$manifest" 2>/dev/null || echo "  (Unable to parse manifest)"
    else
        echo "  No manifest found"
    fi
}

# Scenario 1: Normal conversion with resume
scenario_normal_resume() {
    echo "=== Scenario 1: Normal Conversion with Resume ==="
    echo ""
    
    echo "Step 1: Clean output directory"
    rm -rf "$OUTPUT_DIR"
    mkdir -p "$OUTPUT_DIR"
    
    echo ""
    echo "Step 2: Start conversion (will simulate interruption)"
    echo "Press Ctrl+C after a few blocks complete to test resume..."
    echo ""
    
    # Run converter (user will interrupt with Ctrl+C)
    $CONVERTER \
        --format osgb \
        --input "$INPUT_DIR" \
        --output "$OUTPUT_DIR" \
        --lon $LON --lat $LAT --alt $ALT \
        --enable-texture-compress \
        || echo "(Interrupted by user or error)"
    
    echo ""
    echo "Step 3: Check partial progress"
    count_blocks
    
    echo ""
    echo "Step 4: Resume conversion"
    echo "This will only process incomplete blocks..."
    echo ""
    
    $CONVERTER \
        --format osgb \
        --input "$INPUT_DIR" \
        --output "$OUTPUT_DIR" \
        --lon $LON --lat $LAT --alt $ALT \
        --enable-texture-compress \
        --resume
    
    echo ""
    echo "Step 5: Verify completion"
    count_blocks
    if [ -f "$OUTPUT_DIR/tileset.json" ]; then
        echo "✓ Root tileset.json created"
    else
        echo "✗ Root tileset.json missing"
    fi
}

# Scenario 2: Parameter change detection
scenario_param_change() {
    echo "=== Scenario 2: Parameter Change Detection ==="
    echo ""
    
    echo "Step 1: Complete conversion without Draco"
    rm -rf "$OUTPUT_DIR"
    mkdir -p "$OUTPUT_DIR"
    
    $CONVERTER \
        --format osgb \
        --input "$INPUT_DIR" \
        --output "$OUTPUT_DIR" \
        --lon $LON --lat $LAT --alt $ALT
    
    echo ""
    echo "Step 2: Try to resume with Draco enabled (different params)"
    echo "Expected: Should start fresh due to params hash mismatch"
    echo ""
    
    $CONVERTER \
        --format osgb \
        --input "$INPUT_DIR" \
        --output "$OUTPUT_DIR" \
        --lon $LON --lat $LAT --alt $ALT \
        --enable-draco \
        --resume \
        2>&1 | grep -i "mismatch" || echo "(Check logs for 'params hash mismatch')"
}

# Scenario 3: Crash recovery simulation
scenario_crash_recovery() {
    echo "=== Scenario 3: Crash Recovery Simulation ==="
    echo ""
    
    echo "Step 1: Complete a normal conversion"
    rm -rf "$OUTPUT_DIR"
    mkdir -p "$OUTPUT_DIR"
    
    $CONVERTER \
        --format osgb \
        --input "$INPUT_DIR" \
        --output "$OUTPUT_DIR" \
        --lon $LON --lat $LAT --alt $ALT
    
    echo ""
    echo "Step 2: Simulate crash (modify manifest)"
    echo "Marking first block as 'running' to simulate incomplete update..."
    
    local manifest="$OUTPUT_DIR/block_manifest.json"
    if [ -f "$manifest" ]; then
        # Change first succeeded block to running
        jq '(.blocks[] | select(.status == "succeeded") | .status) |= "running"' \
            "$manifest" > "${manifest}.tmp" && mv "${manifest}.tmp" "$manifest"
        
        echo "Modified manifest:"
        count_blocks
    fi
    
    echo ""
    echo "Step 3: Resume (should reclaim the 'running' block)"
    echo "Expected: Recognize valid output and reclaim without reprocessing"
    echo ""
    
    $CONVERTER \
        --format osgb \
        --input "$INPUT_DIR" \
        --output "$OUTPUT_DIR" \
        --lon $LON --lat $LAT --alt $ALT \
        --resume \
        2>&1 | grep -i "reclaim" || echo "(Check logs for 'Reclaiming completed block')"
    
    echo ""
    echo "Final status:"
    count_blocks
}

# Scenario 4: Input file change detection
scenario_input_change() {
    echo "=== Scenario 4: Input File Change Detection ==="
    echo ""
    
    echo "Step 1: Complete conversion"
    rm -rf "$OUTPUT_DIR"
    mkdir -p "$OUTPUT_DIR"
    
    $CONVERTER \
        --format osgb \
        --input "$INPUT_DIR" \
        --output "$OUTPUT_DIR" \
        --lon $LON --lat $LAT --alt $ALT
    
    echo ""
    echo "Step 2: Modify an input file (change mtime)"
    # Find first OSGB file and touch it
    local first_osgb=$(find "$INPUT_DIR/Data" -name "*.osgb" -type f | head -1)
    if [ -n "$first_osgb" ]; then
        echo "Touching: $first_osgb"
        touch "$first_osgb"
    else
        echo "No OSGB files found to modify"
        return
    fi
    
    echo ""
    echo "Step 3: Resume (should detect changed file and reprocess that block)"
    echo "Expected: Reprocess modified block, reuse others"
    echo ""
    
    $CONVERTER \
        --format osgb \
        --input "$INPUT_DIR" \
        --output "$OUTPUT_DIR" \
        --lon $LON --lat $LAT --alt $ALT \
        --resume \
        2>&1 | grep -i "reprocess\|changed" || echo "(Check logs for 'will reprocess')"
}

# Main menu
main() {
    check_converter
    
    echo ""
    echo "Choose a scenario to run:"
    echo "  1) Normal resume workflow (interrupt & resume)"
    echo "  2) Parameter change detection"
    echo "  3) Crash recovery simulation"
    echo "  4) Input file change detection"
    echo "  5) Run all scenarios"
    echo "  q) Quit"
    echo ""
    read -p "Select option [1-5, q]: " choice
    
    case $choice in
        1) scenario_normal_resume ;;
        2) scenario_param_change ;;
        3) scenario_crash_recovery ;;
        4) scenario_input_change ;;
        5)
            scenario_normal_resume
            echo ""
            echo "=========================="
            echo ""
            scenario_param_change
            echo ""
            echo "=========================="
            echo ""
            scenario_crash_recovery
            echo ""
            echo "=========================="
            echo ""
            scenario_input_change
            ;;
        q|Q) echo "Exiting."; exit 0 ;;
        *) echo "Invalid option"; exit 1 ;;
    esac
    
    echo ""
    echo "=== Demo Complete ==="
}

# Check if jq is available (for manifest parsing)
if ! command -v jq &> /dev/null; then
    echo "WARNING: jq not found. Manifest parsing will be limited."
    echo "Install jq for better output: sudo apt-get install jq"
    echo ""
fi

# Run main menu if no arguments, otherwise run specific scenario
if [ $# -eq 0 ]; then
    main
else
    case $1 in
        normal) scenario_normal_resume ;;
        param) scenario_param_change ;;
        crash) scenario_crash_recovery ;;
        input) scenario_input_change ;;
        *) echo "Usage: $0 [normal|param|crash|input]"; exit 1 ;;
    esac
fi
