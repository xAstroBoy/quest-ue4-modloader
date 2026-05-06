"""
Scan for GUObjectArray in new libUnreal.so using known strings near its init.
Uses the MaxObjectsNotConsideredByGC string, finds ALL ADRP+LDR globals near it,
and dumps them all so we can see which one is the real GUObjectArray.
"""
import struct, sys

LIBBASE = 0x74f72a5000  # from /proc/maps
SO_PATH = r'c:\Users\xAstroBoy\Desktop\UE4-5 Quest Modloader\Pinball FX VR Patches\libUnreal_new.so'

data = open(SO_PATH, 'rb').read()
so_size = len(data)

# --- ELF parsing ---
def read_elf_sections(data):
    e_shoff = struct.unpack_from('<Q', data, 0x28)[0]
    e_shentsize = struct.unpack_from('<H', data, 0x3A)[0]
    e_shnum = struct.unpack_from('<H', data, 0x3C)[0]
    e_shstrndx = struct.unpack_from('<H', data, 0x3E)[0]
    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size = struct.unpack_from('<IIQQQI', data, off)[:6]
        # pad to 64 bytes
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = struct.unpack_from('<IIQQQQIIQQ', data, off)
        sections.append({'name_idx': sh_name, 'type': sh_type, 'flags': sh_flags,
                         'addr': sh_addr, 'offset': sh_offset, 'size': sh_size})
    # Get section name string table
    shstr = sections[e_shstrndx]
    strtab = data[shstr['offset']:shstr['offset']+shstr['size']]
    for s in sections:
        name_off = s['name_idx']
        end = strtab.index(b'\x00', name_off)
        s['name'] = strtab[name_off:end].decode('utf-8', errors='replace')
    return sections

sections = read_elf_sections(data)
text_sec = next((s for s in sections if s['name'] == '.text'), None)
rodata_sec = next((s for s in sections if s['name'] == '.rodata'), None)
# bss/data are in rw sections
rw_sections = [s for s in sections if (s['flags'] & 2) and not (s['flags'] & 4) and s['size'] > 0]

print("Sections:")
for s in sections:
    if s['name'] in ('.text', '.rodata', '.data', '.bss', '.data.rel.ro'):
        print(f"  {s['name']}: addr=0x{s['addr']:X} offset=0x{s['offset']:X} size=0x{s['size']:X}")

# File offset of string
NEEDLE = b'MaxObjectsNotConsideredByGC'
str_file_off = data.find(NEEDLE)
if str_file_off == -1:
    print("String not found!")
    sys.exit(1)

# The string's VA = LIBBASE + str_file_off (approximately, assuming 0 load bias in file)
# Actually we need to find the section that contains this file offset to get the VA
def file_off_to_va(sections, file_off):
    for s in sections:
        if s['offset'] <= file_off < s['offset'] + s['size'] and s['addr'] != 0:
            return s['addr'] + (file_off - s['offset'])
    return None

str_va = file_off_to_va(sections, str_file_off)
print(f"\nString '{NEEDLE.decode()}' at file offset 0x{str_file_off:X}, VA=0x{str_va:X}")

# Find RW sections that would contain GUObjectArray  
print("\nRW sections (candidates for globals):")
for s in rw_sections:
    print(f"  {s['name']}: addr=0x{s['addr']:X} offset=0x{s['offset']:X} size=0x{s['size']:X}")

# ARM64 ADRP+LDR scanner in .text to find refs to the string
def decode_adrp_page(instr, pc_va):
    immlo = (instr >> 29) & 0x3
    immhi = (instr >> 5) & 0x7FFFF
    imm = ((immhi << 2) | immlo) << 12
    if imm & (1 << 32):
        imm -= (1 << 33)
    return (pc_va & ~0xFFF) + imm

def is_adrp(instr):
    return (instr & 0x9F000000) == 0x90000000

def get_rd(instr):
    return instr & 0x1F

def get_rn(instr):
    return (instr >> 5) & 0x1F

def is_ldr_imm64(instr):
    return (instr & 0xFFC00000) == 0xF9400000

def decode_ldr_offset(instr):
    return ((instr >> 10) & 0xFFF) << 3

def is_add_imm(instr):
    return (instr & 0xFF000000) == 0x91000000

def decode_add_offset(instr):
    return (instr >> 10) & 0xFFF

# Scan .text for ADRP to string VA
if text_sec is None:
    print("No .text section found!")
    sys.exit(1)

text_data = data[text_sec['offset']:text_sec['offset']+text_sec['size']]
text_va_base = text_sec['addr']

print(f"\nScanning .text for refs to string VA 0x{str_va:X}...")
xrefs = []
for i in range(0, len(text_data)-8, 4):
    instr = struct.unpack_from('<I', text_data, i)[0]
    if not is_adrp(instr):
        continue
    pc_va = text_va_base + i
    page = decode_adrp_page(instr, pc_va)
    rd = get_rd(instr)
    
    # Check next instruction
    next_instr = struct.unpack_from('<I', text_data, i+4)[0]
    
    # ADRP + ADD (string addr)
    if is_add_imm(next_instr) and get_rn(next_instr) == rd:
        target = page + decode_add_offset(next_instr)
        if target == str_va:
            xrefs.append(pc_va)
    # ADRP + LDR (pointer from page+offset)
    # The string itself could also be referenced this way
    elif is_ldr_imm64(next_instr) and get_rn(next_instr) == rd:
        target = page + decode_ldr_offset(next_instr)
        if target == str_va:
            xrefs.append(pc_va)

print(f"Found {len(xrefs)} xrefs to string")

# RW VA range
rw_ranges = [(s['addr'], s['addr']+s['size']) for s in rw_sections if s['addr'] != 0]

def in_rw(va):
    for lo, hi in rw_ranges:
        if lo <= va < hi:
            return True
    return False

# For each xref, scan window for ADRP+LDR to rw (potential global)
print("\nGlobal candidates near string xrefs:")
seen = set()
for xref_va in xrefs:
    xref_off = xref_va - text_va_base
    window_start = max(0, xref_off - 80)
    window_end = min(len(text_data)-8, xref_off + 120)
    for i in range(window_start, window_end, 4):
        instr0 = struct.unpack_from('<I', text_data, i)[0]
        if not is_adrp(instr0):
            continue
        instr1 = struct.unpack_from('<I', text_data, i+4)[0]
        if not is_ldr_imm64(instr1):
            continue
        if get_rd(instr0) != get_rn(instr1):
            continue
        pc_va = text_va_base + i
        page = decode_adrp_page(instr0, pc_va)
        target = page + decode_ldr_offset(instr1)
        if in_rw(target) and target not in seen:
            seen.add(target)
            runtime_va = LIBBASE + target  # approximate (file VA == runtime VA for PIC)
            print(f"  File VA=0x{target:X}  Runtime~0x{runtime_va:X}  (from code at 0x{pc_va:X}, xref 0x{xref_va:X})")

print("\nDone.")
