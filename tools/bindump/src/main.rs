// tools/bindump/src/main.rs
// ═══════════════════════════════════════════════════════════════════════════════
// BINDUMP — Blazing-fast merged binary dump engine
// Builds a single .bdmp database from IDA + BINJA dumps.
// All queries use mmap + binary search — sub-millisecond function lookups.
// ═══════════════════════════════════════════════════════════════════════════════

use clap::{Parser, Subcommand};
use memmap2::Mmap;
use regex::Regex;
use std::collections::{BTreeMap, HashMap};
use std::fs::{self, File};
use std::io::{self, BufWriter, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::time::Instant;

// ═══════════════════════════════════════════════════════════════════════════════
// BINARY FORMAT CONSTANTS
// ═══════════════════════════════════════════════════════════════════════════════

const MAGIC: &[u8; 4] = b"BDMP";
const VERSION: u32 = 2;
const HEADER_SIZE: usize = 64;
const FUNC_RECORD_SIZE: usize = 128;
const SECTION_ENTRY_SIZE: usize = 24;

// Per-function blob section types
const SEC_ASM_IDA: u16 = 1;
const SEC_ASM_BINJA: u16 = 2;
const SEC_C_IDA: u16 = 3;
const SEC_C_BINJA: u16 = 4;
const SEC_IL_BINJA: u16 = 5;
const SEC_DETAIL_IDA: u16 = 6;
const SEC_DETAIL_BINJA: u16 = 7;
const SEC_XREF_IDA: u16 = 8;
const SEC_XREF_BINJA: u16 = 9;
// Bulk section types (merged file copies)
const SEC_STRINGS: u16 = 20;
const SEC_STRXREFS: u16 = 21;
const SEC_NAMES: u16 = 22;
const SEC_IMPORTS: u16 = 23;
const SEC_EXPORTS: u16 = 24;
const SEC_SEGMENTS: u16 = 25;
const SEC_OVERVIEW: u16 = 26;
const SEC_DATAVARS: u16 = 27;
const SEC_STRUCTURES: u16 = 28;
const SEC_TYPES: u16 = 29;
const SEC_VTABLES: u16 = 30;
const SEC_RTTI: u16 = 31;
const SEC_COMMENTS: u16 = 32;
const SEC_CALLGRAPH: u16 = 33;

fn section_name(t: u16) -> &'static str {
    match t {
        1 => "ASM_IDA", 2 => "ASM_BINJA", 3 => "C_IDA", 4 => "C_BINJA",
        5 => "IL_BINJA", 6 => "DETAIL_IDA", 7 => "DETAIL_BINJA",
        8 => "XREF_IDA", 9 => "XREF_BINJA",
        20 => "STRINGS", 21 => "STRXREFS", 22 => "NAMES",
        23 => "IMPORTS", 24 => "EXPORTS", 25 => "SEGMENTS",
        26 => "OVERVIEW", 27 => "DATAVARS", 28 => "STRUCTURES",
        29 => "TYPES", 30 => "VTABLES", 31 => "RTTI",
        32 => "COMMENTS", 33 => "CALLGRAPH",
        _ => "UNKNOWN",
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// DATA STRUCTURES
// ═══════════════════════════════════════════════════════════════════════════════

#[derive(Clone, Copy, Default)]
struct BlobRef {
    offset: u64,
    len: u32,
}

#[derive(Clone, Default)]
struct FuncRecord {
    addr: u64,
    asm_ida: BlobRef,
    asm_binja: BlobRef,
    c_ida: BlobRef,
    c_binja: BlobRef,
    il_binja: BlobRef,
    detail_ida: BlobRef,
    detail_binja: BlobRef,
    xref_ida: BlobRef,
    xref_binja: BlobRef,
}

struct DbHeader {
    func_count: u32,
    section_count: u32,
    func_index_offset: u64,
    section_dir_offset: u64,
    ida_base: u64,
    binja_base: u64,
}

struct SectionEntry {
    type_id: u16,
    offset: u64,
    size: u64,
}

// ═══════════════════════════════════════════════════════════════════════════════
// BINARY I/O HELPERS
// ═══════════════════════════════════════════════════════════════════════════════

fn read_u16(b: &[u8]) -> u16 { u16::from_le_bytes(b[..2].try_into().unwrap()) }
fn read_u32(b: &[u8]) -> u32 { u32::from_le_bytes(b[..4].try_into().unwrap()) }
fn read_u64(b: &[u8]) -> u64 { u64::from_le_bytes(b[..8].try_into().unwrap()) }

fn write_header(w: &mut impl Write, h: &DbHeader) -> io::Result<()> {
    let mut buf = [0u8; HEADER_SIZE];
    buf[0..4].copy_from_slice(MAGIC);
    buf[4..8].copy_from_slice(&VERSION.to_le_bytes());
    buf[8..12].copy_from_slice(&h.func_count.to_le_bytes());
    buf[12..16].copy_from_slice(&h.section_count.to_le_bytes());
    buf[16..24].copy_from_slice(&h.func_index_offset.to_le_bytes());
    buf[24..32].copy_from_slice(&h.section_dir_offset.to_le_bytes());
    buf[32..40].copy_from_slice(&h.ida_base.to_le_bytes());
    buf[40..48].copy_from_slice(&h.binja_base.to_le_bytes());
    w.write_all(&buf)
}

fn parse_header(data: &[u8]) -> Option<DbHeader> {
    if data.len() < HEADER_SIZE { return None; }
    if &data[0..4] != MAGIC { return None; }
    Some(DbHeader {
        func_count: read_u32(&data[8..]),
        section_count: read_u32(&data[12..]),
        func_index_offset: read_u64(&data[16..]),
        section_dir_offset: read_u64(&data[24..]),
        ida_base: read_u64(&data[32..]),
        binja_base: read_u64(&data[40..]),
    })
}

fn write_func_record(w: &mut impl Write, r: &FuncRecord) -> io::Result<()> {
    let mut buf = [0u8; FUNC_RECORD_SIZE];
    buf[0..8].copy_from_slice(&r.addr.to_le_bytes());
    let refs: [&BlobRef; 9] = [
        &r.asm_ida, &r.asm_binja, &r.c_ida, &r.c_binja, &r.il_binja,
        &r.detail_ida, &r.detail_binja, &r.xref_ida, &r.xref_binja,
    ];
    for (i, br) in refs.iter().enumerate() {
        let off = 8 + i * 12;
        buf[off..off + 8].copy_from_slice(&br.offset.to_le_bytes());
        buf[off + 8..off + 12].copy_from_slice(&br.len.to_le_bytes());
    }
    w.write_all(&buf)
}

fn parse_func_record(data: &[u8]) -> FuncRecord {
    let mut r = FuncRecord::default();
    r.addr = read_u64(&data[0..]);
    let refs: [&mut BlobRef; 9] = [
        &mut r.asm_ida, &mut r.asm_binja, &mut r.c_ida, &mut r.c_binja,
        &mut r.il_binja, &mut r.detail_ida, &mut r.detail_binja,
        &mut r.xref_ida, &mut r.xref_binja,
    ];
    for (i, br) in refs.into_iter().enumerate() {
        let off = 8 + i * 12;
        br.offset = read_u64(&data[off..]);
        br.len = read_u32(&data[off + 8..]);
    }
    r
}

fn write_section_entry(w: &mut impl Write, e: &SectionEntry) -> io::Result<()> {
    let mut buf = [0u8; SECTION_ENTRY_SIZE];
    buf[0..2].copy_from_slice(&e.type_id.to_le_bytes());
    buf[8..16].copy_from_slice(&e.offset.to_le_bytes());
    buf[16..24].copy_from_slice(&e.size.to_le_bytes());
    w.write_all(&buf)
}

fn parse_section_entry(data: &[u8]) -> SectionEntry {
    SectionEntry {
        type_id: read_u16(&data[0..]),
        offset: read_u64(&data[8..]),
        size: read_u64(&data[16..]),
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// SCANNER — Detects function boundaries in dump files (mmap-based, zero-copy)
// ═══════════════════════════════════════════════════════════════════════════════

struct FuncBlock {
    addr: u64,
    start: usize,
    end: usize,
}

/// Extract hex address from "sub_XXXXX" or "Sub_xxxxx"
fn addr_from_sub(name: &[u8]) -> Option<u64> {
    let s = std::str::from_utf8(name).ok()?.trim();
    let hex = s.strip_prefix("sub_")
        .or_else(|| s.strip_prefix("Sub_"))
        .or_else(|| s.strip_prefix("SUB_"))?;
    u64::from_str_radix(hex.trim(), 16).ok()
}

/// Extract hex address from "(0xXXXX)" at end of line
fn addr_from_paren(line: &[u8]) -> Option<u64> {
    let s = std::str::from_utf8(line).ok()?;
    let p = s.rfind("(0x").or_else(|| s.rfind("(0X"))?;
    let end = s[p + 1..].find(')')? + p + 1;
    let hex = &s[p + 3..end];
    u64::from_str_radix(hex, 16).ok()
}

/// Canonical address: normalize BINJA address by subtracting offset
fn canonical(raw: u64, offset: u64) -> u64 {
    if offset > 0 && raw >= offset { raw - offset } else { raw }
}

/// Iterate lines in mmap (yields start offset, end offset excluding newline)
struct LineIter<'a> {
    data: &'a [u8],
    pos: usize,
}
impl<'a> LineIter<'a> {
    fn new(data: &'a [u8]) -> Self { LineIter { data, pos: 0 } }
}
impl<'a> Iterator for LineIter<'a> {
    type Item = (usize, usize); // (line_start, line_end_excl_newline)
    fn next(&mut self) -> Option<Self::Item> {
        if self.pos >= self.data.len() { return None; }
        let start = self.pos;
        while self.pos < self.data.len() && self.data[self.pos] != b'\n' {
            self.pos += 1;
        }
        let mut end = self.pos;
        if end > start && self.data[end - 1] == b'\r' { end -= 1; }
        if self.pos < self.data.len() { self.pos += 1; } // skip \n
        Some((start, end))
    }
}

/// Scan ASM file for function blocks
fn scan_asm(mmap: &[u8], addr_offset: u64) -> Vec<FuncBlock> {
    let mut blocks = Vec::with_capacity(256_000);
    let mut cur_addr: Option<u64> = None;
    let mut block_start: usize = 0;

    for (ls, le) in LineIter::new(mmap) {
        let line = &mmap[ls..le];

        // Separator: ";----...----" (60+ dashes)
        if line.len() > 60 && line.starts_with(b";---") {
            if let Some(addr) = cur_addr.take() {
                blocks.push(FuncBlock { addr, start: block_start, end: ls });
            }
            block_start = ls;
        }

        // Function header: "; Function: sub_XXXXX"
        if line.starts_with(b"; Function: ") {
            if let Some(raw) = addr_from_sub(&line[12..]) {
                cur_addr = Some(canonical(raw, addr_offset));
            }
        }
    }

    if let Some(addr) = cur_addr {
        blocks.push(FuncBlock { addr, start: block_start, end: mmap.len() });
    }
    blocks
}

/// Scan decompiled C / IL pipeline file
fn scan_c(mmap: &[u8], addr_offset: u64) -> Vec<FuncBlock> {
    let mut blocks = Vec::with_capacity(256_000);
    let mut cur_addr: Option<u64> = None;
    let mut block_start: usize = 0;

    for (ls, le) in LineIter::new(mmap) {
        let line = &mmap[ls..le];

        // Separator: "/----...----/"
        if line.len() > 60 && line.starts_with(b"/---") {
            if let Some(addr) = cur_addr.take() {
                if addr != 0 {
                    blocks.push(FuncBlock { addr, start: block_start, end: ls });
                }
            }
            block_start = ls;
        }

        // Function header: "// Function: sub_XXXXX"
        if line.starts_with(b"// Function: ") {
            if let Some(raw) = addr_from_sub(&line[13..]) {
                cur_addr = Some(canonical(raw, addr_offset));
            } else {
                // Named function — defer addr to Address: line
                cur_addr = Some(0);
            }
        }

        // Address line: "// Address: 0xXXXXX"
        if (line.starts_with(b"// Address: 0x") || line.starts_with(b"// Address: 0X"))
            && cur_addr == Some(0)
        {
            if let Ok(s) = std::str::from_utf8(line) {
                if let Some(hx) = s.find("0x").or_else(|| s.find("0X")) {
                    let hex = s[hx + 2..].split(|c: char| !c.is_ascii_hexdigit()).next().unwrap_or("");
                    if let Ok(raw) = u64::from_str_radix(hex, 16) {
                        cur_addr = Some(canonical(raw, addr_offset));
                    }
                }
            }
        }
    }

    if let Some(addr) = cur_addr {
        if addr != 0 { blocks.push(FuncBlock { addr, start: block_start, end: mmap.len() }); }
    }
    blocks
}

/// Scan function details / xrefs ("[FUNC] sub_XXX" or "[FUNC] name (0xADDR)")
fn scan_func_blocks(mmap: &[u8], addr_offset: u64) -> Vec<FuncBlock> {
    let mut blocks = Vec::with_capacity(256_000);
    let mut cur_addr: Option<u64> = None;
    let mut block_start: usize = 0;

    for (ls, le) in LineIter::new(mmap) {
        let line = &mmap[ls..le];

        if line.starts_with(b"[FUNC] ") {
            if let Some(addr) = cur_addr.take() {
                blocks.push(FuncBlock { addr, start: block_start, end: ls });
            }
            block_start = ls;

            let content = &line[7..];
            let raw = addr_from_sub(content)
                .or_else(|| addr_from_paren(line))
                .unwrap_or(0);
            if raw != 0 {
                cur_addr = Some(canonical(raw, addr_offset));
            }
        }

        // BINJA detail separator
        if line.len() > 60 && line.starts_with(b"/---") && cur_addr.is_none() {
            block_start = ls;
        }
    }

    if let Some(addr) = cur_addr {
        blocks.push(FuncBlock { addr, start: block_start, end: mmap.len() });
    }
    blocks
}

/// Auto-detect BINJA address offset by comparing first function addresses
fn auto_detect_offset(ida_dir: &Path, binja_dir: &Path) -> u64 {
    let ida_first = first_func_addr(&ida_dir.join("ALL_ASSEMBLY.asm"));
    let binja_first = first_func_addr(&binja_dir.join("ALL_ASSEMBLY.asm"));

    if let (Some(ida), Some(binja)) = (ida_first, binja_first) {
        if binja > ida {
            let offset = binja - ida;
            eprintln!("  Auto-detected BINJA offset: 0x{:X} (IDA=0x{:X}, BINJA=0x{:X})", offset, ida, binja);
            return offset;
        }
    }
    eprintln!("  Could not auto-detect, assuming 0x400000");
    0x400000
}

fn first_func_addr(path: &Path) -> Option<u64> {
    let file = File::open(path).ok()?;
    let mmap = unsafe { Mmap::map(&file).ok()? };
    for (ls, le) in LineIter::new(&mmap) {
        let line = &mmap[ls..le];
        if line.starts_with(b"; Function: sub_") || line.starts_with(b"; Function: Sub_") {
            return addr_from_sub(&line[12..]);
        }
    }
    None
}

// ═══════════════════════════════════════════════════════════════════════════════
// DATABASE BUILDER
// ═══════════════════════════════════════════════════════════════════════════════

fn build_database(ida_dir: &Path, binja_dir: &Path, output: &Path, binja_offset: u64) -> io::Result<()> {
    let t0 = Instant::now();
    eprintln!("╔══════════════════════════════════════════════════════════════════╗");
    eprintln!("║  BUILDING MERGED DATABASE                                       ║");
    eprintln!("╚══════════════════════════════════════════════════════════════════╝");
    eprintln!("  IDA dir:    {}", ida_dir.display());
    eprintln!("  BINJA dir:  {}", binja_dir.display());
    eprintln!("  Output:     {}", output.display());
    eprintln!("  Offset:     0x{:X}\n", binja_offset);

    let mut func_map: HashMap<u64, FuncRecord> = HashMap::with_capacity(300_000);
    let mut out = BufWriter::with_capacity(16 * 1024 * 1024, File::create(output)?);
    let mut sections: Vec<SectionEntry> = Vec::new();

    // Placeholder header
    out.write_all(&[0u8; HEADER_SIZE])?;

    // ── Per-function sections ────────────────────────────────────────────

    // Helper: process a per-function file and write its section
    fn write_per_func_section(
        path: &Path,
        label: &str,
        sec_type: u16,
        scanner: fn(&[u8], u64) -> Vec<FuncBlock>,
        addr_offset: u64,
        func_map: &mut HashMap<u64, FuncRecord>,
        out: &mut BufWriter<File>,
        sections: &mut Vec<SectionEntry>,
        set_field: fn(&mut FuncRecord, BlobRef),
    ) -> io::Result<()> {
        if !path.exists() { return Ok(()); }
        eprint!("  {:<22}", format!("{} ...", label));

        let file = File::open(path)?;
        let mmap = unsafe { Mmap::map(&file)? };
        let blocks = scanner(&mmap, addr_offset);
        let sec_start = out.stream_position()?;
        let mut count = 0u32;

        for b in &blocks {
            if b.addr == 0 { continue; }
            let wp = out.stream_position()?;
            let chunk = &mmap[b.start..b.end];
            out.write_all(chunk)?;
            let entry = func_map.entry(b.addr).or_insert_with(|| {
                let mut r = FuncRecord::default();
                r.addr = b.addr;
                r
            });
            set_field(entry, BlobRef { offset: wp, len: chunk.len() as u32 });
            count += 1;
        }

        let sec_end = out.stream_position()?;
        let size = sec_end - sec_start;
        sections.push(SectionEntry { type_id: sec_type, offset: sec_start, size });
        eprintln!("{:>7} funcs  {:>8.1} MB", count, size as f64 / 1_048_576.0);
        Ok(())
    }

    write_per_func_section(
        &ida_dir.join("ALL_ASSEMBLY.asm"), "IDA ASM", SEC_ASM_IDA,
        scan_asm, 0, &mut func_map, &mut out, &mut sections,
        |r, b| r.asm_ida = b,
    )?;
    write_per_func_section(
        &binja_dir.join("ALL_ASSEMBLY.asm"), "BINJA ASM", SEC_ASM_BINJA,
        scan_asm, binja_offset, &mut func_map, &mut out, &mut sections,
        |r, b| r.asm_binja = b,
    )?;
    write_per_func_section(
        &ida_dir.join("ALL_DECOMPILED.c"), "IDA C", SEC_C_IDA,
        scan_c, 0, &mut func_map, &mut out, &mut sections,
        |r, b| r.c_ida = b,
    )?;
    write_per_func_section(
        &binja_dir.join("ALL_DECOMPILED.c"), "BINJA C", SEC_C_BINJA,
        scan_c, binja_offset, &mut func_map, &mut out, &mut sections,
        |r, b| r.c_binja = b,
    )?;
    write_per_func_section(
        &binja_dir.join("ALL_IL_PIPELINE.txt"), "BINJA IL", SEC_IL_BINJA,
        scan_c, binja_offset, &mut func_map, &mut out, &mut sections,
        |r, b| r.il_binja = b,
    )?;
    write_per_func_section(
        &ida_dir.join("ALL_FUNCTION_DETAILS.txt"), "IDA DETAILS", SEC_DETAIL_IDA,
        scan_func_blocks, 0, &mut func_map, &mut out, &mut sections,
        |r, b| r.detail_ida = b,
    )?;
    write_per_func_section(
        &binja_dir.join("ALL_FUNCTIONS_DETAIL.txt"), "BINJA DETAILS", SEC_DETAIL_BINJA,
        scan_func_blocks, binja_offset, &mut func_map, &mut out, &mut sections,
        |r, b| r.detail_binja = b,
    )?;
    write_per_func_section(
        &ida_dir.join("ALL_XREFS.txt"), "IDA XREFS", SEC_XREF_IDA,
        scan_func_blocks, 0, &mut func_map, &mut out, &mut sections,
        |r, b| r.xref_ida = b,
    )?;
    write_per_func_section(
        &binja_dir.join("ALL_XREFS.txt"), "BINJA XREFS", SEC_XREF_BINJA,
        scan_func_blocks, binja_offset, &mut func_map, &mut out, &mut sections,
        |r, b| r.xref_binja = b,
    )?;

    // ── Bulk sections (merged from both sources) ─────────────────────────

    eprintln!();
    let bulk_defs: &[(u16, &str, &str)] = &[
        (SEC_STRINGS,    "ALL_STRINGS.txt",        "ALL_STRINGS.txt"),
        (SEC_STRXREFS,   "ALL_STRING_XREFS.txt",   "ALL_STRING_XREFS.txt"),
        (SEC_NAMES,      "ALL_NAMES.txt",           "ALL_NAMES.txt"),
        (SEC_IMPORTS,    "ALL_IMPORTS.txt",          "ALL_IMPORTS.txt"),
        (SEC_EXPORTS,    "ALL_EXPORTS.txt",          "ALL_EXPORTS.txt"),
        (SEC_SEGMENTS,   "ALL_SEGMENTS.txt",         "ALL_SEGMENTS.txt"),
        (SEC_OVERVIEW,   "ALL_OVERVIEW.txt",         "ALL_OVERVIEW.txt"),
        (SEC_DATAVARS,   "ALL_DATA_VARIABLES.txt",   "ALL_DATA_VARIABLES.txt"),
        (SEC_STRUCTURES, "ALL_STRUCTURES.txt",       ""),
        (SEC_TYPES,      "ALL_TYPES.txt",            ""),
        (SEC_VTABLES,    "ALL_VTABLES.txt",          ""),
        (SEC_RTTI,       "ALL_RTTI.txt",             ""),
        (SEC_COMMENTS,   "ALL_COMMENTS.txt",         "ALL_COMMENTS_TAGS.txt"),
        (SEC_CALLGRAPH,  "ALL_CALL_GRAPH.txt",       "ALL_CALL_GRAPH.txt"),
    ];

    for &(sec_type, ida_file, binja_file) in bulk_defs {
        let ida_path = ida_dir.join(ida_file);
        let binja_path = binja_dir.join(binja_file);
        let ida_ok = !ida_file.is_empty() && ida_path.exists();
        let binja_ok = !binja_file.is_empty() && binja_path.exists();
        if !ida_ok && !binja_ok { continue; }

        eprint!("  {:<22}", format!("{} ...", section_name(sec_type)));
        let sec_start = out.stream_position()?;

        if ida_ok {
            let banner = format!("\n═══════════ IDA: {} ═══════════\n", ida_file);
            out.write_all(banner.as_bytes())?;
            let f = File::open(&ida_path)?;
            let m = unsafe { Mmap::map(&f)? };
            out.write_all(&m)?;
        }
        if binja_ok {
            let banner = format!("\n═══════════ BINJA: {} ═══════════\n", binja_file);
            out.write_all(banner.as_bytes())?;
            let f = File::open(&binja_path)?;
            let m = unsafe { Mmap::map(&f)? };
            out.write_all(&m)?;
        }

        let sec_end = out.stream_position()?;
        let size = sec_end - sec_start;
        sections.push(SectionEntry { type_id: sec_type, offset: sec_start, size });
        eprintln!("{:>8.1} MB", size as f64 / 1_048_576.0);
    }

    // ── Function index (sorted by address) ──────────────────────────────

    eprintln!();
    eprint!("  {:<22}", "FUNC INDEX ...");
    let func_index_offset = out.stream_position()?;
    let mut sorted: Vec<FuncRecord> = func_map.into_values().collect();
    sorted.sort_by_key(|f| f.addr);
    for r in &sorted {
        write_func_record(&mut out, r)?;
    }
    let func_count = sorted.len() as u32;
    eprintln!("{:>7} entries {:>5.1} MB",
        func_count, (func_count as f64 * FUNC_RECORD_SIZE as f64) / 1_048_576.0);

    // ── Section directory ────────────────────────────────────────────────

    let section_dir_offset = out.stream_position()?;
    for s in &sections {
        write_section_entry(&mut out, s)?;
    }

    // ── Fix up header ────────────────────────────────────────────────────

    out.flush()?;
    let total_size = out.stream_position()?;
    out.seek(SeekFrom::Start(0))?;
    write_header(&mut out, &DbHeader {
        func_count,
        section_count: sections.len() as u32,
        func_index_offset,
        section_dir_offset,
        ida_base: 0,
        binja_base: binja_offset,
    })?;
    out.flush()?;

    eprintln!("\n╔══════════════════════════════════════════════════════════════════╗");
    eprintln!("║  DATABASE BUILT SUCCESSFULLY                                    ║");
    eprintln!("╚══════════════════════════════════════════════════════════════════╝");
    eprintln!("  File:       {}", output.display());
    eprintln!("  Size:       {:.2} GB ({} bytes)", total_size as f64 / 1_073_741_824.0, total_size);
    eprintln!("  Functions:  {}", func_count);
    eprintln!("  Sections:   {}", sections.len());
    eprintln!("  Time:       {:.1}s", t0.elapsed().as_secs_f64());

    // Cross-coverage quick stats
    let mut both = 0u32;
    let mut ida_only = 0u32;
    let mut binja_only = 0u32;
    for r in &sorted {
        let has_ida = r.asm_ida.len > 0 || r.c_ida.len > 0;
        let has_binja = r.asm_binja.len > 0 || r.c_binja.len > 0;
        match (has_ida, has_binja) {
            (true, true) => both += 1,
            (true, false) => ida_only += 1,
            (false, true) => binja_only += 1,
            _ => {}
        }
    }
    eprintln!("  Cross-ref:  {} both | {} IDA-only | {} BINJA-only", both, ida_only, binja_only);

    Ok(())
}

// ═══════════════════════════════════════════════════════════════════════════════
// DATABASE READER — mmap-backed, binary search lookups
// ═══════════════════════════════════════════════════════════════════════════════

struct DbReader {
    mmap: Mmap,
    header: DbHeader,
    sections: Vec<SectionEntry>,
}

impl DbReader {
    fn open(path: &Path) -> Option<Self> {
        let file = File::open(path).ok()?;
        let mmap = unsafe { Mmap::map(&file).ok()? };
        if mmap.len() < HEADER_SIZE { return None; }
        let header = parse_header(&mmap)?;

        let sd_off = header.section_dir_offset as usize;
        let mut sections = Vec::new();
        for i in 0..header.section_count as usize {
            let off = sd_off + i * SECTION_ENTRY_SIZE;
            if off + SECTION_ENTRY_SIZE > mmap.len() { break; }
            sections.push(parse_section_entry(&mmap[off..]));
        }

        Some(DbReader { mmap, header, sections })
    }

    /// Binary search for function by canonical address — O(log n)
    fn lookup_func(&self, addr: u64) -> Option<FuncRecord> {
        let base = self.header.func_index_offset as usize;
        let count = self.header.func_count as usize;

        let mut lo = 0usize;
        let mut hi = count;
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            let rec_off = base + mid * FUNC_RECORD_SIZE;
            if rec_off + 8 > self.mmap.len() { return None; }
            let rec_addr = read_u64(&self.mmap[rec_off..]);
            if rec_addr < addr { lo = mid + 1; }
            else if rec_addr > addr { hi = mid; }
            else { return Some(parse_func_record(&self.mmap[rec_off..])); }
        }
        None
    }

    /// Fuzzy lookup: try exact, then BINJA-adjusted
    fn lookup_func_flex(&self, addr: u64) -> Option<FuncRecord> {
        self.lookup_func(addr)
            .or_else(|| {
                if self.header.binja_base > 0 && addr >= self.header.binja_base {
                    self.lookup_func(addr - self.header.binja_base)
                } else { None }
            })
    }

    fn read_blob(&self, br: &BlobRef) -> &str {
        if br.len == 0 { return ""; }
        let s = br.offset as usize;
        let e = s + br.len as usize;
        if e > self.mmap.len() { return ""; }
        std::str::from_utf8(&self.mmap[s..e]).unwrap_or("")
    }

    fn section_bytes(&self, type_id: u16) -> &[u8] {
        for s in &self.sections {
            if s.type_id == type_id {
                let start = s.offset as usize;
                let end = start + s.size as usize;
                if end <= self.mmap.len() { return &self.mmap[start..end]; }
            }
        }
        &[]
    }

    fn search_section(&self, type_id: u16, pattern: &str, max: usize) -> Vec<(usize, String)> {
        mmap_regex_search(self.section_bytes(type_id), pattern, max)
    }

    fn search_all_sections(&self, pattern: &str, max_per: usize) -> Vec<(String, Vec<(usize, String)>)> {
        let all = [
            SEC_ASM_IDA, SEC_ASM_BINJA, SEC_C_IDA, SEC_C_BINJA, SEC_IL_BINJA,
            SEC_DETAIL_IDA, SEC_DETAIL_BINJA, SEC_XREF_IDA, SEC_XREF_BINJA,
            SEC_STRINGS, SEC_STRXREFS, SEC_NAMES, SEC_IMPORTS, SEC_EXPORTS,
            SEC_SEGMENTS, SEC_OVERVIEW, SEC_DATAVARS, SEC_STRUCTURES,
            SEC_TYPES, SEC_VTABLES, SEC_RTTI, SEC_COMMENTS, SEC_CALLGRAPH,
        ];
        let mut results = Vec::new();
        for &t in &all {
            let hits = self.search_section(t, pattern, max_per);
            if !hits.is_empty() {
                results.push((section_name(t).to_string(), hits));
            }
        }
        results
    }

    fn iter_funcs(&self) -> impl Iterator<Item = FuncRecord> + '_ {
        let base = self.header.func_index_offset as usize;
        let count = self.header.func_count as usize;
        (0..count).map(move |i| {
            parse_func_record(&self.mmap[base + i * FUNC_RECORD_SIZE..])
        })
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// MMAP SEARCH UTILITIES
// ═══════════════════════════════════════════════════════════════════════════════

fn mmap_regex_search(bytes: &[u8], pattern: &str, max: usize) -> Vec<(usize, String)> {
    if bytes.is_empty() { return vec![]; }
    let re = match regex::bytes::RegexBuilder::new(pattern)
        .case_insensitive(true)
        .unicode(false)
        .build()
    {
        Ok(r) => r,
        Err(e) => { eprintln!("Invalid regex: {}", e); return vec![]; }
    };

    let mut results = Vec::new();
    let mut line_num = 1usize;
    let mut line_start = 0usize;

    for i in 0..bytes.len() {
        if bytes[i] == b'\n' {
            let lb = &bytes[line_start..i];
            if re.is_match(lb) {
                results.push((line_num, String::from_utf8_lossy(lb).into_owned()));
                if max > 0 && results.len() >= max { break; }
            }
            line_num += 1;
            line_start = i + 1;
        }
    }
    results
}

fn mmap_read_lines(bytes: &[u8], start: usize, end: usize) -> Vec<(usize, String)> {
    let mut results = Vec::new();
    let mut line_num = 1usize;
    let mut line_start = 0usize;
    for i in 0..bytes.len() {
        if bytes[i] == b'\n' {
            if line_num >= start && line_num <= end {
                let lb = &bytes[line_start..i];
                results.push((line_num, String::from_utf8_lossy(lb).into_owned()));
            }
            if line_num > end { break; }
            line_num += 1;
            line_start = i + 1;
        }
    }
    results
}

// ═══════════════════════════════════════════════════════════════════════════════
// RAW FILE QUERY (fallback when no DB available)
// ═══════════════════════════════════════════════════════════════════════════════

const RAW_FILE_KEYS: &[(&str, &str)] = &[
    ("asm", "ALL_ASSEMBLY.asm"), ("c", "ALL_DECOMPILED.c"),
    ("strings", "ALL_STRINGS.txt"), ("strxrefs", "ALL_STRING_XREFS.txt"),
    ("xrefs", "ALL_XREFS.txt"), ("names", "ALL_NAMES.txt"),
    ("imports", "ALL_IMPORTS.txt"), ("exports", "ALL_EXPORTS.txt"),
    ("segments", "ALL_SEGMENTS.txt"), ("overview", "ALL_OVERVIEW.txt"),
    ("datavars", "ALL_DATA_VARIABLES.txt"), ("callgraph", "ALL_CALL_GRAPH.txt"),
    ("funcdetails", "ALL_FUNCTION_DETAILS.txt"),
    ("funcdetails", "ALL_FUNCTIONS_DETAIL.txt"),
    ("il", "ALL_IL_PIPELINE.txt"),
    ("vtables", "ALL_VTABLES.txt"), ("rtti", "ALL_RTTI.txt"),
    ("structures", "ALL_STRUCTURES.txt"), ("types", "ALL_TYPES.txt"),
    ("comments", "ALL_COMMENTS.txt"), ("comments", "ALL_COMMENTS_TAGS.txt"),
];

fn resolve_raw_file(dir: &Path, key: &str) -> Option<PathBuf> {
    let lower = key.to_lowercase();
    for &(k, f) in RAW_FILE_KEYS {
        if k == lower {
            let p = dir.join(f);
            if p.exists() { return Some(p); }
        }
    }
    let p = dir.join(key);
    if p.exists() { return Some(p); }
    None
}

fn raw_extract_func(mmap: &[u8], addr_upper: &str, header_prefix: &[u8], boundary: &[u8]) -> Option<String> {
    let target = format!("sub_{}", addr_upper);
    let target_lower = target.to_lowercase();
    let mut in_func = false;
    let mut result = Vec::new();
    let mut brace_count: i32 = 0;
    let mut found_body = false;

    for (ls, le) in LineIter::new(mmap) {
        let line = &mmap[ls..le];
        let lossy = String::from_utf8_lossy(line);

        if !in_func {
            if line.starts_with(header_prefix) && (lossy.contains(&target) || lossy.contains(&target_lower)) {
                in_func = true;
                result.extend_from_slice(line);
                result.push(b'\n');
            }
        } else {
            if line.starts_with(boundary) && result.len() > 100 {
                break;
            }
            if line.starts_with(header_prefix) && !lossy.contains(&target) && !lossy.contains(&target_lower) {
                break;
            }
            result.extend_from_slice(line);
            result.push(b'\n');

            if header_prefix == b"// Function: " {
                for &b in line {
                    if b == b'{' { brace_count += 1; found_body = true; }
                    if b == b'}' { brace_count -= 1; }
                }
                if found_body && brace_count <= 0 && result.len() > 100 { break; }
            }
        }
    }

    if result.is_empty() { None }
    else { Some(String::from_utf8_lossy(&result).into_owned()) }
}

// ═══════════════════════════════════════════════════════════════════════════════
// ADDRESS NORMALIZATION
// ═══════════════════════════════════════════════════════════════════════════════

fn normalize_addr(s: &str) -> String {
    let s = s.trim().to_uppercase();
    let s = s.strip_prefix("SUB_").unwrap_or(&s);
    let s = s.strip_prefix("0X").unwrap_or(s);
    let s = s.strip_prefix("LOC_").unwrap_or(s);
    s.to_string()
}

fn parse_addr_val(s: &str) -> u64 {
    u64::from_str_radix(&normalize_addr(s), 16).unwrap_or(0)
}

fn parse_hex_u64(s: &str) -> Option<u64> {
    let trimmed = s.trim();
    let hex = trimmed
        .strip_prefix("0x")
        .or_else(|| trimmed.strip_prefix("0X"))
        .unwrap_or(trimmed);
    u64::from_str_radix(hex, 16).ok()
}

#[derive(Clone, Copy)]
enum PatTok {
    Any,
    Byte(u8),
}

fn arm64_is_adrp(w: u32) -> bool { (w & 0x9F00_0000) == 0x9000_0000 }
fn arm64_is_adr(w: u32) -> bool { (w & 0x9F00_0000) == 0x1000_0000 }
fn arm64_is_b_bl(w: u32) -> bool { (w & 0x7C00_0000) == 0x1400_0000 }
fn arm64_is_add_sub_imm(w: u32) -> bool {
    let top = w & 0xFF00_0000;
    top == 0x9100_0000 || top == 0xD100_0000
}
fn arm64_is_ldr_lit(w: u32) -> bool { (w & 0x3B00_0000) == 0x1800_0000 }
fn arm64_is_cbz_cbnz(w: u32) -> bool {
    let top = w & 0x7E00_0000;
    top == 0x3400_0000 || top == 0x3500_0000
}
fn arm64_is_tbz_tbnz(w: u32) -> bool {
    let top = w & 0x7E00_0000;
    top == 0x3600_0000 || top == 0x3700_0000
}

fn arm64_masked_tokens(raw: &[u8]) -> Vec<PatTok> {
    let mut out = Vec::with_capacity(raw.len());
    let mut i = 0usize;
    while i + 4 <= raw.len() {
        let w = u32::from_le_bytes([raw[i], raw[i + 1], raw[i + 2], raw[i + 3]]);
        let wildcard = arm64_is_adrp(w)
            || arm64_is_adr(w)
            || arm64_is_b_bl(w)
            || arm64_is_add_sub_imm(w)
            || arm64_is_ldr_lit(w)
            || arm64_is_cbz_cbnz(w)
            || arm64_is_tbz_tbnz(w);
        if wildcard {
            out.extend([PatTok::Any, PatTok::Any, PatTok::Any, PatTok::Any]);
        } else {
            out.extend([
                PatTok::Byte(raw[i]),
                PatTok::Byte(raw[i + 1]),
                PatTok::Byte(raw[i + 2]),
                PatTok::Byte(raw[i + 3]),
            ]);
        }
        i += 4;
    }
    out
}

fn parse_pattern_tokens(s: &str) -> Option<Vec<PatTok>> {
    let mut toks = Vec::new();
    for part in s.split_whitespace() {
        if part == "??" {
            toks.push(PatTok::Any);
            continue;
        }
        let b = u8::from_str_radix(part, 16).ok()?;
        toks.push(PatTok::Byte(b));
    }
    if toks.is_empty() { None } else { Some(toks) }
}

fn format_pattern_tokens(tokens: &[PatTok]) -> String {
    let mut out = String::with_capacity(tokens.len() * 3);
    for (i, t) in tokens.iter().enumerate() {
        if i != 0 {
            out.push(' ');
        }
        match t {
            PatTok::Any => out.push_str("??"),
            PatTok::Byte(b) => out.push_str(&format!("{:02X}", b)),
        }
    }
    out
}

fn pattern_concrete_stats(tokens: &[PatTok]) -> (usize, f32) {
    if tokens.is_empty() {
        return (0, 0.0);
    }
    let concrete = tokens.iter().filter(|t| matches!(t, PatTok::Byte(_))).count();
    let ratio = concrete as f32 / tokens.len() as f32;
    (concrete, ratio)
}

fn count_pattern_hits(bytes: &[u8], tokens: &[PatTok], stop_after: usize) -> usize {
    if tokens.is_empty() || bytes.len() < tokens.len() {
        return 0;
    }
    let limit = if stop_after == 0 { usize::MAX } else { stop_after };

    let concrete: Vec<(usize, u8)> = tokens
        .iter()
        .enumerate()
        .filter_map(|(i, t)| match t {
            PatTok::Byte(b) => Some((i, *b)),
            PatTok::Any => None,
        })
        .collect();

    // All-wildcard pattern matches every position.
    if concrete.is_empty() {
        let total = bytes.len() - tokens.len() + 1;
        return total.min(limit);
    }

    // Use rightmost concrete byte as anchor (often most selective after masking).
    let (anchor_idx, anchor_byte) = concrete[concrete.len() - 1];
    let mut hits = 0usize;
    for i in 0..=(bytes.len() - tokens.len()) {
        if bytes[i + anchor_idx] != anchor_byte {
            continue;
        }
        let mut ok = true;
        for &(j, b) in &concrete {
            if bytes[i + j] != b {
                ok = false;
                break;
            }
        }
        if ok {
            hits += 1;
            if hits >= limit {
                break;
            }
        }
    }
    hits
}

fn pattern_hit_offsets(bytes: &[u8], tokens: &[PatTok], max_hits: usize) -> Vec<usize> {
    let mut hits = Vec::new();
    if tokens.is_empty() || bytes.len() < tokens.len() {
        return hits;
    }
    let limit = if max_hits == 0 { usize::MAX } else { max_hits };

    let concrete: Vec<(usize, u8)> = tokens
        .iter()
        .enumerate()
        .filter_map(|(i, t)| match t {
            PatTok::Byte(b) => Some((i, *b)),
            PatTok::Any => None,
        })
        .collect();

    if concrete.is_empty() {
        let total = bytes.len() - tokens.len() + 1;
        let max_n = total.min(limit);
        hits.reserve(max_n);
        for i in 0..max_n {
            hits.push(i);
        }
        return hits;
    }

    let (anchor_idx, anchor_byte) = concrete[concrete.len() - 1];
    for i in 0..=(bytes.len() - tokens.len()) {
        if bytes[i + anchor_idx] != anchor_byte {
            continue;
        }
        let mut ok = true;
        for &(j, b) in &concrete {
            if bytes[i + j] != b {
                ok = false;
                break;
            }
        }
        if ok {
            hits.push(i);
            if hits.len() >= limit {
                break;
            }
        }
    }
    hits
}

fn parse_u64_expr(mut s: &str) -> Option<u64> {
    s = s.trim();
    if s.is_empty() {
        return None;
    }

    let mut total: i128 = 0;
    let mut current_sign: i128 = 1;
    let mut saw_value = false;
    for raw_part in s.split_whitespace() {
        let part = raw_part.trim();
        if part == "+" {
            current_sign = 1;
            continue;
        }
        if part == "-" {
            current_sign = -1;
            continue;
        }

        let v = parse_hex_u64(part).or_else(|| part.parse::<u64>().ok())? as i128;
        total += current_sign * v;
        current_sign = 1;
        saw_value = true;
    }

    if !saw_value || total < 0 {
        None
    } else {
        Some(total as u64)
    }
}

fn parse_symbol_offsets(path: &Path) -> io::Result<Vec<(String, u64)>> {
    let text = fs::read_to_string(path)?;
    // Matches lines like: {"Name", 0x1234}, {"Name", 0x4B8CA44 - 0x400000},
    let re = Regex::new(r#"\{\s*\"([^\"]+)\"\s*,\s*([^\}]+)\}"#).expect("regex compile");
    let mut out = Vec::new();
    for cap in re.captures_iter(&text) {
        let name = cap.get(1).map(|m| m.as_str().trim()).unwrap_or("").to_string();
        if name.is_empty() {
            continue;
        }
        let expr = cap.get(2).map(|m| m.as_str().trim()).unwrap_or("");
        if let Some(v) = parse_u64_expr(expr) {
            if v != 0 {
                out.push((name, v));
            }
        }
    }
    Ok(out)
}

fn parse_symbol_names(path: &Path) -> io::Result<Vec<String>> {
    let text = fs::read_to_string(path)?;
    let re = Regex::new(r#"\{\s*\"([^\"]+)\""#).expect("regex compile");
    let mut out = Vec::new();
    let mut seen: HashMap<String, bool> = HashMap::new();

    for cap in re.captures_iter(&text) {
        if let Some(m) = cap.get(1) {
            let name = m.as_str().trim().to_string();
            if !name.is_empty() && !seen.contains_key(&name) {
                seen.insert(name.clone(), true);
                out.push(name);
            }
        }
    }

    if out.is_empty() {
        for line in text.lines() {
            let t = line.trim();
            if t.is_empty() || t.starts_with("//") || t.starts_with('#') {
                continue;
            }
            let name = t
                .split(',')
                .next()
                .unwrap_or("")
                .trim()
                .trim_matches('"')
                .to_string();
            if !name.is_empty() && !seen.contains_key(&name) {
                seen.insert(name.clone(), true);
                out.push(name);
            }
        }
    }

    Ok(out)
}

fn extract_first_addr_from_text(line: &str) -> Option<u64> {
    let bytes = line.as_bytes();
    // Prefer sub_XXXXX tokens.
    if let Some(pos) = line.find("sub_").or_else(|| line.find("Sub_")).or_else(|| line.find("SUB_")) {
        let start = pos + 4;
        let mut end = start;
        while end < bytes.len() && (bytes[end] as char).is_ascii_hexdigit() {
            end += 1;
        }
        if end > start {
            if let Ok(v) = u64::from_str_radix(&line[start..end], 16) {
                return Some(v);
            }
        }
    }
    // Fallback: any 0xADDR token.
    if let Some(pos) = line.find("0x").or_else(|| line.find("0X")) {
        let start = pos + 2;
        let mut end = start;
        while end < bytes.len() && (bytes[end] as char).is_ascii_hexdigit() {
            end += 1;
        }
        if end > start {
            if let Ok(v) = u64::from_str_radix(&line[start..end], 16) {
                return Some(v);
            }
        }
    }
    None
}

fn resolve_symbol_addr_from_db(db: &DbReader, aliases: &AliasMap, name: &str) -> Option<u64> {
    if let Some(addr) = aliases.resolve_name(name) {
        return Some(addr);
    }

    // First try names/symbols section.
    let pat = regex::escape(name);
    let hits = db.search_section(SEC_NAMES, &pat, 50);
    for (_, line) in hits {
        if let Some(mut addr) = extract_first_addr_from_text(&line) {
            if db.header.binja_base > 0 && addr >= db.header.binja_base {
                addr -= db.header.binja_base;
            }
            return Some(addr);
        }
    }

    // Fallback: global search (expensive, but bounded).
    let all = db.search_all_sections(&pat, 20);
    for (_, section_hits) in all {
        for (_, line) in section_hits {
            if let Some(mut addr) = extract_first_addr_from_text(&line) {
                if db.header.binja_base > 0 && addr >= db.header.binja_base {
                    addr -= db.header.binja_base;
                }
                return Some(addr);
            }
        }
    }

    None
}

fn likely_non_file_data_symbol(name: &str) -> bool {
    matches!(
        name,
        "GNames" | "GUObjectArray" | "GEngine" | "GWorld"
    )
}

#[derive(Clone)]
struct PortResult {
    name: String,
    old_off: u64,
    shift: isize,
    len: usize,
    old_hits: usize,
    new_hits: usize,
    new_first: Option<usize>,
    pattern: String,
}

#[derive(Clone)]
struct BatchResult {
    name: String,
    off: u64,
    shift: isize,
    len: usize,
    hits: usize,
    pattern: String,
}

fn evaluate_port_shift(
    old_mmap: &[u8],
    new_mmap: &[u8],
    name: &str,
    old_off: usize,
    off_raw: u64,
    min_bytes: usize,
    max_bytes: usize,
    shift: isize,
    lengths: &[usize],
    best_score: &mut Option<(usize, usize, usize, usize)>,
    best: &mut Option<PortResult>,
) {
    let start = if shift >= 0 {
        old_off + (shift as usize)
    } else {
        let neg = (-shift) as usize;
        if neg > old_off { return; }
        old_off - neg
    };
    if start + min_bytes > old_mmap.len() {
        return;
    }
    let hard_max = max_bytes.min(old_mmap.len() - start);
    let hard_max = hard_max - (hard_max % 4);
    if hard_max < min_bytes {
        return;
    }

    let masked = arm64_masked_tokens(&old_mmap[start..start + hard_max]);
    for &n in lengths {
        if n < min_bytes || n > hard_max {
            continue;
        }
        let toks = &masked[..n];
        let (concrete, ratio) = pattern_concrete_stats(toks);
        if concrete < 6 || ratio < 0.20 {
            continue;
        }
        // Fast uniqueness classification on NEW binary only during search.
        let new_hits = count_pattern_hits(new_mmap, toks, 8);
        let new_first = pattern_hit_offsets(new_mmap, toks, 1).first().copied();

        let score = (
            if new_hits == 1 { 0 } else { 1 },
            new_hits,
            n,
            shift.abs() as usize,
        );

        if best_score.map(|s| score < s).unwrap_or(true) {
            *best_score = Some(score);
            *best = Some(PortResult {
                name: name.to_string(),
                old_off: off_raw,
                shift,
                len: n,
                old_hits: 0,
                new_hits,
                new_first,
                pattern: format_pattern_tokens(toks),
            });
        }

        if new_hits == 1 {
            break;
        }
    }
}

fn evaluate_batch_shift(
    mmap: &[u8],
    name: &str,
    off: usize,
    off_raw: u64,
    min_bytes: usize,
    max_bytes: usize,
    shift: isize,
    lengths: &[usize],
    best_score: &mut Option<(usize, usize, usize)>,
    best: &mut Option<BatchResult>,
) {
    let start = if shift >= 0 {
        off + (shift as usize)
    } else {
        let neg = (-shift) as usize;
        if neg > off { return; }
        off - neg
    };
    if start + min_bytes > mmap.len() {
        return;
    }
    let hard_max = max_bytes.min(mmap.len() - start);
    let hard_max = hard_max - (hard_max % 4);
    if hard_max < min_bytes {
        return;
    }

    let masked = arm64_masked_tokens(&mmap[start..start + hard_max]);
    for &n in lengths {
        if n < min_bytes || n > hard_max {
            continue;
        }
        let toks = &masked[..n];
        let (concrete, ratio) = pattern_concrete_stats(toks);
        if concrete < 6 || ratio < 0.20 {
            continue;
        }
        let hits = count_pattern_hits(mmap, toks, 8);
        let score = (
            if hits == 1 { 0 } else { 1 },
            hits,
            n,
        );
        if best_score.map(|s| score < s).unwrap_or(true) {
            *best_score = Some(score);
            *best = Some(BatchResult {
                name: name.to_string(),
                off: off_raw,
                shift,
                len: n,
                hits,
                pattern: format_pattern_tokens(toks),
            });
        }
        if hits == 1 {
            break;
        }
    }
}

fn cmd_aob_batch(
    bin: &Path,
    source_file: &Path,
    base: Option<&str>,
    min_bytes: usize,
    max_bytes: usize,
    step: usize,
    start_max: usize,
    only_non_unique: bool,
    cpp: bool,
) {
    let symbols = match parse_symbol_offsets(source_file) {
        Ok(v) => v,
        Err(e) => {
            eprintln!("Failed to parse source file '{}': {}", source_file.display(), e);
            return;
        }
    };
    if symbols.is_empty() {
        eprintln!("No non-zero symbol offsets found in '{}'.", source_file.display());
        return;
    }

    cmd_aob_batch_with_symbols(
        bin,
        source_file,
        symbols,
        base,
        min_bytes,
        max_bytes,
        step,
        start_max,
        only_non_unique,
        cpp,
    );
}

fn cmd_aob_db_batch(
    db: &DbReader,
    aliases: &AliasMap,
    bin: &Path,
    source_file: &Path,
    base: Option<&str>,
    min_bytes: usize,
    max_bytes: usize,
    step: usize,
    start_max: usize,
    only_non_unique: bool,
    cpp: bool,
) {
    let names = match parse_symbol_names(source_file) {
        Ok(v) => v,
        Err(e) => {
            eprintln!("Failed to parse source names file '{}': {}", source_file.display(), e);
            return;
        }
    };
    if names.is_empty() {
        eprintln!("No symbol names found in '{}'.", source_file.display());
        return;
    }

    let mut symbols = Vec::new();
    let mut unresolved = 0usize;
    for name in names {
        if let Some(addr) = resolve_symbol_addr_from_db(db, aliases, &name) {
            symbols.push((name, addr));
        } else {
            unresolved += 1;
            eprintln!("[UNRESOLVED] {}", name);
        }
    }

    eprintln!(
        "Resolved {} symbols from DB (unresolved={})",
        symbols.len(), unresolved
    );

    if symbols.is_empty() {
        eprintln!("Nothing to process after DB resolution.");
        return;
    }

    cmd_aob_batch_with_symbols(
        bin,
        source_file,
        symbols,
        base,
        min_bytes,
        max_bytes,
        step,
        start_max,
        only_non_unique,
        cpp,
    );
}

fn cmd_aob_batch_with_symbols(
    bin: &Path,
    source_file: &Path,
    symbols: Vec<(String, u64)>,
    base: Option<&str>,
    min_bytes: usize,
    max_bytes: usize,
    step: usize,
    start_max: usize,
    only_non_unique: bool,
    cpp: bool,
) {
    let file = match File::open(bin) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("Failed to open binary '{}': {}", bin.display(), e);
            return;
        }
    };
    let mmap = match unsafe { Mmap::map(&file) } {
        Ok(m) => m,
        Err(e) => {
            eprintln!("Failed to map binary '{}': {}", bin.display(), e);
            return;
        }
    };

    let base_val = base.and_then(parse_hex_u64).unwrap_or(0);
    let step = (step.max(4) / 4) * 4;
    let min_bytes = ((min_bytes.max(4)) / 4) * 4;
    let max_bytes = ((max_bytes.max(min_bytes)) / 4) * 4;
    let start_max = ((start_max.max(0)) / 4) * 4;

    let total = symbols.len();
    let mut results: Vec<BatchResult> = Vec::new();
    let mut skipped = 0usize;

    for (idx, (name, mut off_raw)) in symbols.into_iter().enumerate() {
        if idx % 10 == 0 {
            eprintln!("[PROGRESS] {}/{} ...", idx + 1, total);
        }

        if base_val > 0 && off_raw >= base_val {
            off_raw -= base_val;
        }

        let off = off_raw as usize;
        if off >= mmap.len() {
            skipped += 1;
            if likely_non_file_data_symbol(&name) {
                eprintln!(
                    "[SKIP] {:<48} non-file global/data symbol (runtime VA), offset=0x{:X}, file_size=0x{:X}",
                    name, off_raw, mmap.len()
                );
            } else {
                eprintln!(
                    "[SKIP] {:<48} old offset out of range: 0x{:X} (file_size=0x{:X})",
                    name, off_raw, mmap.len()
                );
            }
            continue;
        }

        let mut best_score: Option<(usize, usize, usize)> = None;
        let mut best: Option<BatchResult> = None;

        let mut phase1_lengths = Vec::new();
        let mut n = min_bytes;
        while n <= max_bytes {
            phase1_lengths.push(n);
            if n < 48 {
                n += step.max(8);
            } else {
                n += 16;
            }
        }

        evaluate_batch_shift(
            &mmap,
            &name,
            off,
            off_raw,
            min_bytes,
            max_bytes,
            0,
            &phase1_lengths,
            &mut best_score,
            &mut best,
        );

        let unique = best.as_ref().map(|r| r.hits == 1).unwrap_or(false);
        if !unique {
            let mut shift = 4isize;
            while shift <= start_max as isize {
                let mut probe = vec![min_bytes, 24, 32, 48, 64, max_bytes];
                probe.sort_unstable();
                probe.dedup();
                evaluate_batch_shift(
                    &mmap,
                    &name,
                    off,
                    off_raw,
                    min_bytes,
                    max_bytes,
                    shift,
                    &probe,
                    &mut best_score,
                    &mut best,
                );
                if best.as_ref().map(|r| r.hits == 1).unwrap_or(false) {
                    break;
                }
                shift += 4;
            }
        }

        if let Some(r) = best {
            results.push(r);
        } else {
            skipped += 1;
            eprintln!("[SKIP] {:<48} unable to generate pattern", name);
        }
    }

    let mut unique = 0usize;
    let mut ambiguous = 0usize;
    let mut missing = 0usize;

    println!("Binary     : {}", bin.display());
    println!("Source file: {}", source_file.display());
    println!("Symbols    : {} (skipped={})\n", results.len(), skipped);

    for r in &results {
        if r.hits == 0 {
            missing += 1;
        } else if r.hits == 1 {
            unique += 1;
        } else {
            ambiguous += 1;
        }

        if only_non_unique && r.hits == 1 {
            continue;
        }

        let status = if r.hits == 0 { "MISS" } else if r.hits == 1 { "UNIQ" } else { "AMBG" };
        println!(
            "[{}] {:<48} off=0x{:08X} shift={:<3} len={:<3} hits={}",
            status, r.name, r.off, r.shift, r.len, r.hits
        );
    }

    println!(
        "\nSummary: total={} unique={} ambiguous={} missing={} skipped={}",
        results.len(), unique, ambiguous, missing, skipped
    );

    if cpp {
        println!("\nCPP pattern_signatures:");
        for r in &results {
            if only_non_unique && r.hits == 1 {
                continue;
            }
            println!("{{\"{}\", \"{}\", -1, 0}},", r.name, r.pattern);
        }
    }
}

fn cmd_aob_port(
    old_bin: &Path,
    new_bin: &Path,
    source_file: &Path,
    base_old: Option<&str>,
    min_bytes: usize,
    max_bytes: usize,
    step: usize,
    start_max: usize,
    only_non_unique: bool,
    cpp: bool,
) {
    let symbols = match parse_symbol_offsets(source_file) {
        Ok(v) => v,
        Err(e) => {
            eprintln!("Failed to parse source file '{}': {}", source_file.display(), e);
            return;
        }
    };
    if symbols.is_empty() {
        eprintln!("No non-zero symbol offsets found in '{}'.", source_file.display());
        return;
    }

    cmd_aob_port_with_symbols(
        old_bin,
        new_bin,
        symbols,
        base_old,
        min_bytes,
        max_bytes,
        step,
        start_max,
        only_non_unique,
        cpp,
    );
}

fn cmd_aob_port_db(
    db: &DbReader,
    aliases: &AliasMap,
    old_bin: &Path,
    new_bin: &Path,
    source_file: &Path,
    base_old: Option<&str>,
    min_bytes: usize,
    max_bytes: usize,
    step: usize,
    start_max: usize,
    only_non_unique: bool,
    cpp: bool,
) {
    let names = match parse_symbol_names(source_file) {
        Ok(v) => v,
        Err(e) => {
            eprintln!("Failed to parse source names file '{}': {}", source_file.display(), e);
            return;
        }
    };
    if names.is_empty() {
        eprintln!("No symbol names found in '{}'.", source_file.display());
        return;
    }

    let mut symbols = Vec::new();
    let mut unresolved = 0usize;
    for name in names {
        if let Some(addr) = resolve_symbol_addr_from_db(db, aliases, &name) {
            symbols.push((name, addr));
        } else {
            unresolved += 1;
            eprintln!("[UNRESOLVED] {}", name);
        }
    }

    eprintln!(
        "Resolved {} symbols from DB (unresolved={})",
        symbols.len(), unresolved
    );
    if symbols.is_empty() {
        eprintln!("Nothing to process after DB resolution.");
        return;
    }

    cmd_aob_port_with_symbols(
        old_bin,
        new_bin,
        symbols,
        base_old,
        min_bytes,
        max_bytes,
        step,
        start_max,
        only_non_unique,
        cpp,
    );
}

fn cmd_aob_port_with_symbols(
    old_bin: &Path,
    new_bin: &Path,
    symbols: Vec<(String, u64)>,
    base_old: Option<&str>,
    min_bytes: usize,
    max_bytes: usize,
    step: usize,
    start_max: usize,
    only_non_unique: bool,
    cpp: bool,
) {
    let old_file = match File::open(old_bin) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("Failed to open old binary '{}': {}", old_bin.display(), e);
            return;
        }
    };
    let new_file = match File::open(new_bin) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("Failed to open new binary '{}': {}", new_bin.display(), e);
            return;
        }
    };

    let old_mmap = match unsafe { Mmap::map(&old_file) } {
        Ok(m) => m,
        Err(e) => {
            eprintln!("Failed to map old binary '{}': {}", old_bin.display(), e);
            return;
        }
    };
    let new_mmap = match unsafe { Mmap::map(&new_file) } {
        Ok(m) => m,
        Err(e) => {
            eprintln!("Failed to map new binary '{}': {}", new_bin.display(), e);
            return;
        }
    };

    let base_old_val = base_old.and_then(parse_hex_u64).unwrap_or(0);
    let step = step.max(4);
    let step = (step / 4) * 4;
    let min_bytes = ((min_bytes.max(4)) / 4) * 4;
    let max_bytes = ((max_bytes.max(min_bytes)) / 4) * 4;
    let start_max = ((start_max.max(0)) / 4) * 4;

    let mut results: Vec<PortResult> = Vec::new();
    let mut skipped = 0usize;

    let total_symbols = symbols.len();
    for (idx, (name, mut off_raw)) in symbols.into_iter().enumerate() {
        if idx % 8 == 0 {
            eprintln!("[PROGRESS] {}/{} ...", idx + 1, total_symbols);
        }
        if base_old_val > 0 && off_raw >= base_old_val {
            off_raw -= base_old_val;
        }
        let old_off = off_raw as usize;
        if old_off >= old_mmap.len() {
            skipped += 1;
            if likely_non_file_data_symbol(&name) {
                eprintln!(
                    "[SKIP] {:<48} non-file global/data symbol (runtime VA), offset=0x{:X}, file_size=0x{:X}",
                    name,
                    off_raw,
                    old_mmap.len()
                );
            } else {
                eprintln!(
                    "[SKIP] {:<48} old offset out of range: 0x{:X} (file_size=0x{:X})",
                    name,
                    off_raw,
                    old_mmap.len()
                );
            }
            continue;
        }

        let mut best_score: Option<(usize, usize, usize, usize)> = None;
        let mut best: Option<PortResult> = None;

        // Phase 1: sparse growth at canonical offset (faster than full sweep)
        let mut phase1_lengths = Vec::new();
        let mut n = min_bytes;
        while n <= max_bytes {
            phase1_lengths.push(n);
            if n < 48 {
                n += step.max(8);
            } else {
                n += 16;
            }
        }
        evaluate_port_shift(
            &old_mmap,
            &new_mmap,
            &name,
            old_off,
            off_raw,
            min_bytes,
            max_bytes,
            0,
            &phase1_lengths,
            &mut best_score,
            &mut best,
        );

        // Phase 2: probe shifted starts with a tiny length set only if still not unique
        let already_unique = best
            .as_ref()
            .map(|r| r.new_hits == 1)
            .unwrap_or(false);
        if !already_unique {
            // Try positive shifts first
            let mut shift = 4isize;
            while shift <= start_max as isize {
                let mut probe = vec![min_bytes, 24, 32, 48, 64, max_bytes];
                probe.sort_unstable();
                probe.dedup();
                evaluate_port_shift(
                    &old_mmap,
                    &new_mmap,
                    &name,
                    old_off,
                    off_raw,
                    min_bytes,
                    max_bytes,
                    shift,
                    &probe,
                    &mut best_score,
                    &mut best,
                );

                let now_unique = best
                    .as_ref()
                    .map(|r| r.new_hits == 1)
                    .unwrap_or(false);
                if now_unique {
                    break;
                }
                shift += 4;
            }

            // Try negative shifts (start BEFORE the function to capture preceding unique bytes)
            // Use coarser steps since we're looking for bulk uniqueness from the prior function.
            let still_not_unique = best
                .as_ref()
                .map(|r| r.new_hits != 1)
                .unwrap_or(true);
            if still_not_unique {
                let neg_max = start_max as isize;
                // Try sparse negative shifts: 16, 32, 48, 64, 96, 128
                for neg_shift in [ -16isize, -32, -48, -64, -96, -128 ].iter() {
                    if *neg_shift < -neg_max { continue; }
                    let abs = (-neg_shift) as usize;
                    if abs > old_off { continue; }
                    let mut probe = vec![min_bytes, 32, 64, 96, max_bytes];
                    probe.sort_unstable();
                    probe.dedup();
                    evaluate_port_shift(
                        &old_mmap,
                        &new_mmap,
                        &name,
                        old_off,
                        off_raw,
                        min_bytes,
                        max_bytes,
                        *neg_shift,
                        &probe,
                        &mut best_score,
                        &mut best,
                    );

                    if best.as_ref().map(|r| r.new_hits == 1).unwrap_or(false) {
                        break;
                    }
                }
            }
        }

        if let Some(mut r) = best {
            // Final old-binary verification only for the selected candidate.
            let start = if r.shift >= 0 {
                old_off + (r.shift as usize)
            } else {
                old_off - ((-r.shift) as usize)
            };
            if start + r.len <= old_mmap.len() {
                let masked = arm64_masked_tokens(&old_mmap[start..start + r.len]);
                r.old_hits = count_pattern_hits(&old_mmap, &masked, 8);
            }
            results.push(r);
        } else {
            skipped += 1;
            eprintln!("[SKIP] {:<48} unable to generate pattern", name);
        }
    }

    println!("Old binary : {}", old_bin.display());
    println!("New binary : {}", new_bin.display());
    println!("Source symbols: in-memory list");
    println!("Symbols    : {} (skipped={})\n", results.len(), skipped);

    let mut unique_both = 0usize;
    let mut unique_new = 0usize;
    let mut ambig_new = 0usize;
    let mut missing_new = 0usize;

    for r in &results {
        if r.new_hits == 1 {
            unique_new += 1;
        } else if r.new_hits == 0 {
            missing_new += 1;
        } else {
            ambig_new += 1;
        }
        if r.new_hits == 1 && r.old_hits == 1 {
            unique_both += 1;
        }

        if only_non_unique && r.new_hits == 1 {
            continue;
        }

        let status = if r.new_hits == 1 {
            if r.old_hits == 1 { "UNIQ" } else { "NEW1" }
        } else if r.new_hits == 0 {
            "MISS"
        } else {
            "AMBG"
        };
        let new_at = r
            .new_first
            .map(|x| format!("0x{:X}", x))
            .unwrap_or_else(|| "-".to_string());

        println!(
            "[{}] {:<48} old=0x{:08X} shift={:<3} len={:<3} old_hits={:<3} new_hits={:<3} new_at={}",
            status, r.name, r.old_off, r.shift, r.len, r.old_hits, r.new_hits, new_at
        );
    }

    println!(
        "\nSummary: total={} unique_both={} unique_new={} ambig_new={} missing_new={} skipped={}",
        results.len(), unique_both, unique_new, ambig_new, missing_new, skipped
    );

    if cpp {
        println!("\nCPP pattern_signatures:");
        for r in &results {
            if only_non_unique && r.new_hits == 1 {
                continue;
            }
            println!("{{\"{}\", \"{}\", -1, 0}},", r.name, r.pattern);
        }
    }
}

