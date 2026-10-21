"""Task 12 E2E aid: per-image UDP isolation on loopback.

Spawns four copies of python.exe under DISTINCT image names (clumsy targets
by image, so two plain python.exe processes could never be told apart):

    pair 1 (target this):  appA.exe  <--> appB.exe
    pair 2 (control):      appC.exe  <--> appD.exe

Every worker sends numbered UDP datagrams to its peer and counts gaps on
receive. Run while clumsy is affecting appA at the chosen loss rate.
Pair 1 must show heavy loss; control pair 2 must stay lossless, proving
unowned traffic is never touched (the security iron rule).

Usage (non-elevated is fine):  python scripts/e2e_udp_pair.py [seconds]
Results print at the end and are written to build/e2e_udp_pair.log.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import time

WORKER = textwrap.dedent(r"""
    import socket, sys, time, json, os
    tag, bind_port, peer_port, seconds = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", bind_port))
    peer = ("127.0.0.1", peer_port)
    s.setblocking(False)
    time.sleep(0.5)  # warm-up: let all four workers bind before anyone sends
    end = time.time() + seconds
    sent = 0
    next_seq = 0
    received = 0
    gaps = 0
    last_send = 0.0
    while time.time() < end:
        now = time.time()
        if now - last_send >= 0.02:  # 50 packets/s
            s.sendto(b"%s:%d" % (tag.encode(), sent), peer)
            sent += 1
            last_send = now
        try:
            data, _ = s.recvfrom(1500)
            seq = int(data.split(b":", 1)[1])
            if seq > next_seq:
                gaps += seq - next_seq
            next_seq = max(next_seq, seq + 1)
            received += 1
        except BlockingIOError:
            pass
        except Exception:
            pass
        time.sleep(0.002)
    print(json.dumps({"tag": tag, "sent": sent, "received": received, "gaps": gaps}))
""")

# (image exe, worker tag, bind port, peer port)
NODES = [
    ("appA.exe", "A", 19101, 19102),  # pair 1 — clumsy targets appA.exe
    ("appB.exe", "B", 19102, 19101),
    ("appC.exe", "C", 19103, 19104),  # control pair, never targeted
    ("appD.exe", "D", 19104, 19103),
]


def main() -> int:
    seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    py = sys.executable
    tmp = tempfile.mkdtemp(prefix="clumsy-e2e-")
    script = os.path.join(tmp, "worker.py")
    with open(script, "w", encoding="utf-8") as f:
        f.write(WORKER)
    env = os.environ.copy()
    home = os.path.dirname(py)
    env["PYTHONHOME"] = home
    env["PYTHONPATH"] = os.path.join(home, "Lib")

    procs = []
    for exe_name, tag, bind_port, peer_port in NODES:
        exe_path = os.path.join(tmp, exe_name)
        shutil.copyfile(py, exe_path)
        p = subprocess.Popen(
            [exe_path, script, tag, str(bind_port), str(peer_port), str(seconds)],
            cwd=tmp, env=env, stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        procs.append((tag, p))

    print(f"four workers running for {seconds}s; in clumsy pick appA.exe, "
          f"direction all, loss 95%, press Start")
    results = []
    for tag, p in procs:
        out, _ = p.communicate(timeout=seconds + 15)
        line = out.strip().splitlines()[-1] if out.strip() else "{}"
        print(line)
        results.append(line)

    os.remove(script)
    for exe_name, *_ in NODES:
        try:
            os.remove(os.path.join(tmp, exe_name))
        except OSError:
            pass
    os.rmdir(tmp)

    import json
    by_tag = {json.loads(r)["tag"]: json.loads(r) for r in results if r.strip("{}")}
    log = os.path.join("build", "e2e_udp_pair.log")
    os.makedirs("build", exist_ok=True)
    with open(log, "w", encoding="utf-8") as f:
        f.write("\n".join(results) + "\n")
    a, b, c, d = (by_tag.get(x, {"gaps": -1, "received": 0}) for x in "ABCD")
    pair1_loss = a["gaps"] + b["gaps"]
    pair2_loss = c["gaps"] + d["gaps"]
    verdict = "PASS" if pair1_loss > 0 and pair2_loss == 0 else "CHECK"
    print(f"pair1 (targeted) gaps={pair1_loss}  pair2 (control) gaps={pair2_loss}  -> {verdict}")
    print(f"log: {log}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
