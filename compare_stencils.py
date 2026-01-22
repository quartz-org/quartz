#!/usr/bin/env python3
import re
import sys

def extract_stencil_names_from_c(filepath):
    with open(filepath, 'r') as f:
        content = f.read()
    # Find STENCIL_NAMES array
    pattern = r'static const char\* STENCIL_NAMES\[\] = \{([^}]+)\}'
    match = re.search(pattern, content, re.DOTALL)
    if not match:
        print("Cannot find STENCIL_NAMES in", filepath)
        return []
    lines = match.group(1).split('\n')
    names = []
    for line in lines:
        line = line.strip()
        if line.startswith('"'):
            name = line.strip('",')
            names.append(name)
    return names

def extract_defined_stencils(filepath):
    with open(filepath, 'r') as f:
        content = f.read()
    # Find all void stencil_* functions
    pattern = r'void (stencil_[a-zA-Z0-9_]+)'
    return re.findall(pattern, content)

def main():
    # Paths relative to workspace root
    extract_file = 'jit/extract_stencils.cpp'
    stencils_file = 'jit/stencils/stencils.c'
    defined = extract_defined_stencils(stencils_file)
    expected = extract_stencil_names_from_c(extract_file)
    
    print(f'Expected stencils ({len(expected)}):')
    # print(expected)
    print(f'Defined stencils ({len(defined)}):')
    # print(defined)
    
    missing = [name for name in expected if name not in defined]
    extra = [name for name in defined if name not in expected]
    
    print('\nMissing stencils (in STENCIL_NAMES but not defined):')
    for m in missing:
        print('  ', m)
    print('\nExtra stencils (defined but not in STENCIL_NAMES):')
    for e in extra:
        print('  ', e)
    
    # Also check stencils_aarch64.c
    aarch64_file = 'jit/stencils/stencils_aarch64.c'
    defined_aarch64 = extract_defined_stencils(aarch64_file)
    missing_aarch64 = [name for name in expected if name not in defined_aarch64]
    print(f'\nMissing in aarch64 ({len(missing_aarch64)}):')
    for m in missing_aarch64[:20]:
        print('  ', m)
    if len(missing_aarch64) > 20:
        print(f'  ... and {len(missing_aarch64)-20} more')

if __name__ == '__main__':
    main()