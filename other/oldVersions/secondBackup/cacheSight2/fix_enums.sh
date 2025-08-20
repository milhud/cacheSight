#!/bin/bash
# manual_enum_fix.sh - Direct replacement approach

echo "Applying manual enum fixes..."

# First, let's see what files we have
echo "Found source files:"
ls -la *.c *.cpp *.h 2>/dev/null || echo "No source files found yet"

# Apply fixes only to files that exist
for file in *.c *.cpp *.h; do
    if [ -f "$file" ]; then
        echo "Fixing $file..."
        
        # Create a temporary file
        cp "$file" "${file}.tmp"
        
        # Apply all replacements
        sed -i \
            -e 's/\bcase SEQUENTIAL:/case PATTERN_SEQUENTIAL:/g' \
            -e 's/\bcase STRIDED:/case PATTERN_STRIDED:/g' \
            -e 's/\bcase RANDOM:/case PATTERN_RANDOM:/g' \
            -e 's/\bcase GATHER_SCATTER:/case PATTERN_GATHER_SCATTER:/g' \
            -e 's/\bcase NESTED_LOOP:/case PATTERN_NESTED_LOOP:/g' \
            -e 's/\bcase INDIRECT_ACCESS:/case PATTERN_INDIRECT_ACCESS:/g' \
            -e 's/\bpattern == SEQUENTIAL/pattern == PATTERN_SEQUENTIAL/g' \
            -e 's/\bpattern == STRIDED/pattern == PATTERN_STRIDED/g' \
            -e 's/\bpattern == RANDOM/pattern == PATTERN_RANDOM/g' \
            -e 's/\bpattern == GATHER_SCATTER/pattern == PATTERN_GATHER_SCATTER/g' \
            -e 's/\bpattern == NESTED_LOOP/pattern == PATTERN_NESTED_LOOP/g' \
            -e 's/\bpattern == INDIRECT_ACCESS/pattern == PATTERN_INDIRECT_ACCESS/g' \
            -e 's/\bpattern = SEQUENTIAL/pattern = PATTERN_SEQUENTIAL/g' \
            -e 's/\bpattern = STRIDED/pattern = PATTERN_STRIDED/g' \
            -e 's/\bpattern = RANDOM/pattern = PATTERN_RANDOM/g' \
            -e 's/\bpattern = GATHER_SCATTER/pattern = PATTERN_GATHER_SCATTER/g' \
            -e 's/\bpattern = NESTED_LOOP/pattern = PATTERN_NESTED_LOOP/g' \
            -e 's/\bpattern = INDIRECT_ACCESS/pattern = PATTERN_INDIRECT_ACCESS/g' \
            -e 's/dominant_pattern = SEQUENTIAL/dominant_pattern = PATTERN_SEQUENTIAL/g' \
            -e 's/dominant_pattern = STRIDED/dominant_pattern = PATTERN_STRIDED/g' \
            -e 's/dominant_pattern = RANDOM/dominant_pattern = PATTERN_RANDOM/g' \
            -e 's/dominant_pattern == SEQUENTIAL/dominant_pattern == PATTERN_SEQUENTIAL/g' \
            -e 's/dominant_pattern == STRIDED/dominant_pattern == PATTERN_STRIDED/g' \
            -e 's/dominant_pattern == RANDOM/dominant_pattern == PATTERN_RANDOM/g' \
            "$file"
            
        # Fix antipatterns
        sed -i \
            -e 's/\bcase HOTSPOT_REUSE:/case ANTIPATTERN_HOTSPOT_REUSE:/g' \
            -e 's/\bcase THRASHING:/case ANTIPATTERN_THRASHING:/g' \
            -e 's/\bcase FALSE_SHARING:/case ANTIPATTERN_FALSE_SHARING:/g' \
            -e 's/\bcase IRREGULAR_GATHER_SCATTER:/case ANTIPATTERN_IRREGULAR_GATHER_SCATTER:/g' \
            -e 's/\bcase UNCOALESCED_ACCESS:/case ANTIPATTERN_UNCOALESCED_ACCESS:/g' \
            -e 's/\bcase INSTRUCTION_OVERFLOW:/case ANTIPATTERN_INSTRUCTION_OVERFLOW:/g' \
            -e 's/\bcase DEAD_STORES:/case ANTIPATTERN_DEAD_STORES:/g' \
            -e 's/\bcase HIGH_ASSOCIATIVITY_PRESSURE:/case ANTIPATTERN_HIGH_ASSOCIATIVITY_PRESSURE:/g' \
            -e 's/\bcase STREAMING_EVICTION:/case ANTIPATTERN_STREAMING_EVICTION:/g' \
            -e 's/\bcase STACK_OVERFLOW:/case ANTIPATTERN_STACK_OVERFLOW:/g' \
            -e 's/\bcase BANK_CONFLICTS:/case ANTIPATTERN_BANK_CONFLICTS:/g' \
            -e 's/\btype = HOTSPOT_REUSE/type = ANTIPATTERN_HOTSPOT_REUSE/g' \
            -e 's/\btype = THRASHING/type = ANTIPATTERN_THRASHING/g' \
            -e 's/\btype = FALSE_SHARING/type = ANTIPATTERN_FALSE_SHARING/g' \
            -e 's/\btype = IRREGULAR_GATHER_SCATTER/type = ANTIPATTERN_IRREGULAR_GATHER_SCATTER/g' \
            -e 's/\btype = UNCOALESCED_ACCESS/type = ANTIPATTERN_UNCOALESCED_ACCESS/g' \
            -e 's/\btype = STREAMING_EVICTION/type = ANTIPATTERN_STREAMING_EVICTION/g' \
            -e 's/\bdetected_type = HOTSPOT_REUSE/detected_type = ANTIPATTERN_HOTSPOT_REUSE/g' \
            -e 's/\bdetected_type = THRASHING/detected_type = ANTIPATTERN_THRASHING/g' \
            -e 's/\bdetected_type = FALSE_SHARING/detected_type = ANTIPATTERN_FALSE_SHARING/g' \
            -e 's/\bdetected_type = STREAMING_EVICTION/detected_type = ANTIPATTERN_STREAMING_EVICTION/g' \
            "$file"
            
        # Fix LOOP_CARRIED_DEP based on context
        # In pattern context
        sed -i \
            -e 's/pattern = LOOP_CARRIED_DEP/pattern = PATTERN_LOOP_CARRIED_DEP/g' \
            -e 's/pattern == LOOP_CARRIED_DEP/pattern == PATTERN_LOOP_CARRIED_DEP/g' \
            -e 's/\->pattern == LOOP_CARRIED_DEP/->pattern == PATTERN_LOOP_CARRIED_DEP/g' \
            -e 's/case LOOP_CARRIED_DEP:.*\/\/ pattern/case PATTERN_LOOP_CARRIED_DEP: \/\/ pattern/g' \
            "$file"
            
        # In antipattern context  
        sed -i \
            -e 's/type = LOOP_CARRIED_DEP/type = ANTIPATTERN_LOOP_CARRIED_DEP/g' \
            -e 's/type == LOOP_CARRIED_DEP/type == ANTIPATTERN_LOOP_CARRIED_DEP/g' \
            -e 's/detected_type = LOOP_CARRIED_DEP/detected_type = ANTIPATTERN_LOOP_CARRIED_DEP/g' \
            -e 's/case LOOP_CARRIED_DEP:.*\/\/ antipattern/case ANTIPATTERN_LOOP_CARRIED_DEP: \/\/ antipattern/g' \
            "$file"
            
        # Cleanup temp file
        rm -f "${file}.tmp"
    fi
done

echo "Manual fixes complete!"