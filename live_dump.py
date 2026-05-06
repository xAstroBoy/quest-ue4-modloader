#!/usr/bin/env python3
"""
Live runtime dump of PFXCollectibleEntry fields via bridge.
Waits for game to be ready, then dumps field layout and nil stats.
"""

import socket
import json
import time

def bridge(code, timeout=30):
    s = socket.socket()
    s.settimeout(timeout)
    s.connect(('127.0.0.1', 19420))
    s.sendall(json.dumps({'cmd': 'exec_lua', 'code': code}).encode() + b'\n')
    data = b''
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
            if b'\n' in data:
                break
    except socket.timeout:
        pass
    s.close()
    line = data.split(b'\n')[0].strip()
    if not line:
        return None
    try:
        return json.loads(line)
    except Exception as e:
        return {'raw': data.decode(errors='replace')}

def ping():
    s = socket.socket()
    s.settimeout(5)
    try:
        s.connect(('127.0.0.1', 19420))
        s.sendall(json.dumps({'cmd': 'ping'}).encode() + b'\n')
        data = b''
        while True:
            chunk = s.recv(1024)
            if not chunk: break
            data += chunk
            if b'\n' in data: break
        s.close()
        return b'pong' in data
    except:
        return False

print("=== Waiting for bridge ===")
for i in range(60):
    if ping():
        print(f"  Bridge alive after {i+1}s")
        break
    time.sleep(1)
else:
    print("  Bridge never responded — is game running?")
    exit(1)

# Wait for PlayerController to exist (game in hub)
print("  Waiting for PlayerController (hub loaded)...")
for i in range(120):
    r = bridge(r'local pc = FindFirstOf("PlayerController"); return tostring(pc)')
    result = r.get('result', '') if r else ''
    if result and result != 'nil' and result != '':
        print(f"  PlayerController found after {i+1}s: {result}")
        break
    time.sleep(1)
else:
    print("  Timeout waiting for hub — running anyway")

time.sleep(2)

# ── STEP 1: Dump all fields on a PFXCollectibleEntry_Arm ──────────────────────
print("\n=== STEP 1: Field dump on PFXCollectibleEntry_Arm[1] ===")
r = bridge(r"""
local entries = FindAllOf("PFXCollectibleEntry_Arm_C")
if not entries or #entries == 0 then return "NO ENTRIES FOUND" end
local e = entries[1]
local out = {}
out[#out+1] = "Entry: " .. tostring(e)
out[#out+1] = "Class: " .. tostring(e:GetClass():GetName())

-- Try to get all properties via reflection
local props = e:GetClass():GetProperties()
if props then
    for i = 1, #props do
        local p = props[i]
        local name = p:GetName()
        local ok, val = pcall(function() return e:Get(name) end)
        local vstr = ok and tostring(val) or ("ERR:"..tostring(val))
        out[#out+1] = string.format("  [%d] %s = %s", i, name, vstr)
    end
else
    out[#out+1] = "  GetProperties() returned nil"
end
return table.concat(out, "\n")
""")
print(r)

# ── STEP 2: Direct offset inspection ─────────────────────────────────────────
print("\n=== STEP 2: Raw pointer at UnlockData (+0x108) and CategoryData (+0x118) ===")
r = bridge(r"""
local entries = FindAllOf("PFXCollectibleEntry_Arm_C")
if not entries or #entries == 0 then return "NO ENTRIES" end
local e = entries[1]
local out = {}
out[#out+1] = "Entry addr: " .. string.format("0x%x", e:GetAddress())
-- Read raw 8-byte pointers at the two offsets
local ok1, p1 = pcall(function() return ReadU64(e:GetAddress() + 0x108) end)
local ok2, p2 = pcall(function() return ReadU64(e:GetAddress() + 0x118) end)
out[#out+1] = string.format("  +0x108 (UnlockData) raw ptr  = %s", ok1 and string.format("0x%x", p1) or ("ERR:"..tostring(p1)))
out[#out+1] = string.format("  +0x118 (CategoryData) raw ptr = %s", ok2 and string.format("0x%x", p2) or ("ERR:"..tostring(p2)))
-- What does Get() return?
local ok3, ud = pcall(function() return e:Get("UnlockData") end)
local ok4, cd = pcall(function() return e:Get("CategoryData") end)
out[#out+1] = string.format("  Get('UnlockData')  = %s", ok3 and tostring(ud) or ("ERR:"..tostring(ud)))
out[#out+1] = string.format("  Get('CategoryData')= %s", ok4 and tostring(cd) or ("ERR:"..tostring(cd)))
-- If CategoryData is not nil, dump its class
if ok4 and cd then
    local ok5, cn = pcall(function() return cd:GetClass():GetName() end)
    out[#out+1] = string.format("  CategoryData class = %s", ok5 and cn or ("ERR:"..tostring(cn)))
end
return table.concat(out, "\n")
""")
print(r)

