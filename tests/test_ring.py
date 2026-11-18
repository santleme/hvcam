#!/usr/bin/env python3
"""test_ring — a grabbed frame must stay intact while later grabs
land in other ring slots (multi-thread consumers read a frame while
the capture loop keeps pulling). Serves the real wire protocol over
TCP loopback and checks payload survival across grabs.

Exit 77 = skipped (numpy missing), so ctest can mark it skipped.
"""
import importlib.util
import os
import socket
import struct
import subprocess
import sys
import threading

try:
    import numpy as np
except ImportError:
    sys.exit(77)

sys.path.insert(0, __file__.rsplit("/", 2)[0] + "/bindings/python")
import hvcam  # noqa: E402

MAGIC = 0x4856434D
REQ = 0x51
W, H, FMT, BPP = 4, 2, 2, 16  # yuy2, 16-byte payload


def serve_once(conn, seq):
    hdr = struct.pack("<IHHQQHHII", MAGIC, 1, 1, seq, 1000000 + seq,
                      W, H, FMT, BPP)
    conn.sendall(hdr + bytes([seq & 0xFF]) * BPP)


def server(lsock, n, ready):
    conn, _ = lsock.accept()
    ready.set()
    seq = 1
    try:
        while seq <= n:
            if conn.recv(1) != bytes([REQ]):
                break
            serve_once(conn, seq)
            seq += 1
    finally:
        conn.close()


def main():
    lsock = socket.socket()
    lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    lsock.bind(("127.0.0.1", 0))
    lsock.listen(1)
    port = lsock.getsockname()[1]
    ready = threading.Event()
    t = threading.Thread(target=server, args=(lsock, 6, ready), daemon=True)
    t.start()

    cam = hvcam.Camera(tcp=f"127.0.0.1:{port}")
    ready.wait(5)

    _, a1 = cam.grab()
    _, a2 = cam.grab()
    _, a3 = cam.grab()
    # ring=3 default: a1 must still hold seq=1 after two more grabs —
    # the pre-ring code overwrote it already at grab #2.
    assert (a1 == 1).all(), f"frame 1 torn: {a1[:4]}"
    assert (a2 == 2).all(), f"frame 2 torn: {a2[:4]}"
    # zero-copy is kept: the array is a view, not a fresh buffer.
    assert a1.base is not None

    _, a4 = cam.grab()
    # grab #4 reuses slot 0 — a1 may now be recycled. The guarantee
    # is ring-1 = 2 subsequent grabs: a2 and a3 must be intact.
    assert (a2 == 2).all(), f"frame 2 torn early: {a2[:4]}"
    assert (a3 == 3).all(), f"frame 3 torn early: {a3[:4]}"
    _, a5 = cam.grab()
    assert (a3 == 3).all(), f"frame 3 torn late: {a3[:4]}"
    assert (a4 == 4).all(), f"frame 4 torn late: {a4[:4]}"

    # Corrupt MJPEG must surface as ValueError on BOTH decode paths —
    # PyTurboJPEG raises OSError internally and must be translated.
    # to_bgr() needs cv2 for every format (flip/cvtColor), so decoder
    # coverage is gated on the optional dependency rather than
    # failing the ring test when only numpy is installed.
    if importlib.util.find_spec("cv2") is not None:
        bad = bytes([0xFF]) * 64
        try:
            cam.to_bgr({"width": W, "height": H, "format": 4, "flags": 1},
                       np.frombuffer(bad, dtype=np.uint8))
        except ValueError:
            pass
        else:
            raise AssertionError("corrupt MJPEG did not raise ValueError")

    cam.close()
    lsock.close()
    t.join(5)


def check_hvcam_lib_pin():
    # HVCAM_LIB is an authoritative pin: pointing it at a missing
    # library must fail the import even though a valid libhvcam.so
    # sits beside the binding — a silent fallback would let CI test
    # a stale build while believing it tested the requested one.
    env = dict(os.environ)
    env["HVCAM_LIB"] = "/nonexistent/dir/libhvcam.so"
    env["PYTHONPATH"] = os.path.dirname(hvcam.__file__)
    r = subprocess.run([sys.executable, "-c", "import hvcam"],
                       env=env, capture_output=True)
    assert r.returncode != 0, \
        "bad HVCAM_LIB pin silently fell back to the default search"

    print("test_ring: OK")


if __name__ == "__main__":
    main()
    check_hvcam_lib_pin()
