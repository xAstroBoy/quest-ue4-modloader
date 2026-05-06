import struct
from pathlib import Path


ROOT = Path(r"c:\Users\xAstroBoy\Desktop\UE4-5 Quest Modloader\Pinball FX VR Patches")
LIBS = [ROOT / "libUnreal.so", ROOT / "libUnreal_new.so"]

# Stable Pinball FX VR FUObjectItem lookup sequence.
# The 8 bytes immediately before the match are ADRP+ADD -> GUObjectArray.
PATTERN = bytes.fromhex(
    "88 0E 40 B9 2A 25 40 B9 5F 01 08 6B 4D 03 00 54 "
    "0A FD 4D D3 29 09 40 F9 4A 3D 7D 92 08 3D 00 12 "
    "29 69 6A F8 8A 02 80 52 17 25 AA 9B"
)


def read_sections(data):
    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x3A)[0]
    e_shnum = struct.unpack_from("<H", data, 0x3C)[0]
    e_shstrndx = struct.unpack_from("<H", data, 0x3E)[0]

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, sh_addralign, sh_entsize = struct.unpack_from(
            "<IIQQQQIIQQ", data, off
        )
        sections.append(
            {
                "name_idx": sh_name,
                "type": sh_type,
                "flags": sh_flags,
                "addr": sh_addr,
                "offset": sh_offset,
                "size": sh_size,
            }
        )

    shstr = sections[e_shstrndx]
    strtab = data[shstr["offset"] : shstr["offset"] + shstr["size"]]
    for section in sections:
        start = section["name_idx"]
        end = strtab.find(b"\x00", start)
        section["name"] = strtab[start:end].decode("utf-8", errors="replace")
    return sections


def file_off_to_va(sections, file_off):
    for section in sections:
        start = section["offset"]
        end = start + section["size"]
        if start <= file_off < end:
            return section["addr"] + (file_off - start)
    return None


def is_adrp(instr):
    return (instr & 0x9F000000) == 0x90000000


def is_add_imm64(instr):
    return (instr & 0xFF000000) == 0x91000000


def get_rd(instr):
    return instr & 0x1F


def get_rn(instr):
    return (instr >> 5) & 0x1F


def decode_adrp_page(instr, pc):
    immhi = (instr >> 5) & 0x7FFFF
    immlo = (instr >> 29) & 0x3
    imm = (immhi << 2) | immlo
    if imm & (1 << 20):
        imm |= ~((1 << 21) - 1)
    return (pc & ~0xFFF) + (imm << 12)


def decode_add_imm(instr):
    imm12 = (instr >> 10) & 0xFFF
    shift = (instr >> 22) & 0x3
    if shift == 1:
        imm12 <<= 12
    return imm12


def decode_adrp_add_target(data, sections, pattern_file_off):
    adrp_file_off = pattern_file_off - 8
    add_file_off = pattern_file_off - 4
    adrp = struct.unpack_from("<I", data, adrp_file_off)[0]
    add = struct.unpack_from("<I", data, add_file_off)[0]
    adrp_va = file_off_to_va(sections, adrp_file_off)

    if not is_adrp(adrp) or not is_add_imm64(add):
        raise RuntimeError("preceding instructions are not ADRP+ADD")
    if get_rd(adrp) != get_rd(add) or get_rd(adrp) != get_rn(add):
        raise RuntimeError("preceding ADRP+ADD do not share the same register")

    return decode_adrp_page(adrp, adrp_va) + decode_add_imm(add)


def verify_lib(path):
    data = path.read_bytes()
    sections = read_sections(data)
    matches = []

    search_from = 0
    while True:
        index = data.find(PATTERN, search_from)
        if index < 0:
            break
        matches.append(index)
        search_from = index + 1

    print(f"\n=== {path.name} ===")
    print(f"pattern matches: {len(matches)}")
    if not matches:
        return []

    targets = []
    for idx, match_off in enumerate(matches, 1):
        match_va = file_off_to_va(sections, match_off)
        target = decode_adrp_add_target(data, sections, match_off)
        targets.append(target)
        print(f"  #{idx:02d} match_off=0x{match_off:X} match_va=0x{match_va:X} -> GUObjectArray=0x{target:X}")

    return targets


if __name__ == "__main__":
    decoded = {}
    for lib in LIBS:
        decoded[lib.name] = verify_lib(lib)

    print("\n=== summary ===")
    old_target = decoded["libUnreal.so"][0] if decoded["libUnreal.so"] else None
    new_target = decoded["libUnreal_new.so"][0] if decoded["libUnreal_new.so"] else None
    print(f"old target: {hex(old_target) if old_target is not None else 'not found'}")
    print(f"new target: {hex(new_target) if new_target is not None else 'not found'}")
    if old_target is not None and new_target is not None:
        delta = new_target - old_target
        print(f"delta: {delta:+#x}")