# ── STEP 3: Nil stats across all entry classes ────────────────────────────────
print("\n=== STEP 3: Nil stats for UnlockData/CategoryData across entry classes ===")
r = bridge(r"""
local classes = {
    "PFXCollectibleEntry_Arm_C",
    "PFXCollectibleEntry_Avatar_C",
    "PFXCollectibleEntry_Ball_C",
    "PFXCollectibleEntry_Table_C",
    "PFXCollectibleEntry_Flipper_C",
    "PFXCollectibleEntry_Gadget_C",
    "PFXCollectibleEntry_Poster_C",
}
local out = {}
for _, cn in ipairs(classes) do
    local ok, all = pcall(function() return FindAllOf(cn) end)
    if not ok or not all then
        out[#out+1] = string.format("%s: FindAllOf error", cn)
    else
        local total = #all
        local ud_nil = 0
        local cd_nil = 0
        for i = 1, total do
            local e = all[i]
            local ok1, ud = pcall(function() return e:Get("UnlockData") end)
            local ok2, cd = pcall(function() return e:Get("CategoryData") end)
            if not ok1 or not ud then ud_nil = ud_nil + 1 end
            if not ok2 or not cd then cd_nil = cd_nil + 1 end
        end
        out[#out+1] = string.format("%s: total=%d  UnlockData_nil=%d  CategoryData_nil=%d", cn, total, ud_nil, cd_nil)
    end
end
return table.concat(out, "\n")
""")
print(r)

# ── STEP 4: On entries with nil CategoryData, check raw ptr ──────────────────
print("\n=== STEP 4: Raw CategoryData ptr for nil-Get entries (first 5) ===")
r = bridge(r"""
local out = {}
local classes = {
    "PFXCollectibleEntry_Arm_C",
    "PFXCollectibleEntry_Avatar_C",
    "PFXCollectibleEntry_Ball_C",
}
local count = 0
for _, cn in ipairs(classes) do
    local ok, all = pcall(function() return FindAllOf(cn) end)
    if ok and all then
        for i = 1, #all do
            if count >= 5 then break end
            local e = all[i]
            local ok2, cd = pcall(function() return e:Get("CategoryData") end)
            if not ok2 or not cd then
                local base = e:GetAddress()
                local ok3, raw = pcall(function() return ReadU64(base + 0x118) end)
                local ok4, ud = pcall(function() return e:Get("UnlockData") end)
                out[#out+1] = string.format(
                    "[%s #%d] base=0x%x  +0x118 raw=0x%s  UnlockData=%s",
                    cn, i, base,
                    ok3 and string.format("%x", raw) or "ERR",
                    tostring(ok4 and ud or nil)
                )
                count = count + 1
            end
        end
    end
end
if count == 0 then return "ALL entries have valid CategoryData - nil from Get() is suspicious" end
return table.concat(out, "\n")
""")
print(r)

# ── STEP 5: Compare Get vs Call for CategoryData on one entry ─────────────────
print("\n=== STEP 5: Get('CategoryData') vs Call('GetCategoryData') ===")
r = bridge(r"""
local entries = FindAllOf("PFXCollectibleEntry_Arm_C")
if not entries or #entries == 0 then return "NO ENTRIES" end
local e = entries[1]
local out = {}
local ok1, gv = pcall(function() return e:Get("CategoryData") end)
out[#out+1] = "Get('CategoryData')         = " .. tostring(ok1 and gv or ("ERR:"..tostring(gv)))
local ok2, cv = pcall(function() return e:Call("GetCategoryData") end)
out[#out+1] = "Call('GetCategoryData')      = " .. tostring(ok2 and cv or ("ERR:"..tostring(cv)))
-- Also check GetClass name list for the entry
local ok3, cls = pcall(function()
    local c = e:GetClass()
    local names = {}
    while c do
        names[#names+1] = c:GetName()
        c = c:GetSuperClass()
    end
    return table.concat(names, " -> ")
end)
out[#out+1] = "Class hierarchy: " .. (ok3 and cls or ("ERR:"..tostring(cls)))
return table.concat(out, "\n")
""")
print(r)

print("\n=== Done ===")
