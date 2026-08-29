#!/usr/bin/env python3
"""Fetch RetroAchievements sets for a ROM library and write them beside it.

    tools/ra-sets.py [--roms DIR] [--system NAME] [--force] [--dry-run]

Writes `Roms/<System>/.cheevos/<name>.set` - the same shape as box art's
`.media/<name>.png`, so a set travels with the ROM folder and a card reflash
loses nothing that was not already lost.

HOST-SIDE, and that is a constraint rather than a preference. RetroAchievements
is HTTPS only, the Brick ships no TLS library and no curl, and vendoring one to
fetch a file that never changes would be a large dependency bought for a static
asset. Achievement DETECTION needs no network at all once the set is on the
card; only submitting unlocks back to an account does, and that is blocked on
client registration regardless.

The file is read by two programs and is deliberately one file:

    #! tortos-cheevos 1  game=1459  console=7  title=Blaster Master
    #: 76195  5  Blast Off  Complete the first area
    76195     0xH06f3=0_0xH0400=3_0xH00ba=0_0xH06f0<d0xH06f0

Diatom reads the bare `<id>\\t<condition>` lines and skips every `#` line, which
its format says it will (Diatom ADR-0026). TortOS reads the `#` lines for the
menu. One file means the set being evaluated and the list being shown cannot
drift apart, which two files would eventually do.

CREDENTIALS come from RA_USER and RA_PASS, or --user/--password, or a prompt.
Nothing is written to disk and nothing is stored here.

`r=patch` returns the whole set including things that must not be shown:

  - **Flags 5 is "unofficial"** - achievements in development, not part of the
    set anyone is playing. Skipped.
  - **"Warning: Unknown Emulator"** is injected by RA into every set fetched by
    a client it does not recognize, with condition `1=1.300.` - true after 300
    frames. It is a notice to the developer, not an achievement, and writing it
    would put a fake entry in every game's list that unlocks itself five
    seconds in. Skipped, and counted in the summary so the fact that this
    client is unregistered stays visible rather than being papered over.
"""
import argparse
import getpass
import hashlib
import importlib.util
import json
import os
import sys
import time
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UA = "TortOS/1.0 (+achievement sets)"


def _ra_check():
    """The hash rules live in ra-check.py and are imported, not copied. Two
    tables of per-console header offsets would agree right up until one of them
    was corrected."""
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ra-check.py")
    spec = importlib.util.spec_from_file_location("ra_check", p)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


rac = _ra_check()


def post(**kw):
    body = urllib.parse.urlencode(kw).encode()
    req = urllib.request.Request("https://retroachievements.org/dorequest.php",
                                 data=body, headers={"User-Agent": UA})
    return json.load(urllib.request.urlopen(req, timeout=25))


def login(user, password):
    for verb in ("login2", "login"):
        try:
            d = post(r=verb, u=user, p=password)
            if d.get("Success") and d.get("Token"):
                return d["Token"]
        except Exception as e:
            print(f"  login ({verb}): {e}", file=sys.stderr)
    return None


def patch(user, token, gid, tries=3):
    for k in range(tries):
        try:
            return post(r="patch", u=user, t=token, g=gid)
        except Exception:
            time.sleep(1.5 * (k + 1))
    return None


def tsv_safe(s):
    """A tab or a newline in a title would become a field. RA titles have
    neither today; this is here so the day one does, the file stays readable
    rather than becoming subtly wrong."""
    return " ".join(str(s or "").split())


def write_set(path, game, console, title, achievements):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(f"#! tortos-cheevos 1\tgame={game}\tconsole={console}"
                f"\ttitle={tsv_safe(title)}\n")
        for a in achievements:
            f.write("#:\t{}\t{}\t{}\t{}\n".format(
                a["ID"], a.get("Points", 0),
                tsv_safe(a.get("Title")), tsv_safe(a.get("Description"))))
            f.write("{}\t{}\n".format(a["ID"], a["MemAddr"]))
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roms", default=os.path.join(ROOT, "TortOS-Test-Set", "Roms"))
    ap.add_argument("--system")
    ap.add_argument("--user", default=os.environ.get("RA_USER"))
    ap.add_argument("--password", default=os.environ.get("RA_PASS"))
    ap.add_argument("--force", action="store_true",
                    help="refetch sets that are already on disk")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    user = a.user or input("RetroAchievements user: ").strip()
    pw = a.password or getpass.getpass("password: ")
    token = login(user, pw)
    if not token:
        sys.exit("ra-sets: login failed")
    del pw

    systems = rac.systems_from_cfg(os.path.join(ROOT, "config", "systems.cfg"))
    if a.system:
        systems = [s for s in systems if s[0] == a.system]
        if not systems:
            sys.exit(f"ra-sets: {a.system} is not in systems.cfg")

    wrote = skipped = unknown = empty = failed = 0
    warned = 0
    for folder, tag, exts in systems:
        d = os.path.join(a.roms, folder)
        if not os.path.isdir(d):
            continue
        out_dir = os.path.join(d, ".cheevos")
        n = k = 0
        for fn in sorted(os.listdir(d)):
            p = os.path.join(d, fn)
            if fn.startswith(".") or not os.path.isfile(p):
                continue
            if exts and os.path.splitext(fn)[1][1:].lower() not in exts:
                continue
            data = rac.rom_bytes(p)
            if data is None:
                continue
            n += 1
            out = os.path.join(out_dir, os.path.splitext(fn)[0] + ".set")
            if os.path.exists(out) and not a.force:
                skipped += 1
                k += 1
                continue

            gid = rac.gameid(hashlib.md5(rac.ra_body(tag, data)).hexdigest())
            if gid < 0:
                failed += 1
                print(f"    ASK FAILED: {folder}/{fn}")
                continue
            if gid == 0:
                unknown += 1
                continue

            d_ = patch(user, token, gid)
            pd = (d_ or {}).get("PatchData") or {}
            ach = pd.get("Achievements") or []
            keep = []
            for x in ach:
                if int(x.get("Flags", 3)) != 3:
                    continue
                if str(x.get("Title", "")).startswith("Warning: Unknown Emulator"):
                    warned += 1
                    continue
                if not x.get("MemAddr"):
                    continue
                keep.append(x)
            if not keep:
                empty += 1
                continue

            if not a.dry_run:
                os.makedirs(out_dir, exist_ok=True)
                write_set(out, gid, pd.get("ConsoleID", 0),
                          pd.get("Title", ""), keep)
            wrote += 1
            k += 1
            time.sleep(0.15)          # civility, and it is what keeps this honest
        if n:
            print(f"  {folder:<20} {k:>3}/{n}")

    print(f"\n  wrote {wrote}, already had {skipped}, "
          f"RA does not know {unknown}, no achievements {empty}, "
          f"could not ask {failed}")
    if warned:
        print(f"  {warned} sets carried RA's \"Unknown Emulator\" notice and it was\n"
              f"  dropped from each. That is what an unregistered client is told;\n"
              f"  registering is the fix, not filtering.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
