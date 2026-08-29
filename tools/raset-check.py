#!/usr/bin/env python3
"""Do the C and Python set converters agree, on real RetroAchievements data?

    RA_USER=... RA_PASS=... make check-raset

Fetches a sample of patch responses, converts each one both ways, and requires
the two files to be byte identical. Needs credentials and a network; skips
cleanly without them, because `make` should not fail on a plane.

Two converters exist for the same reason two hashers do: one has to run on the
device and one is the host bulk tool. This is what keeps the duplication from
becoming a drift - and the failure it guards against is silent, since a set
that is subtly wrong still loads and just never fires.

Nothing fetched here is written into the repository. RetroAchievements' data is
theirs; this holds it in a temp directory for the length of the run.
"""
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "build-native", "raset-check")
UA = "TortOS/1.0 (+converter check)"

# A spread rather than a favorite: a small set, two large ones, and one whose
# conditions are long enough to have broken a fixed buffer.
GAMES = [(1459, "Blaster Master"), (355, "Zelda: A Link to the Past"),
         (10003, "Super Metroid"), (11278, "Mega Man 2"),
         (1447, "Castlevania"), (4646, "Metroid Fusion")]


def post(**kw):
    body = urllib.parse.urlencode(kw).encode()
    req = urllib.request.Request("https://retroachievements.org/dorequest.php",
                                 data=body, headers={"User-Agent": UA})
    return json.load(urllib.request.urlopen(req, timeout=30))


def main():
    user, pw = os.environ.get("RA_USER"), os.environ.get("RA_PASS")
    if not user or not pw:
        print("  skip: set RA_USER and RA_PASS to run this one")
        return 0
    ras = importlib.util.spec_from_file_location(
        "ra_sets", os.path.join(ROOT, "tools", "ra-sets.py"))
    m = importlib.util.module_from_spec(ras)
    ras.loader.exec_module(m)

    tok = post(r="login2", u=user, p=pw).get("Token")
    if not tok:
        print("  login failed")
        return 1

    tmp = tempfile.mkdtemp(prefix="raset-check-")
    bad = 0
    for gid, name in GAMES:
        d = post(r="patch", u=user, t=tok, g=gid)
        raw = os.path.join(tmp, f"{gid}.json")
        with open(raw, "w", encoding="utf-8") as f:
            json.dump(d, f, ensure_ascii=False)

        pd = d.get("PatchData", {})
        keep = [a for a in (pd.get("Achievements") or [])
                if int(a.get("Flags", 3)) == 3 and a.get("MemAddr")
                and not str(a.get("Title", "")).startswith("Warning: Unknown Emulator")]
        py = os.path.join(tmp, f"{gid}.py.set")
        m.write_set(py, gid, pd.get("ConsoleID", 0), pd.get("Title", ""), keep)

        c = os.path.join(tmp, f"{gid}.c.set")
        r = subprocess.run([BIN, str(gid), raw, c], capture_output=True, text=True)
        if r.returncode != 0:
            print(f"    {name}: C converter failed: {r.stderr.strip()}")
            bad += 1
            continue

        a, b = open(py, "rb").read(), open(c, "rb").read()
        if a != b:
            bad += 1
            print(f"    MISMATCH {name} ({len(keep)} achievements)")
            la, lb = a.decode().splitlines(), b.decode().splitlines()
            for i, (x, y) in enumerate(zip(la, lb)):
                if x != y:
                    print(f"      line {i+1}\n        python {x[:100]}\n        c      {y[:100]}")
                    break
            if len(la) != len(lb):
                print(f"      python {len(la)} lines, c {len(lb)}")
        else:
            print(f"  {name:<28} {len(keep):>3} achievements, identical")

    print(f"\n  {len(GAMES)} sets, {bad} mismatched")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
