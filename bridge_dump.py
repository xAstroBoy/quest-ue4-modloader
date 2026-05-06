import socket, json, time

def bridge(code):
    s = socket.socket()
    s.settimeout(15)
    s.connect(('127.0.0.1', 19420))
    s.sendall(json.dumps({'cmd': 'exec_lua', 'code': code}).encode() + b'\n')
    time.sleep(3)
    s.settimeout(5)
    data = b''
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk: break
            data += chunk
    except: pass
    s.close()
    try: return json.loads(data)
    except: return {'raw': data.decode()}

# Step 1: dump all SDK fields on one entry
code1 = r"""
local entries = FindAllOf("PFXCollectibleEntry_Arm") or {}
if #entries == 0 then return "NO ENTRIES" end
local e = entries[1]
local fields = {
  "CollectibleEntryID","CollectibleActorClass","LocalizedName",
  "CollectibleIP","ConditionText","bCanGrab","bIsUnique",
  "bIsHiddenByDefault","bOverrideScale","CustomInventoryRotation",
  "CustomInventoryOffset","HandInventoryScaleOverride",
  "CustomInventoryScaleOverride","GrabScaleOverride",
  "SlotPlacementSound","UnlockData","CategoryData"
}
local out = {}
for _, f in ipairs(fields) do
  local ok, v = pcall(function() return e:Get(f) end)
  if ok then
    table.insert(out, f .. "=" .. tostring(v))
  else
    table.insert(out, f .. "=ERR:" .. tostring(v))
  end
end
return table.concat(out, "\n")
"""

print("=== FIELD DUMP: PFXCollectibleEntry_Arm[1] ===")
r = bridge(code1)
if r.get('ok'):
    print(r['result'])
else:
    print("ERROR:", r)

# Step 2: find entries where UnlockData or CategoryData is nil
code2 = r"""
local classes = {
  "PFXCollectibleEntry_Arm","PFXCollectibleEntry_BallSkin",
  "PFXCollectibleEntry_BallTrail","PFXCollectibleEntry_Cabinet",
  "PFXCollectibleEntry_Statue","PFXCollectibleEntry_Gadget",
  "PFXCollectibleEntry_Poster"
}
local nil_ud = 0
local nil_cd = 0
local nil_both = 0
local total = 0
for _, cls in ipairs(classes) do
  local entries = FindAllOf(cls) or {}
  for _, e in ipairs(entries) do
    total = total + 1
    local ud = nil
    local cd = nil
    pcall(function() ud = e:Get("UnlockData") end)
    pcall(function() cd = e:Get("CategoryData") end)
    if not ud then nil_ud = nil_ud + 1 end
    if not cd then nil_cd = nil_cd + 1 end
    if not ud and not cd then nil_both = nil_both + 1 end
  end
end
return string.format("total=%d nil_UnlockData=%d nil_CategoryData=%d nil_both=%d", total, nil_ud, nil_cd, nil_both)
"""

print("\n=== NIL FIELD STATS ===")
r2 = bridge(code2)
if r2.get('ok'):
    print(r2['result'])
else:
    print("ERROR:", r2)

# Step 3: on an entry with nil CategoryData, dump its GetClass() and raw GetFields if available
code3 = r"""
local classes = {
  "PFXCollectibleEntry_BallSkin","PFXCollectibleEntry_BallTrail",
  "PFXCollectibleEntry_Cabinet"
}
local out = {}
for _, cls in ipairs(classes) do
  local entries = FindAllOf(cls) or {}
  for _, e in ipairs(entries) do
    local cd = nil
    pcall(function() cd = e:Get("CategoryData") end)
    if not cd then
      -- This entry has nil CategoryData - dump what we CAN read
      local ud = nil
      pcall(function() ud = e:Get("UnlockData") end)
      local name = tostring(e)
      local ud_class = "nil"
      if ud then
        local ok, uc = pcall(function() return ud:GetClass():GetName() end)
        if ok then ud_class = uc end
      end
      table.insert(out, string.format("cls=%s name=%s ud=%s(%s) cd=nil", cls, name, tostring(ud), ud_class))
      if #out >= 5 then break end
    end
  end
  if #out >= 5 then break end
end
if #out == 0 then return "ALL entries have CategoryData" end
return table.concat(out, "\n")
"""

print("\n=== ENTRIES WITH nil CategoryData ===")
r3 = bridge(code3)
if r3.get('ok'):
    print(r3['result'])
else:
    print("ERROR:", r3)

# Step 4: Call GetCategoryData() function (not Get property) - maybe it's computed
code4 = r"""
local entries = FindAllOf("PFXCollectibleEntry_BallSkin") or {}
if #entries == 0 then return "no ballskin entries" end
local out = {}
for i = 1, math.min(5, #entries) do
  local e = entries[i]
  -- Try the function call version
  local fn_cd = nil
  local prop_cd = nil
  pcall(function() fn_cd = e:Call("GetCategoryData") end)
  pcall(function() prop_cd = e:Get("CategoryData") end)
  table.insert(out, string.format("[%d] GetCategoryData()=%s  Get(CategoryData)=%s", i, tostring(fn_cd), tostring(prop_cd)))
end
return table.concat(out, "\n")
"""

print("\n=== GetCategoryData() vs Get(CategoryData) ===")
r4 = bridge(code4)
if r4.get('ok'):
    print(r4['result'])
else:
    print("ERROR:", r4)
