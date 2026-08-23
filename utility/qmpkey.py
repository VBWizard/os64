import socket, json, sys, time
# tokens: down:KEY up:KEY  btn:down btn:up  (left button)  wait:MS
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); f = s.makefile('rw')
f.readline(); f.write(json.dumps({"execute":"qmp_capabilities"})+"\n"); f.flush(); f.readline()
for tok in sys.argv[2:]:
    kind, arg = tok.split(':', 1)
    if kind == 'wait': time.sleep(int(arg)/1000); continue
    if kind == 'btn': ev = {"type":"btn","data":{"down": arg=="down","button":"left"}}
    else: ev = {"type":"key","data":{"down": kind=="down","key":{"type":"qcode","data":arg}}}
    f.write(json.dumps({"execute":"input-send-event","arguments":{"events":[ev]}})+"\n"); f.flush(); f.readline()
    time.sleep(0.05)