fn cmd_aob_generate(
    bin: &Path,
    address: &str,
    name: Option<&str>,
    base: Option<&str>,
    start_offset: isize,
    min_bytes: usize,
    max_bytes: usize,
    step: usize,
    cpp: bool,
) {
    let file = match File::open(bin) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("Failed to open binary '{}': {}", bin.display(), e);
            return;
        }
    };
    let mmap = match unsafe { Mmap::map(&file) } {
        Ok(m) => m,
        Err(e) => {
            eprintln!("Failed to map binary '{}': {}", bin.display(), e);
            return;
        }
    };

    let mut addr = parse_addr_val(address);
    let base_val = base.and_then(parse_hex_u64).unwrap_or(0);
    if base_val > 0 && addr >= base_val {
        addr -= base_val;
    }
    let off = if start_offset >= 0 {
        addr as usize + (start_offset as usize)
    } else {
        let neg = (-start_offset) as usize;
        if neg > addr as usize { 
            eprintln!("Address out of range: 0x{:X} (+{})", addr, start_offset);
            return;
        }
        addr as usize - neg
    };
    if off >= mmap.len() {
        eprintln!("Address out of range: 0x{:X} (+{})", addr, start_offset);
        return;
    }

    let step = step.max(4);
    let min_bytes = (min_bytes.max(4) / 4) * 4;
    let max_bytes = (max_bytes.max(min_bytes) / 4) * 4;
    let available = mmap.len() - off;
    let hard_max = max_bytes.min(available - (available % 4));
    if hard_max < min_bytes {
        eprintln!("Not enough bytes at 0x{:X} for min_bytes={}.", addr, min_bytes);
        return;
    }

    let masked = arm64_masked_tokens(&mmap[off..off + hard_max]);

    let mut best_tokens: Option<Vec<PatTok>> = None;
    let mut best_hits = usize::MAX;
    let mut best_len = 0usize;

    let mut n = min_bytes;
    while n <= hard_max {
        let toks = masked[..n].to_vec();
        let hits = count_pattern_hits(&mmap, &toks, 4);
        if hits < best_hits {
            best_hits = hits;
            best_len = n;
            best_tokens = Some(toks.clone());
        }
        if hits == 1 {
            best_hits = hits;
            best_len = n;
            best_tokens = Some(toks);
            break;
        }
        n += step;
    }

    let Some(tokens) = best_tokens else {
        eprintln!("Failed to generate pattern.");
        return;
    };

    let pattern = format_pattern_tokens(&tokens);
    let resolved_name = name
        .map(|s| s.to_string())
        .unwrap_or_else(|| format!("sub_{:X}", addr as u64));

    println!("Binary: {}", bin.display());
    println!("Address: 0x{:X} (start+0x{:X})", addr, start_offset);
    println!("Length: {} bytes", best_len);
    println!("Hits:   {}", best_hits);
    println!("Pattern:\n{}", pattern);
    if cpp {
        println!("\nCPP:");
        println!("{{\"{}\", \"{}\", -1, 0}},", resolved_name, pattern);
    }

    if best_hits != 1 {
        eprintln!("WARNING: pattern is not unique (hits={}). Increase --max-bytes or use --start-offset.", best_hits);
    }
}

