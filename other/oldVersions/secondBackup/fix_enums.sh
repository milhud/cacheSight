#!/bin/bash
# fix_enums.sh - Fix duplicate enum values in the codebase

echo "Fixing duplicate enum values in cache optimizer source..."

# Create backup directory
mkdir -p backup
cp *.c *.cpp *.h backup/ 2>/dev/null

# Fix access_pattern_t enum references
echo "Fixing access_pattern_t references..."

# List of files that need updating
FILES="*.c *.cpp *.h"

# Replace access pattern enum values
for file in $FILES; do
    if [ -f "$file" ]; then
        # Skip binary files and backups
        if file "$file" | grep -q "text"; then
            echo -n "  Processing $file... "
            
            # Use sed to replace enum values, being careful with word boundaries
            sed -i.bak \
                -e 's/\bSEQUENTIAL\b/PATTERN_SEQUENTIAL/g' \
                -e 's/\bSTRIDED\b/PATTERN_STRIDED/g' \
                -e 's/\bRANDOM\b/PATTERN_RANDOM/g' \
                -e 's/\bGATHER_SCATTER\b/PATTERN_GATHER_SCATTER/g' \
                -e 's/\bNESTED_LOOP\b/PATTERN_NESTED_LOOP/g' \
                -e 's/\bINDIRECT_ACCESS\b/PATTERN_INDIRECT_ACCESS/g' \
                "$file"
            
            # Special handling for LOOP_CARRIED_DEP in access pattern context
            # This is tricky - we need context-aware replacement
            
            echo "done"
        fi
    fi
done

# Fix cache_antipattern_t enum references
echo "Fixing cache_antipattern_t references..."

for file in $FILES; do
    if [ -f "$file" ]; then
        if file "$file" | grep -q "text"; then
            echo -n "  Processing $file... "
            
            # Replace antipattern enum values
            sed -i \
                -e 's/\bHOTSPOT_REUSE\b/ANTIPATTERN_HOTSPOT_REUSE/g' \
                -e 's/\bTHRASHING\b/ANTIPATTERN_THRASHING/g' \
                -e 's/\bFALSE_SHARING\b/ANTIPATTERN_FALSE_SHARING/g' \
                -e 's/\bIRREGULAR_GATHER_SCATTER\b/ANTIPATTERN_IRREGULAR_GATHER_SCATTER/g' \
                -e 's/\bUNCOALESCED_ACCESS\b/ANTIPATTERN_UNCOALESCED_ACCESS/g' \
                -e 's/\bINSTRUCTION_OVERFLOW\b/ANTIPATTERN_INSTRUCTION_OVERFLOW/g' \
                -e 's/\bDEAD_STORES\b/ANTIPATTERN_DEAD_STORES/g' \
                -e 's/\bHIGH_ASSOCIATIVITY_PRESSURE\b/ANTIPATTERN_HIGH_ASSOCIATIVITY_PRESSURE/g' \
                -e 's/\bSTREAMING_EVICTION\b/ANTIPATTERN_STREAMING_EVICTION/g' \
                -e 's/\bSTACK_OVERFLOW\b/ANTIPATTERN_STACK_OVERFLOW/g' \
                -e 's/\bBANK_CONFLICTS\b/ANTIPATTERN_BANK_CONFLICTS/g' \
                "$file"
            
            echo "done"
        fi
    fi
done

# Now handle the ambiguous LOOP_CARRIED_DEP
echo "Handling LOOP_CARRIED_DEP ambiguity..."

# We need to manually review and fix files that use LOOP_CARRIED_DEP
# Let's create a context-aware fix

cat > fix_loop_carried_dep.py << 'EOF'
#!/usr/bin/env python3
import re
import sys

def fix_loop_carried_dep(filename):
    with open(filename, 'r') as f:
        content = f.read()
    
    # Pattern to find LOOP_CARRIED_DEP usage
    # Look for context clues like pattern->, antipattern->, case statements, etc.
    
    # Fix in access_pattern_t context
    content = re.sub(r'(\bpattern\s*==\s*)LOOP_CARRIED_DEP\b', r'\1PATTERN_LOOP_CARRIED_DEP', content)
    content = re.sub(r'(\bpattern\s*=\s*)LOOP_CARRIED_DEP\b', r'\1PATTERN_LOOP_CARRIED_DEP', content)
    content = re.sub(r'(case\s+)LOOP_CARRIED_DEP(\s*:.*access_pattern)', r'\1PATTERN_LOOP_CARRIED_DEP\2', content, flags=re.IGNORECASE)
    
    # Fix in cache_antipattern_t context
    content = re.sub(r'(\btype\s*==\s*)LOOP_CARRIED_DEP\b', r'\1ANTIPATTERN_LOOP_CARRIED_DEP', content)
    content = re.sub(r'(\btype\s*=\s*)LOOP_CARRIED_DEP\b', r'\1ANTIPATTERN_LOOP_CARRIED_DEP', content)
    content = re.sub(r'(case\s+)LOOP_CARRIED_DEP(\s*:.*antipattern)', r'\1ANTIPATTERN_LOOP_CARRIED_DEP\2', content, flags=re.IGNORECASE)
    
    # Fix in pattern classification context (usually antipattern)
    content = re.sub(r'(detected_type\s*=\s*)LOOP_CARRIED_DEP\b', r'\1ANTIPATTERN_LOOP_CARRIED_DEP', content)
    
    with open(filename, 'w') as f:
        f.write(content)

if __name__ == "__main__":
    for filename in sys.argv[1:]:
        print(f"Fixing LOOP_CARRIED_DEP in {filename}...")
        fix_loop_carried_dep(filename)
EOF

chmod +x fix_loop_carried_dep.py

# Run the Python script on relevant files
python3 fix_loop_carried_dep.py pattern_classifier.c pattern_detector.c ast_analyzer.cpp

# Clean up .bak files
rm -f *.bak

echo "Enum fixes complete!"
echo ""
echo "Now running a test compile to check for remaining issues..."

# Test compile a few key files
echo -n "Testing common.c... "
if gcc -Wall -Wextra -c common.c -o common.o 2>/tmp/enum_test.log; then
    echo "OK"
else
    echo "FAILED - Check /tmp/enum_test.log"
fi

echo -n "Testing pattern_classifier.c... "
if gcc -Wall -Wextra -c pattern_classifier.c -o pattern_classifier.o 2>/tmp/enum_test.log; then
    echo "OK"
else
    echo "FAILED - Check /tmp/enum_test.log"
fi

echo ""
echo "If any tests failed, check the error logs and manually fix remaining issues."
echo "Backup files are in the 'backup' directory."