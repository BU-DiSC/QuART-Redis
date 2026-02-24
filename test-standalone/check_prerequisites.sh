#!/bin/bash
# Check prerequisites for QuART-Redis testing

# Get script directory and cd to it
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=========================================="
echo "QuART-Redis Test - Prerequisites Check"
echo "=========================================="
echo ""

WORKLOAD_DIR="../../bods/workloads"
KL_VALUES=(0 1 5 10 25 100)
N=500000000
TEST_PROG="../src/test-rax-quart-workloads"

ALL_GOOD=true

# Check 1: Test program source
echo "1. Checking test program source..."
if [ -f "../src/test_rax_quart_workloads.c" ]; then
    echo "   ✓ ../src/test_rax_quart_workloads.c exists"
else
    echo "   ✗ ../src/test_rax_quart_workloads.c not found"
    ALL_GOOD=false
fi

# Check 2: Test program executable
echo ""
echo "2. Checking test program executable..."
if [ -f "$TEST_PROG" ]; then
    echo "   ✓ $TEST_PROG exists"
else
    echo "   ✗ $TEST_PROG not found"
    echo "     Run: cd src && make test-rax-quart-workloads"
    ALL_GOOD=false
fi

# Check 3: QuART implementation
echo ""
echo "3. Checking QuART implementation..."
if [ -f "../src/rax_quart.c" ] && [ -f "../src/rax_quart.h" ]; then
    echo "   ✓ rax_quart.c and rax_quart.h exist"
else
    echo "   ✗ QuART implementation files missing"
    ALL_GOOD=false
fi

# Check 4: Workload files
echo ""
echo "4. Checking workload files..."
MISSING_FILES=0
for KL in "${KL_VALUES[@]}"; do
    FILE="${WORKLOAD_DIR}/workload_N${N}_K${KL}_L${KL}.bin"
    if [ -f "$FILE" ]; then
        SIZE=$(du -h "$FILE" | cut -f1)
        echo "   ✓ K=$KL, L=$KL: $FILE ($SIZE)"
    else
        echo "   ✗ K=$KL, L=$KL: $FILE (NOT FOUND)"
        MISSING_FILES=$((MISSING_FILES + 1))
        ALL_GOOD=false
    fi
done

if [ $MISSING_FILES -gt 0 ]; then
    echo ""
    echo "   Missing $MISSING_FILES workload file(s)"
    echo "   Generate them using the BoDS data generator in ../bods"
fi

# Check 5: BC calculator (for speedup calculations)
echo ""
echo "5. Checking dependencies..."
if command -v bc &> /dev/null; then
    echo "   ✓ bc (calculator) is installed"
else
    echo "   ✗ bc (calculator) not found - speedup calculations may fail"
    echo "     Install: sudo apt-get install bc (or yum install bc)"
fi

# Summary
echo ""
echo "=========================================="
if [ "$ALL_GOOD" = true ]; then
    echo "✓ All prerequisites met!"
    echo "Ready to run: cd standalone && ./test_quart_redis.sh"
else
    echo "✗ Some prerequisites are missing"
    echo "Please fix the issues above before running tests"
fi
echo "=========================================="