fn cmd_aob_verify(bin: &Path, pattern_file: &Path, only_non_unique: bool) {
    let file = match File::open(bin) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("Failed to open binary '{}': {}", bin.display(), e);
            return;
        }
    };
    let mmap = match unsafe { Mmap::map(&file) } {
        Ok(m) => m,
        Err(e) => {
            eprintln!("Failed to map binary '{}': {}", bin.display(), e);
            return;
        }
    };
    let content = match fs::read_to_string(pattern_file) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("Failed to read '{}': {}", pattern_file.display(), e);
            return;
        }
    };

    let re = Regex::new(r#"\{\s*\"([^\"]+)\"\s*,\s*\"([^\"]+)\"\s*,\s*-?\d+\s*,\s*\d+\s*\}"#)
        .expect("regex compile");

    let mut total = 0usize;
    let mut unique = 0usize;
    let mut ambiguous = 0usize;
    let mut missing = 0usize;

    for cap in re.captures_iter(&content) {
        let name = cap.get(1).map(|m| m.as_str()).unwrap_or("");
        let pat = cap.get(2).map(|m| m.as_str()).unwrap_or("");
        let Some(tokens) = parse_pattern_tokens(pat) else { continue; };
        total += 1;
        let hits = count_pattern_hits(&mmap, &tokens, usize::MAX);
        let status = if hits == 0 {
            missing += 1;
            "MISS"
        } else if hits == 1 {
            unique += 1;
            "UNIQUE"
        } else {
            ambiguous += 1;
            "AMBIG"
        };
        if !only_non_unique || hits != 1 {
            println!("[{:<6}] {:<48} hits={}", status, name, hits);
        }
    }

    println!("\nSummary: total={} unique={} ambiguous={} missing={}", total, unique, ambiguous, missing);
}

