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
