#!/usr/bin/env python3
import json, os, socket, struct, subprocess, sys, tempfile, threading
from pathlib import Path

binary = Path(sys.argv[1])
with tempfile.TemporaryDirectory() as td:
    root = Path(td)
    unix_path = root / 'configd.sock'
    pid_path = root / 'configd.pid'
    pid_path.write_text(str(os.getpid()) + '\n')

    unix_ready = threading.Event()
    def unix_server():
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(str(unix_path)); server.listen(1); unix_ready.set()
        conn, _ = server.accept()
        data = b''
        while b'\n' not in data:
            part = conn.recv(4096)
            if not part: break
            data += part
        request = json.loads(data.decode())
        assert request['type'] == 'session'
        reply = {'type':'session','data':{'username':'root','role':'administrator','capabilities':['status.read']}}
        conn.sendall((json.dumps(reply) + '\n').encode())
        conn.close(); server.close()

    tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp.bind(('127.0.0.1', 0)); tcp.listen(1)
    port = tcp.getsockname()[1]
    def ws_server():
        conn, _ = tcp.accept()
        headers = b''
        while b'\r\n\r\n' not in headers:
            headers += conn.recv(1)
        assert b'Sec-WebSocket-Protocol: configd-ws' in headers
        conn.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Protocol: configd-ws\r\n\r\n')
        first = conn.recv(2)
        assert first[0] & 0x0f == 1
        length = first[1] & 0x7f
        mask = conn.recv(4)
        payload = bytes(b ^ mask[i & 3] for i, b in enumerate(conn.recv(length)))
        message = json.loads(payload.decode())
        assert message == {'id':'health','type':'hello'}
        reply = json.dumps({'id':'health','type':'hello','data':{'service':'configd','protocol':2,'websocket_protocol':'configd-ws'}}).encode()
        if len(reply) < 126:
            frame = bytes([0x81, len(reply)]) + reply
        else:
            frame = bytes([0x81, 126]) + struct.pack('!H', len(reply)) + reply
        conn.sendall(frame)
        conn.close(); tcp.close()

    tu = threading.Thread(target=unix_server, daemon=True); tu.start(); unix_ready.wait()
    tw = threading.Thread(target=ws_server, daemon=True); tw.start()
    env = os.environ.copy()
    env['POSTMERKOS_CONFIGD_SOCKET'] = str(unix_path)
    env['POSTMERKOS_CONFIGD_PID'] = str(pid_path)
    env['POSTMERKOS_WEBSOCKET_PORT'] = str(port)
    result = subprocess.run([str(binary), 'management-health'], env=env, text=True, capture_output=True)
    if result.returncode != 0:
        print(result.stdout); print(result.stderr, file=sys.stderr)
        raise SystemExit('management-health rejected a valid local and WebSocket path')
    for marker in ('configd process:            PASS', 'local management socket:   PASS',
                   'local session request:       PASS', 'WebSocket configd-ws hello:  PASS'):
        assert marker in result.stdout, (marker, result.stdout)
    tu.join(2); tw.join(2)
print('configd management-health protocol test passed')