// ═══════════════════════════════════════════════════════════════════════════════
// FUNCTION ALIASES — persistent name→address mapping
// ═══════════════════════════════════════════════════════════════════════════════

struct AliasMap {
    path: PathBuf,
    map: BTreeMap<u64, String>,  // addr → friendly name
    reverse: HashMap<String, u64>,  // lowercase name → addr (for search)
    /// Aho-Corasick automaton for fast multi-pattern replacement (built lazily)
    ac: Option<aho_corasick::AhoCorasick>,
    ac_patterns: Vec<(String, String)>,  // (pattern, replacement) pairs
}

impl AliasMap {
    fn load(db_path: &Path) -> Self {
        let path = db_path.with_extension("aliases.json");
        let map: BTreeMap<u64, String> = if path.exists() {
            let data = fs::read_to_string(&path).unwrap_or_default();
            let raw: HashMap<String, String> = serde_json::from_str(&data).unwrap_or_default();
            raw.into_iter()
                .filter_map(|(k, v)| {
                    let addr = u64::from_str_radix(k.strip_prefix("0x").or(k.strip_prefix("0X")).unwrap_or(&k), 16).ok()?;
                    Some((addr, v))
                })
                .collect()
        } else {
            BTreeMap::new()
        };

        let reverse: HashMap<String, u64> = map.iter()
            .map(|(&addr, name)| (name.to_lowercase(), addr))
            .collect();

        let mut result = AliasMap { path, map, reverse, ac: None, ac_patterns: Vec::new() };
        result.rebuild_ac();
        result
    }

