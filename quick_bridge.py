import socket, json, time, sys

def send(cmd, code=None):
    s = socket.socket()
    s.settimeout(5)
    s.connect(('127.0.0.1', 19420))
    if code:
        payload = json.dumps({'cmd': cmd, 'code': code})
    else:
        payload = json.dumps({'cmd': cmd})
    s.sendall(payload.encode() + b'\n')
    time.sleep(0.5)
    r = s.recv(8192)
    s.close()
    return r.decode()

if len(sys.argv) > 1:
    cmd = sys.argv[1]
    code = sys.argv[2] if len(sys.argv) > 2 else None
    print(send(cmd, code))
else:
    print(send('ping'))
