import socket, json

def bridge(code, timeout=20):
    s = socket.socket()
    s.settimeout(timeout)
    s.connect(('127.0.0.1', 19420))
    s.sendall(json.dumps({'cmd': 'exec_lua', 'code': code}).encode() + b'\n')
    data = b''
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk: break
            data += chunk
            if b'\n' in data: break
    except: pass
    s.close()
    line = data.split(b'\n')[0].strip()
    try: return json.loads(line)
    except: return {'raw': data.decode(errors='replace')}

code = r"""
local out = {}
local patterns = {"PFXCollectibleEntry_Arm_C", "PFXCollectibleSlotComponent", "AC_CollectibleSlot_C"}
for _, p in ipairs(patterns) do
    local ok, res = pcall(function()
        local arr = FindAllOf(p)
        return arr and #arr or 0
    end)
    out[#out+1] = p .. ": " .. (ok and tostring(res) or "ERR")
end
local pc = FindFirstOf("PlayerController")
out[#out+1] = "PlayerController: " .. tostring(pc)
local gm = FindFirstOf("GameMode")
out[#out+1] = "GameMode: " .. tostring(gm)
return table.concat(out, "\n")
"""

r = bridge(code)
print(r)