    /// Rebuild the Aho-Corasick automaton from current aliases
    fn rebuild_ac(&mut self) {
        if self.map.is_empty() {
            self.ac = None;
            self.ac_patterns.clear();
            return;
        }

        let mut patterns = Vec::new();
        let mut replacements = Vec::new();

        for (&addr, name) in &self.map {
            // Generate all casing variants: sub_XXXXX, sub_xxxxx, Sub_Xxxxx, 0xXXXXX, 0xxxxxx
            let hex_upper = format!("{:X}", addr);
            let hex_lower = format!("{:x}", addr);

            let sub_variants = [
                format!("sub_{}", hex_upper),
                format!("sub_{}", hex_lower),
                format!("Sub_{}", hex_upper),
                format!("Sub_{}", hex_lower),
                format!("SUB_{}", hex_upper),
                format!("SUB_{}", hex_lower),
            ];

            for pat in sub_variants {
                patterns.push(pat.clone());
                replacements.push(name.clone());
            }

            // Also match "0xADDR" style references (but only in call contexts like "0x4b8ca44(")
            // We add both casings of the hex address with 0x prefix
            let addr_variants = [
                format!("0x{}", hex_lower),  // BINJA style: 0x4b8ca44
                format!("0x{}", hex_upper),  // alternate: 0x4B8CA44
                format!("0X{}", hex_upper),
            ];
            for pat in addr_variants {
                patterns.push(pat.clone());
                replacements.push(name.clone());
            }
        }

        self.ac_patterns = patterns.iter().zip(replacements.iter())
            .map(|(p, r)| (p.clone(), r.clone()))
            .collect();

        // Build AC automaton — longest match first to avoid partial replacements
        if let Ok(ac) = aho_corasick::AhoCorasickBuilder::new()
            .match_kind(aho_corasick::MatchKind::LeftmostLongest)
            .build(&patterns)
        {
            self.ac = Some(ac);
        }
    }

