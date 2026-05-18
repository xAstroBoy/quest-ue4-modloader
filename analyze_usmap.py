#!/usr/bin/env python3
import struct

data = open(r'C:\Users\xAstroBoy\Desktop\Pinball FX VR paks\Mappings.usmap', 'rb').read()
payload = data[16:]  # Skip 16-byte header

# Names table
names_count = int.from_bytes(payload[0:4], 'little')
print(f'Names table count: {names_count}')

# Read first few names
pos = 4
names = []
for i in range(min(10, names_count)):
    name_len = int.from_bytes(payload[pos:pos+2], 'little')
    name = payload[pos+2:pos+2+name_len].decode('utf-8', errors='ignore')
    names.append(name)
    print(f'  Name[{i}]: len={name_len}, value="{name}"')
    pos += 2 + name_len

print(f'After first 10 names, payload position: {pos}')

# Enums table should be next
enums_count = int.from_bytes(payload[pos:pos+4], 'little')
print(f'\nEnums table count: {enums_count}')
pos += 4

# Read first enum
if enums_count > 0:
    enum_name_idx = int.from_bytes(payload[pos:pos+4], 'little', signed=True)
    enum_member_count = int.from_bytes(payload[pos+4:pos+6], 'little')
    print(f'  Enum[0]: name_idx={enum_name_idx} (max valid={names_count-1}), member_count={enum_member_count}')
    if enum_name_idx >= names_count or enum_name_idx < 0:
        print(f'    ERROR: name index {enum_name_idx} out of range for names table size {names_count}!')