    fn save(&self) -> io::Result<()> {
        let raw: BTreeMap<String, &String> = self.map.iter()
            .map(|(addr, name)| (format!("0x{:X}", addr), name))
            .collect();
        let json = serde_json::to_string_pretty(&raw).unwrap();
        fs::write(&self.path, json)
    }

    fn set(&mut self, addr: u64, name: String) {
        self.reverse.insert(name.to_lowercase(), addr);
        self.map.insert(addr, name);
        self.rebuild_ac();
    }

    fn remove(&mut self, addr: u64) -> bool {
        if let Some(name) = self.map.remove(&addr) {
            self.reverse.remove(&name.to_lowercase());
            self.rebuild_ac();
            true
        } else {
            false
        }
    }

    fn get(&self, addr: u64) -> Option<&str> {
        self.map.get(&addr).map(|s| s.as_str())
    }

    /// Reverse lookup: find address by alias name (case-insensitive)
    fn resolve_name(&self, name: &str) -> Option<u64> {
        self.reverse.get(&name.to_lowercase()).copied()
    }

    /// Format a function address with its alias if available
    fn fmt_addr(&self, addr: u64) -> String {
        if let Some(name) = self.get(addr) {
            format!("{} (0x{:X})", name, addr)
        } else {
            format!("sub_{:X}", addr)
        }
    }

    /// Replace ALL sub_XXXXX references in a string with their alias names.
    /// Uses Aho-Corasick for O(n) multi-pattern replacement — blazing fast.
    fn apply(&self, input: &str) -> String {
        if self.ac.is_none() || self.ac_patterns.is_empty() {
            return input.to_string();
        }
        let ac = self.ac.as_ref().unwrap();
        let mut result = String::with_capacity(input.len());
        let mut last = 0;

        for mat in ac.find_iter(input.as_bytes()) {
            let start = mat.start();
            let end = mat.end();
            let pat_idx = mat.pattern().as_usize();
            let replacement = &self.ac_patterns[pat_idx].1;

            // Boundary check: don't replace inside longer hex strings
            // e.g. don't replace "sub_4B8" inside "sub_4B8CA44"
            let after_ok = end >= input.len()
                || !input.as_bytes()[end].is_ascii_hexdigit();
            let before_ok = start == 0
                || !input.as_bytes()[start - 1].is_ascii_alphanumeric();

            if after_ok && before_ok {
                result.push_str(&input[last..start]);
                result.push_str(replacement);
                last = end;
            }
        }

        result.push_str(&input[last..]);
        result
    }

    /// Expand a search pattern: if it matches an alias name, add the sub_XXX pattern
    /// So searching "YUP_MakePropertyName" also finds "sub_4B8CA44"
    fn expand_pattern(&self, pattern: &str) -> String {
        // Check if pattern matches any alias name
        let mut alternatives = vec![regex::escape(pattern)];

        for (&addr, name) in &self.map {
            if name.to_lowercase().contains(&pattern.to_lowercase()) {
                // Add sub_ADDR variant so we find it in raw data
                alternatives.push(format!("sub_{:x}", addr));
                alternatives.push(format!("sub_{:X}", addr));
                alternatives.push(format!("0x{:x}", addr));
            }
        }

        // Also check if pattern IS a sub_XXX and add its alias
        if let Some(hex) = pattern.strip_prefix("sub_")
            .or_else(|| pattern.strip_prefix("Sub_"))
            .or_else(|| pattern.strip_prefix("SUB_"))
        {
            if let Ok(addr) = u64::from_str_radix(hex, 16) {
                if let Some(name) = self.get(addr) {
                    alternatives.push(regex::escape(name));
                }
            }
        }

        if alternatives.len() == 1 {
            pattern.to_string()  // no expansion needed, return original (may be regex)
        } else {
            alternatives.sort();
            alternatives.dedup();
            alternatives.join("|")
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// CLI
// ═══════════════════════════════════════════════════════════════════════════════

#[derive(Parser)]
#[command(name = "bindump", version, about = "Blazing-fast merged binary dump engine — IDA + BINJA cross-view database")]
struct Cli {
    /// Database file (.bdmp) or dump directory
    #[arg(short = 'p', long, env = "BINDUMP_DB")]
    db: Option<PathBuf>,

    /// Raw dump directory (fallback)
    #[arg(short, long, env = "BINDUMP_DIR")]
    dir: Option<PathBuf>,

    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Build merged database from IDA + BINJA dumps
    BuildDb {
        /// IDA dumps directory
        #[arg(long)]
        ida: PathBuf,
        /// BINJA dumps directory
        #[arg(long)]
        binja: PathBuf,
        /// Output .bdmp file (default: ./bindump.bdmp)
        #[arg(short, long)]
        output: Option<PathBuf>,
        /// BINJA address offset in hex (auto-detect if omitted)
        #[arg(long)]
        offset: Option<String>,
    },
    /// Show database / dump info & stats
    Info,
    /// Extract full function — BOTH IDA + BINJA views
    Func {
        address: String,
        /// Extract to files in ./extracted/sub_XXX/
        #[arg(short, long)]
        extract: bool,
    },
    /// Search ALL sections for pattern
    Search { pattern: String, #[arg(short, long, default_value = "0")] max: usize },
    /// Search strings
    Strings { pattern: String, #[arg(short, long, default_value = "0")] max: usize },
    /// Search string xrefs
    Strxrefs { pattern: String, #[arg(short, long, default_value = "0")] max: usize },
    /// Find xrefs for function address
    Xrefs { address: String },
    /// Search names/symbols
    Names { pattern: String, #[arg(short, long, default_value = "0")] max: usize },
    /// Search imports
    Imports { pattern: String, #[arg(short, long, default_value = "0")] max: usize },
    /// Search exports
    Exports { pattern: String, #[arg(short, long, default_value = "0")] max: usize },
    /// Show segments
    Segments,
    /// Show overview
    Overview,
    /// Search types
    Types { pattern: String, #[arg(short, long, default_value = "0")] max: usize },
    /// Search data variables
    Datavars { pattern: String, #[arg(short, long, default_value = "0")] max: usize },
    /// Function details (both IDA + BINJA)
    Funcdetail { address: String },
    /// IL pipeline (BINJA only)
    Il { address: String },
    /// Search comments
    Comments { pattern: String, #[arg(short, long, default_value = "0")] max: usize },
    /// Search structures
    Structures { pattern: String, #[arg(short, long, default_value = "0")] max: usize },
    /// Search vtables
    Vtables { pattern: String, #[arg(short, long, default_value = "0")] max: usize },
    /// Search call graph
    Callgraph { pattern: String, #[arg(short, long, default_value = "0")] max: usize },
    /// Read line range from section
    Read { section: String, start: usize, end: usize },
    /// Grep pattern in section
    Grep { pattern: String, section: String, #[arg(short, long, default_value = "0")] max: usize },
    /// Rename a function (set a friendly alias)
    Rename {
        /// Function address (hex) or existing alias name
        address: String,
        /// Friendly name for the function
        name: String,
    },
    /// Remove a function alias
    Unname {
        /// Function address (hex) or existing alias name
        address: String,
    },
    /// List all function aliases
    Aliases,
    /// Resolve an alias name to its address (or address to alias)
    Resolve {
        /// Function address or alias name
        query: String,
    },
    /// Generate masked ARM64 AOB pattern and auto-expand until unique hit
    Aob {
        /// Function address (hex, sub_xxx, or alias)
        address: String,
        /// Optional symbol name for CPP output
        #[arg(long)]
        name: Option<String>,
        /// Target ELF/binary file to scan (e.g. libUnreal.so)
        #[arg(long)]
        bin: PathBuf,
        /// Optional address base to subtract from input address (e.g. 0x400000 for BINJA)
        #[arg(long)]
        base: Option<String>,
        /// Start pattern extraction at address + this offset (negative = before)
        #[arg(long, default_value_t = 0, allow_hyphen_values = true)]
        start_offset: isize,
        /// Minimum bytes in pattern (must be multiple of 4)
        #[arg(long, default_value_t = 16)]
        min_bytes: usize,
        /// Maximum bytes in pattern (must be multiple of 4)
        #[arg(long, default_value_t = 96)]
        max_bytes: usize,
        /// Growth step in bytes while searching for uniqueness
        #[arg(long, default_value_t = 4)]
        step: usize,
        /// Emit ready-to-paste C++ pattern_signatures row
        #[arg(long, default_value_t = false)]
        cpp: bool,
    },
    /// Verify pattern uniqueness from C++ entries against a binary
    AobVerify {
        /// Target ELF/binary file (e.g. libUnreal.so)
        #[arg(long)]
        bin: PathBuf,
        /// Source file containing {"name","AOB",rip,size} entries
        #[arg(long)]
        file: PathBuf,
        /// Print only non-unique/missing entries
        #[arg(long, default_value_t = false)]
        only_non_unique: bool,
    },
    /// Fast batch AOB generation from confirmed offsets against one binary (old dump)
    AobBatch {
        /// Target ELF/binary file (e.g. old libUnreal.so)
        #[arg(long)]
        bin: PathBuf,
        /// Source file containing {"name", offset} entries
        #[arg(long)]
        file: PathBuf,
        /// Optional base to subtract from offsets (e.g. 0x400000)
        #[arg(long)]
        base: Option<String>,
        /// Minimum bytes in pattern (multiple of 4)
        #[arg(long, default_value_t = 16)]
        min_bytes: usize,
        /// Maximum bytes in pattern (multiple of 4)
        #[arg(long, default_value_t = 64)]
        max_bytes: usize,
        /// Growth step while searching
        #[arg(long, default_value_t = 4)]
        step: usize,
        /// Max starting shift (in bytes) from symbol offset
        #[arg(long, default_value_t = 16)]
        start_max: usize,
        /// Print only non-unique/missing entries
        #[arg(long, default_value_t = false)]
        only_non_unique: bool,
        /// Emit ready-to-paste C++ rows
        #[arg(long, default_value_t = false)]
        cpp: bool,
    },
    /// Fast batch AOB generation using symbol addresses resolved from existing bindump DB
    AobDbBatch {
        /// Target ELF/binary file (e.g. old libUnreal.so)
        #[arg(long)]
        bin: PathBuf,
        /// Source file containing symbol names or {"name", ...} rows
        #[arg(long)]
        file: PathBuf,
        /// Optional base to subtract from resolved addresses
        #[arg(long)]
        base: Option<String>,
        /// Minimum bytes in pattern (multiple of 4)
        #[arg(long, default_value_t = 16)]
        min_bytes: usize,
        /// Maximum bytes in pattern (multiple of 4)
        #[arg(long, default_value_t = 128)]
        max_bytes: usize,
        /// Growth step while searching
        #[arg(long, default_value_t = 4)]
        step: usize,
        /// Max starting shift (in bytes) from symbol offset
        #[arg(long, default_value_t = 32)]
        start_max: usize,
        /// Print only non-unique/missing entries
        #[arg(long, default_value_t = false)]
        only_non_unique: bool,
        /// Emit ready-to-paste C++ rows
        #[arg(long, default_value_t = false)]
        cpp: bool,
    },
    /// Generate signatures from old binary offsets and verify uniqueness in new binary
    AobPort {
        /// Old binary used for signature extraction
        #[arg(long)]
        old_bin: PathBuf,
        /// New binary used for verification
        #[arg(long)]
        new_bin: PathBuf,
        /// Source file containing {"Symbol", offset} entries
        #[arg(long)]
        file: PathBuf,
        /// Optional base to subtract from old offsets (e.g. 0x400000 for BINJA addresses)
        #[arg(long)]
        base_old: Option<String>,
        /// Minimum bytes in pattern (multiple of 4)
        #[arg(long, default_value_t = 16)]
        min_bytes: usize,
        /// Maximum bytes in pattern (multiple of 4)
        #[arg(long, default_value_t = 128)]
        max_bytes: usize,
        /// Growth step while searching
        #[arg(long, default_value_t = 4)]
        step: usize,
        /// Max starting shift (in bytes) from symbol offset
        #[arg(long, default_value_t = 32)]
        start_max: usize,
        /// Print only non-unique or missing-in-new signatures
        #[arg(long, default_value_t = false)]
        only_non_unique: bool,
        /// Emit ready-to-paste C++ rows
        #[arg(long, default_value_t = false)]
        cpp: bool,
    },
    /// Resolve symbol addresses from existing bindump DB, then build AOBs from old binary and verify on new binary
    AobPortDb {
        /// Old binary used for signature extraction
        #[arg(long)]
        old_bin: PathBuf,
        /// New binary used for verification
        #[arg(long)]
        new_bin: PathBuf,
        /// Source file containing symbol names (or {"Symbol", ...} lines)
        #[arg(long)]
        file: PathBuf,
        /// Optional base to subtract from old offsets
        #[arg(long)]
        base_old: Option<String>,
        /// Minimum bytes in pattern (multiple of 4)
        #[arg(long, default_value_t = 16)]
        min_bytes: usize,
        /// Maximum bytes in pattern (multiple of 4)
        #[arg(long, default_value_t = 128)]
        max_bytes: usize,
        /// Growth step while searching
        #[arg(long, default_value_t = 4)]
        step: usize,
        /// Max starting shift (in bytes) from symbol offset
        #[arg(long, default_value_t = 32)]
        start_max: usize,
        /// Print only non-unique or missing-in-new signatures
        #[arg(long, default_value_t = false)]
        only_non_unique: bool,
        /// Emit ready-to-paste C++ rows
        #[arg(long, default_value_t = false)]
        cpp: bool,
    },
}

// ═══════════════════════════════════════════════════════════════════════════════
// COMMAND HANDLERS
// ═══════════════════════════════════════════════════════════════════════════════

enum Backend {
    Db(DbReader),
    Raw(PathBuf),
}

fn cmd_func(be: &Backend, address: &str, extract: bool, aliases: &AliasMap) {
    let addr_upper = normalize_addr(address);
    let addr_val = parse_addr_val(address);
    let display_name = aliases.fmt_addr(addr_val);
    let t = Instant::now();

    println!("╔══════════════════════════════════════════════════════════════════╗");
    println!("║  FUNCTION: {:<53}║", display_name);
    println!("║  Canonical: 0x{:<50X}║", addr_val);
    println!("╚══════════════════════════════════════════════════════════════════╝");

    let extract_dir = if extract {
        let d = PathBuf::from(format!("extracted/sub_{}", addr_upper));
        fs::create_dir_all(&d).ok();
        Some(d)
    } else { None };

    match be {
        Backend::Db(db) => {
            let rec = db.lookup_func_flex(addr_val);
            if let Some(rec) = rec {
                let pairs: &[(&str, &str, &BlobRef)] = &[
                    ("IDA ASSEMBLY",     "ida_assembly.asm",      &rec.asm_ida),
                    ("BINJA ASSEMBLY",   "binja_assembly.asm",    &rec.asm_binja),
                    ("IDA DECOMPILED",   "ida_decompiled.c",      &rec.c_ida),
                    ("BINJA DECOMPILED", "binja_decompiled.c",    &rec.c_binja),
                    ("BINJA IL PIPELINE","binja_il_pipeline.txt", &rec.il_binja),
                    ("IDA DETAILS",      "ida_details.txt",       &rec.detail_ida),
                    ("BINJA DETAILS",    "binja_details.txt",     &rec.detail_binja),
                    ("IDA XREFS",        "ida_xrefs.txt",         &rec.xref_ida),
                    ("BINJA XREFS",      "binja_xrefs.txt",       &rec.xref_binja),
                ];

                let mut found_any = false;
                for &(label, filename, bref) in pairs {
                    let data = db.read_blob(bref);
                    if data.is_empty() { continue; }
                    found_any = true;
                    println!("\n──── {} ({} bytes) ────", label, data.len());
                    println!("{}", aliases.apply(&data));
                    if let Some(ref d) = extract_dir {
                        fs::write(d.join(filename), data).ok();
                    }
                }

                if !found_any {
                    println!("\n  Function record exists but no data blobs attached.");
                }

                if let Some(ref d) = extract_dir {
                    println!("\n✓ Extracted to {}/", d.display());
                }
            } else {
                println!("\n✗ Function {} not found (canonical 0x{:X})", display_name, addr_val);
                // Try nearest
                let nearest = find_nearest(db, addr_val, 5);
                if !nearest.is_empty() {
                    println!("  Nearest functions:");
                    for r in &nearest {
                        println!("    {}", aliases.fmt_addr(r.addr));
                    }
                }
            }
        }
        Backend::Raw(dir) => {
            let searches: &[(&str, &str, &[u8], &[u8])] = &[
                ("asm", "ASM", b"; Function: " as &[u8], b";---"),
                ("c", "DECOMPILED C", b"// Function: ", b"/---"),
            ];
            for &(key, label, hdr, boundary) in searches {
                if let Some(path) = resolve_raw_file(dir, key) {
                    let file = File::open(&path).unwrap();
                    let mmap = unsafe { Mmap::map(&file).unwrap() };
                    if let Some(content) = raw_extract_func(&mmap, &addr_upper, hdr, boundary) {
                        println!("\n──── {} ({} lines) ────", label, content.lines().count());
                        println!("{}", content);
                    }
                }
            }
        }
    }
    eprintln!("\nCompleted in {:.3}s", t.elapsed().as_secs_f64());
}

fn find_nearest(db: &DbReader, addr: u64, count: usize) -> Vec<FuncRecord> {
    let base = db.header.func_index_offset as usize;
    let total = db.header.func_count as usize;

    // Binary search to find insertion point
    let mut lo = 0usize;
    let mut hi = total;
    while lo < hi {
        let mid = lo + (hi - lo) / 2;
        let off = base + mid * FUNC_RECORD_SIZE;
        if off + 8 > db.mmap.len() { break; }
        let a = read_u64(&db.mmap[off..]);
        if a < addr { lo = mid + 1; } else { hi = mid; }
    }

    let start = if lo >= count { lo - count } else { 0 };
    let end = (lo + count).min(total);
    (start..end).map(|i| {
        parse_func_record(&db.mmap[base + i * FUNC_RECORD_SIZE..])
    }).collect()
}

fn cmd_search(be: &Backend, pattern: &str, max: usize, aliases: &AliasMap) {
    let t = Instant::now();
    let eff = if max == 0 { usize::MAX } else { max };
    let expanded = aliases.expand_pattern(pattern);

    match be {
        Backend::Db(db) => {
            let results = db.search_all_sections(&expanded, eff);
            let mut total = 0;
            for (name, hits) in &results {
                println!("\n══ {} ({} matches) ══", name, hits.len());
                for (ln, line) in hits.iter().take(50) {
                    println!("  L{:>8}: {}", ln, aliases.apply(line));
                }
                if hits.len() > 50 { println!("  ... and {} more", hits.len() - 50); }
                total += hits.len();
            }
            println!("\nTotal: {} matches across {} sections in {:.3}s",
                total, results.len(), t.elapsed().as_secs_f64());
        }
        Backend::Raw(dir) => {
            for &(key, filename) in RAW_FILE_KEYS {
                let path = dir.join(filename);
                if !path.exists() { continue; }
                let file = File::open(&path).unwrap();
                let mmap = unsafe { Mmap::map(&file).unwrap() };
                let hits = mmap_regex_search(&mmap, &expanded, eff);
                if !hits.is_empty() {
                    println!("\n══ {} ({}) ══", key.to_uppercase(), hits.len());
                    for (ln, line) in hits.iter().take(50) {
                        println!("  L{:>8}: {}", ln, aliases.apply(line));
                    }
                }
            }
            eprintln!("Completed in {:.3}s", t.elapsed().as_secs_f64());
        }
    }
}

fn cmd_section_search(be: &Backend, sec_type: u16, raw_key: &str, pattern: &str, max: usize, aliases: &AliasMap) {
    let t = Instant::now();
    let eff = if max == 0 { usize::MAX } else { max };
    let expanded = aliases.expand_pattern(pattern);

    match be {
        Backend::Db(db) => {
            let hits = db.search_section(sec_type, &expanded, eff);
            println!("{} matching '{}' ({} results):", section_name(sec_type), pattern, hits.len());
            for (ln, line) in &hits {
                println!("  L{:>8}: {}", ln, aliases.apply(line));
            }
        }
        Backend::Raw(dir) => {
            if let Some(path) = resolve_raw_file(dir, raw_key) {
                let file = File::open(&path).unwrap();
                let mmap = unsafe { Mmap::map(&file).unwrap() };
                let hits = mmap_regex_search(&mmap, &expanded, eff);
                println!("{} matching '{}' ({} results):", raw_key.to_uppercase(), pattern, hits.len());
                for (ln, line) in &hits {
                    println!("  L{:>8}: {}", ln, aliases.apply(line));
                }
            } else {
                eprintln!("No {} file found", raw_key);
            }
        }
    }
    eprintln!("Completed in {:.3}s", t.elapsed().as_secs_f64());
}

fn cmd_section_dump(be: &Backend, sec_type: u16, raw_key: &str) {
    match be {
        Backend::Db(db) => {
            let bytes = db.section_bytes(sec_type);
            if !bytes.is_empty() {
                print!("{}", String::from_utf8_lossy(bytes));
            } else {
                eprintln!("Section {} not found", section_name(sec_type));
            }
        }
        Backend::Raw(dir) => {
            if let Some(path) = resolve_raw_file(dir, raw_key) {
                let content = fs::read_to_string(&path).unwrap_or_default();
                print!("{}", content);
            } else {
                eprintln!("No {} file found", raw_key);
            }
        }
    }
}

fn cmd_xrefs(be: &Backend, address: &str, aliases: &AliasMap) {
    let addr_val = parse_addr_val(address);
    let addr_upper = normalize_addr(address);
    let t = Instant::now();

    match be {
        Backend::Db(db) => {
            if let Some(rec) = db.lookup_func_flex(addr_val) {
                let ida = db.read_blob(&rec.xref_ida);
                let binja = db.read_blob(&rec.xref_binja);
                if !ida.is_empty() { println!("──── IDA XREFS ────\n{}", aliases.apply(&ida)); }
                if !binja.is_empty() { println!("──── BINJA XREFS ────\n{}", aliases.apply(&binja)); }
                if ida.is_empty() && binja.is_empty() { println!("No xref data."); }
            } else {
                println!("Function {} not found", aliases.fmt_addr(addr_val));
            }
        }
        Backend::Raw(dir) => {
            if let Some(path) = resolve_raw_file(dir, "xrefs") {
                let file = File::open(&path).unwrap();
                let mmap = unsafe { Mmap::map(&file).unwrap() };
                let hits = mmap_regex_search(&mmap, &addr_upper, 100);
                for (ln, line) in &hits { println!("  L{:>8}: {}", ln, aliases.apply(line)); }
            }
        }
    }
    eprintln!("Completed in {:.3}s", t.elapsed().as_secs_f64());
}

fn cmd_funcdetail(be: &Backend, address: &str, aliases: &AliasMap) {
    let addr_val = parse_addr_val(address);
    let addr_upper = normalize_addr(address);
    let t = Instant::now();

    match be {
        Backend::Db(db) => {
            if let Some(rec) = db.lookup_func_flex(addr_val) {
                let ida = db.read_blob(&rec.detail_ida);
                let binja = db.read_blob(&rec.detail_binja);
                if !ida.is_empty() { println!("──── IDA DETAILS ────\n{}", aliases.apply(&ida)); }
                if !binja.is_empty() { println!("──── BINJA DETAILS ────\n{}", aliases.apply(&binja)); }
                if ida.is_empty() && binja.is_empty() { println!("No details."); }
            } else {
                println!("Function {} not found", aliases.fmt_addr(addr_val));
            }
        }
        Backend::Raw(_dir) => {
            cmd_section_search(be, 0, "funcdetails", &addr_upper, 10, aliases);
        }
    }
    eprintln!("Completed in {:.3}s", t.elapsed().as_secs_f64());
}

fn cmd_il(be: &Backend, address: &str, aliases: &AliasMap) {
    let addr_val = parse_addr_val(address);
    let addr_upper = normalize_addr(address);
    let t = Instant::now();

    match be {
        Backend::Db(db) => {
            if let Some(rec) = db.lookup_func_flex(addr_val) {
                let il = db.read_blob(&rec.il_binja);
                if !il.is_empty() { println!("{}", aliases.apply(&il)); }
                else { println!("No IL data (BINJA only)."); }
            } else {
                println!("Function {} not found", aliases.fmt_addr(addr_val));
            }
        }
        Backend::Raw(dir) => {
            if let Some(path) = resolve_raw_file(dir, "il") {
                let file = File::open(&path).unwrap();
                let mmap = unsafe { Mmap::map(&file).unwrap() };
                if let Some(c) = raw_extract_func(&mmap, &addr_upper, b"// Function: ", b"/---") {
                    println!("{}", c);
                }
            }
        }
    }
    eprintln!("Completed in {:.3}s", t.elapsed().as_secs_f64());
}

fn cmd_info(be: &Backend) {
    match be {
        Backend::Db(db) => {
            println!("╔══════════════════════════════════════════════════════════════════╗");
            println!("║  BINDUMP DATABASE v{}                                           ║", VERSION);
            println!("╚══════════════════════════════════════════════════════════════════╝");
            println!("  Functions:    {}", db.header.func_count);
            println!("  Sections:     {}", db.header.section_count);
            println!("  IDA base:     0x{:X}", db.header.ida_base);
            println!("  BINJA base:   0x{:X}", db.header.binja_base);
            println!("  Index offset: 0x{:X}", db.header.func_index_offset);
            println!("  DB size:      {:.2} GB", db.mmap.len() as f64 / 1_073_741_824.0);

            println!("\n  Sections:");
            for s in &db.sections {
                let sz = if s.size > 1_073_741_824 {
                    format!("{:.1} GB", s.size as f64 / 1_073_741_824.0)
                } else if s.size > 1_048_576 {
                    format!("{:.1} MB", s.size as f64 / 1_048_576.0)
                } else {
                    format!("{:.1} KB", s.size as f64 / 1024.0)
                };
                println!("    {:<20} {:>10}  @0x{:X}", section_name(s.type_id), sz, s.offset);
            }

            let mut both = 0u32;
            let mut ida_only = 0u32;
            let mut binja_only = 0u32;
            for rec in db.iter_funcs() {
                let has_ida = rec.asm_ida.len > 0 || rec.c_ida.len > 0;
                let has_binja = rec.asm_binja.len > 0 || rec.c_binja.len > 0;
                match (has_ida, has_binja) {
                    (true, true) => both += 1,
                    (true, false) => ida_only += 1,
                    (false, true) => binja_only += 1,
                    _ => {}
                }
            }
            println!("\n  Cross-coverage:");
            println!("    Both IDA+BINJA: {}", both);
            println!("    IDA only:       {}", ida_only);
            println!("    BINJA only:     {}", binja_only);
        }
        Backend::Raw(dir) => {
            println!("bindump — Raw file mode");
            println!("Directory: {}\n", dir.display());
            for &(key, filename) in RAW_FILE_KEYS {
                let path = dir.join(filename);
                if path.exists() {
                    let size = fs::metadata(&path).map(|m| m.len()).unwrap_or(0);
                    let sz = if size > 1_073_741_824 {
                        format!("{:.1} GB", size as f64 / 1_073_741_824.0)
                    } else if size > 1_048_576 {
                        format!("{:.1} MB", size as f64 / 1_048_576.0)
                    } else {
                        format!("{:.1} KB", size as f64 / 1024.0)
                    };
                    println!("  {:<16} {:>10}  {}", key, sz, filename);
                }
            }
        }
    }
}

fn cmd_read(be: &Backend, section: &str, start: usize, end: usize) {
    let sec_type = section_type_from_name(section);
    match be {
        Backend::Db(db) => {
            let bytes = if sec_type > 0 { db.section_bytes(sec_type) } else { &[] };
            if bytes.is_empty() { eprintln!("Section '{}' not found", section); return; }
            for (ln, text) in mmap_read_lines(bytes, start, end) {
                println!("{:>8}: {}", ln, text);
            }
        }
        Backend::Raw(dir) => {
            if let Some(path) = resolve_raw_file(dir, section) {
                let file = File::open(&path).unwrap();
                let mmap = unsafe { Mmap::map(&file).unwrap() };
                for (ln, text) in mmap_read_lines(&mmap, start, end) {
                    println!("{:>8}: {}", ln, text);
                }
            } else { eprintln!("Section not found: {}", section); }
        }
    }
}

fn cmd_grep(be: &Backend, section: &str, pattern: &str, max: usize, aliases: &AliasMap) {
    let sec_type = section_type_from_name(section);
    let eff = if max == 0 { usize::MAX } else { max };
    let expanded = aliases.expand_pattern(pattern);
    let t = Instant::now();

    match be {
        Backend::Db(db) => {
            let bytes = if sec_type > 0 { db.section_bytes(sec_type) } else { &[] };
            if bytes.is_empty() { eprintln!("Section '{}' not found", section); return; }
            let hits = mmap_regex_search(bytes, &expanded, eff);
            println!("Grep '{}' in {} ({} matches):", pattern, section, hits.len());
            for (ln, line) in &hits { println!("  L{:>8}: {}", ln, aliases.apply(line)); }
        }
        Backend::Raw(dir) => {
            if let Some(path) = resolve_raw_file(dir, section) {
                let file = File::open(&path).unwrap();
                let mmap = unsafe { Mmap::map(&file).unwrap() };
                let hits = mmap_regex_search(&mmap, &expanded, eff);
                println!("Grep '{}' in {} ({} matches):", pattern, section, hits.len());
                for (ln, line) in &hits { println!("  L{:>8}: {}", ln, aliases.apply(line)); }
            }
        }
    }
    eprintln!("Completed in {:.3}s", t.elapsed().as_secs_f64());
}

fn section_type_from_name(name: &str) -> u16 {
    match name.to_lowercase().as_str() {
        "asm" | "asm_ida" | "assembly" => SEC_ASM_IDA,
        "asm_binja" => SEC_ASM_BINJA,
        "c" | "c_ida" | "decompiled" => SEC_C_IDA,
        "c_binja" => SEC_C_BINJA,
        "il" | "il_binja" | "pipeline" => SEC_IL_BINJA,
        "detail_ida" | "details_ida" | "funcdetails" | "func_details" => SEC_DETAIL_IDA,
        "detail_binja" | "details_binja" => SEC_DETAIL_BINJA,
        "xref_ida" | "xrefs_ida" => SEC_XREF_IDA,
        "xref_binja" | "xrefs_binja" => SEC_XREF_BINJA,
        "strings" | "string" => SEC_STRINGS,
        "strxrefs" | "stringxrefs" | "string_xrefs" => SEC_STRXREFS,
        "names" | "name" | "symbols" => SEC_NAMES,
        "imports" | "import" => SEC_IMPORTS,
        "exports" | "export" => SEC_EXPORTS,
        "segments" | "segment" => SEC_SEGMENTS,
        "overview" | "info" | "triage" => SEC_OVERVIEW,
        "datavars" | "data_vars" | "vars" => SEC_DATAVARS,
        "structures" | "struct" | "structs" => SEC_STRUCTURES,
        "types" | "type" | "typedef" => SEC_TYPES,
        "vtables" | "vtable" => SEC_VTABLES,
        "rtti" => SEC_RTTI,
        "comments" | "comment" => SEC_COMMENTS,
        "callgraph" | "call_graph" | "graph" => SEC_CALLGRAPH,
        "xrefs" => SEC_XREF_IDA,
        _ => 0,
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// BACKEND DETECTION
// ═══════════════════════════════════════════════════════════════════════════════

fn detect_backend(cli: &Cli) -> Backend {
    if let Some(ref p) = cli.db {
        if p.is_file() {
            if let Some(r) = DbReader::open(p) {
                eprintln!("Using database: {} ({:.2} GB)", p.display(), r.mmap.len() as f64 / 1_073_741_824.0);
                return Backend::Db(r);
            }
        }
        if p.is_dir() {
            eprintln!("Using raw dir: {}", p.display());
            return Backend::Raw(p.clone());
        }
    }

    if let Some(ref d) = cli.dir {
        if d.is_dir() {
            eprintln!("Using raw dir: {}", d.display());
            return Backend::Raw(d.clone());
        }
    }

    // Auto-detect
    let cwd = std::env::current_dir().unwrap_or_default();
    for name in &["bindump.bdmp", "../bindump.bdmp"] {
        let p = cwd.join(name);
        if p.is_file() {
            if let Some(r) = DbReader::open(&p) {
                eprintln!("Auto: {} ({:.2} GB)", p.display(), r.mmap.len() as f64 / 1_073_741_824.0);
                return Backend::Db(r);
            }
        }
    }
    for name in &["BINJA_DUMPS", "IDA_DUMPS", "../BINJA_DUMPS", "../IDA_DUMPS"] {
        let p = cwd.join(name);
        if p.is_dir() {
            eprintln!("Auto: raw dir {}", p.display());
            return Backend::Raw(p);
        }
    }

    eprintln!("ERROR: No database or dump directory found.");
    eprintln!("  Use: -p <file.bdmp> or -d <dump_dir>");
    eprintln!("  Or:  bindump build-db --ida <dir> --binja <dir>");
    std::process::exit(1);
}

// ═══════════════════════════════════════════════════════════════════════════════
// MAIN
// ═══════════════════════════════════════════════════════════════════════════════

fn main() {
    let cli = Cli::parse();

    if let Cmd::BuildDb { ref ida, ref binja, ref output, ref offset } = cli.cmd {
        let out_path = output.clone().unwrap_or_else(|| PathBuf::from("bindump.bdmp"));
        let binja_off = if let Some(off) = offset {
            let hex = off.strip_prefix("0x").or_else(|| off.strip_prefix("0X")).unwrap_or(off);
            u64::from_str_radix(hex, 16).expect("Invalid hex offset")
        } else {
            auto_detect_offset(ida, binja)
        };
        if let Err(e) = build_database(ida, binja, &out_path, binja_off) {
            eprintln!("FATAL: {}", e);
            std::process::exit(1);
        }
        return;
    }

    // Load alias map (uses db path or cwd)
    let alias_path = match &cli.db {
        Some(p) if p.is_file() => p.clone(),
        _ => std::env::current_dir().unwrap_or_default().join("bindump.bdmp"),
    };
    let mut aliases = AliasMap::load(&alias_path);

    // Handle alias management commands
    match &cli.cmd {
        Cmd::Rename { address, name } => {
            let addr = parse_addr_val(address);
            let old = aliases.get(addr).map(|s| s.to_string());
            aliases.set(addr, name.clone());
            aliases.save().expect("Failed to save aliases");
            if let Some(old_name) = old {
                println!("Renamed 0x{:X}: '{}' → '{}'", addr, old_name, name);
            } else {
                println!("Named 0x{:X} → '{}'", addr, name);
            }
            return;
        }
        Cmd::Unname { address } => {
            let addr = parse_addr_val(address);
            if aliases.remove(addr) {
                aliases.save().expect("Failed to save aliases");
                println!("Removed alias for 0x{:X}", addr);
            } else {
                println!("No alias found for 0x{:X}", addr);
            }
            return;
        }
        Cmd::Aliases => {
            if aliases.map.is_empty() {
                println!("No aliases defined.");
                println!("Use: bindump rename <address> <name>");
            } else {
                println!("╔══════════════════════════════════════════════════════════════════╗");
                println!("║  FUNCTION ALIASES ({:>5} entries){:>30}║", aliases.map.len(), "");
                println!("╚══════════════════════════════════════════════════════════════════╝");
                for (addr, name) in &aliases.map {
                    println!("  0x{:<14X} {}", addr, name);
                }
                println!("\nStored in: {}", aliases.path.display());
            }
            return;
        }
        _ => {}
    }

    // Commands that operate directly on raw ELF bytes, no DB required.
    if let Cmd::Aob {
        ref address,
        ref name,
        ref bin,
        ref base,
        start_offset,
        min_bytes,
        max_bytes,
        step,
        cpp,
    } = cli.cmd
    {
        let resolved = resolve_addr_or_alias(address, &aliases);
        cmd_aob_generate(
            bin,
            &resolved,
            name.as_deref(),
            base.as_deref(),
            start_offset,
            min_bytes,
            max_bytes,
            step,
            cpp,
        );
        return;
    }

    if let Cmd::AobVerify { ref bin, ref file, only_non_unique } = cli.cmd {
        cmd_aob_verify(bin, file, only_non_unique);
        return;
    }

    if let Cmd::AobBatch {
        ref bin,
        ref file,
        ref base,
        min_bytes,
        max_bytes,
        step,
        start_max,
        only_non_unique,
        cpp,
    } = cli.cmd
    {
        cmd_aob_batch(
            bin,
            file,
            base.as_deref(),
            min_bytes,
            max_bytes,
            step,
            start_max,
            only_non_unique,
            cpp,
        );
        return;
    }

    if let Cmd::AobDbBatch {
        ref bin,
        ref file,
        ref base,
        min_bytes,
        max_bytes,
        step,
        start_max,
        only_non_unique,
        cpp,
    } = cli.cmd
    {
        let db_path = match &cli.db {
            Some(p) => p.clone(),
            None => std::env::current_dir().unwrap_or_default().join("bindump.bdmp"),
        };
        let Some(db_reader) = DbReader::open(&db_path) else {
            eprintln!("AobDbBatch requires a valid .bdmp database. Use -p <path/to/bindump.bdmp>.");
            return;
        };

        cmd_aob_db_batch(
            &db_reader,
            &aliases,
            bin,
            file,
            base.as_deref(),
            min_bytes,
            max_bytes,
            step,
            start_max,
            only_non_unique,
            cpp,
        );
        return;
    }

    if let Cmd::AobPort {
        ref old_bin,
        ref new_bin,
        ref file,
        ref base_old,
        min_bytes,
        max_bytes,
        step,
        start_max,
        only_non_unique,
        cpp,
    } = cli.cmd
    {
        cmd_aob_port(
            old_bin,
            new_bin,
            file,
            base_old.as_deref(),
            min_bytes,
            max_bytes,
            step,
            start_max,
            only_non_unique,
            cpp,
        );
        return;
    }

    if let Cmd::AobPortDb {
        ref old_bin,
        ref new_bin,
        ref file,
        ref base_old,
        min_bytes,
        max_bytes,
        step,
        start_max,
        only_non_unique,
        cpp,
    } = cli.cmd
    {
        let db_path = match &cli.db {
            Some(p) => p.clone(),
            None => std::env::current_dir().unwrap_or_default().join("bindump.bdmp"),
        };
        let Some(db_reader) = DbReader::open(&db_path) else {
            eprintln!("AobPortDb requires a valid .bdmp database. Use -p <path/to/bindump.bdmp>.");
            return;
        };

        cmd_aob_port_db(
            &db_reader,
            &aliases,
            old_bin,
            new_bin,
            file,
            base_old.as_deref(),
            min_bytes,
            max_bytes,
            step,
            start_max,
            only_non_unique,
            cpp,
        );
        return;
    }

    let be = detect_backend(&cli);

    match cli.cmd {
        Cmd::BuildDb { .. } => unreachable!(),
        Cmd::Rename { .. } | Cmd::Unname { .. } | Cmd::Aliases => unreachable!(),
        Cmd::Resolve { ref query } => {
            // Try as address first
            let norm = normalize_addr(query);
            if let Ok(addr) = u64::from_str_radix(&norm, 16) {
                if addr > 0 {
                    if let Some(name) = aliases.get(addr) {
                        println!("0x{:X} → {}", addr, name);
                    } else {
                        println!("0x{:X} has no alias", addr);
                    }
                    return;
                }
            }
            // Try as name
            if let Some(addr) = aliases.resolve_name(query) {
                println!("{} → 0x{:X} (sub_{:X})", query, addr, addr);
            } else {
                println!("'{}' not found as address or alias name", query);
            }
        }
        Cmd::Aob { .. } | Cmd::AobVerify { .. } | Cmd::AobBatch { .. } | Cmd::AobDbBatch { .. } | Cmd::AobPort { .. } | Cmd::AobPortDb { .. } => unreachable!(),
        Cmd::Info => cmd_info(&be),
        Cmd::Func { ref address, extract } => {
            let resolved = resolve_addr_or_alias(address, &aliases);
            cmd_func(&be, &resolved, extract, &aliases);
        }
        Cmd::Search { ref pattern, max } => cmd_search(&be, pattern, max, &aliases),
        Cmd::Strings { ref pattern, max } => cmd_section_search(&be, SEC_STRINGS, "strings", pattern, max, &aliases),
        Cmd::Strxrefs { ref pattern, max } => cmd_section_search(&be, SEC_STRXREFS, "strxrefs", pattern, max, &aliases),
        Cmd::Xrefs { ref address } => {
            let resolved = resolve_addr_or_alias(address, &aliases);
            cmd_xrefs(&be, &resolved, &aliases);
        }
        Cmd::Names { ref pattern, max } => cmd_section_search(&be, SEC_NAMES, "names", pattern, max, &aliases),
        Cmd::Imports { ref pattern, max } => cmd_section_search(&be, SEC_IMPORTS, "imports", pattern, max, &aliases),
        Cmd::Exports { ref pattern, max } => cmd_section_search(&be, SEC_EXPORTS, "exports", pattern, max, &aliases),
        Cmd::Segments => cmd_section_dump(&be, SEC_SEGMENTS, "segments"),
        Cmd::Overview => cmd_section_dump(&be, SEC_OVERVIEW, "overview"),
        Cmd::Types { ref pattern, max } => cmd_section_search(&be, SEC_TYPES, "types", pattern, max, &aliases),
        Cmd::Datavars { ref pattern, max } => cmd_section_search(&be, SEC_DATAVARS, "datavars", pattern, max, &aliases),
        Cmd::Funcdetail { ref address } => {
            let resolved = resolve_addr_or_alias(address, &aliases);
            cmd_funcdetail(&be, &resolved, &aliases);
        }
        Cmd::Il { ref address } => {
            let resolved = resolve_addr_or_alias(address, &aliases);
            cmd_il(&be, &resolved, &aliases);
        }
        Cmd::Comments { ref pattern, max } => cmd_section_search(&be, SEC_COMMENTS, "comments", pattern, max, &aliases),
        Cmd::Structures { ref pattern, max } => cmd_section_search(&be, SEC_STRUCTURES, "structures", pattern, max, &aliases),
        Cmd::Vtables { ref pattern, max } => cmd_section_search(&be, SEC_VTABLES, "vtables", pattern, max, &aliases),
        Cmd::Callgraph { ref pattern, max } => cmd_section_search(&be, SEC_CALLGRAPH, "callgraph", pattern, max, &aliases),
        Cmd::Read { ref section, start, end } => cmd_read(&be, section, start, end),
        Cmd::Grep { ref pattern, ref section, max } => cmd_grep(&be, section, pattern, max, &aliases),
    }
}

/// Resolve an address string that might be an alias name instead of hex
fn resolve_addr_or_alias(s: &str, aliases: &AliasMap) -> String {
    // Try as alias name first
    if let Some(addr) = aliases.resolve_name(s) {
        return format!("0x{:X}", addr);
    }
    s.to_string()
}
