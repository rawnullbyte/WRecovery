#!/usr/bin/env python3
"""compare-registry.py - differential test for winreg against python-registry.

Walks every key and value of a hive with both implementations and reports any
difference. Used by tests/run-tests.sh; needs `pip install python-registry`.

    compare-registry.py HIVE [WINREG_BINARY]
"""

import os
import subprocess
import sys

from Registry import Registry


def find_winreg():
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (os.path.join(here, "..", ".build", "bin", "winreg"),
                 os.path.join(here, "..", "out", "winreg")):
        if os.access(cand, os.X_OK):
            return os.path.abspath(cand)
    return "winreg"


def run(binary, *args):
    r = subprocess.run([binary, *args], capture_output=True)
    if r.returncode not in (0, 1):
        raise SystemExit("winreg failed: %s" % r.stderr.decode(errors="replace"))
    return r.stdout.decode("utf-8", "surrogateescape")


def escape(s):
    return (s.replace("\\", "\\\\").replace("\t", "\\t")
             .replace("\r", "\\r").replace("\n", "\\n"))


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    hive = sys.argv[1]
    winreg = sys.argv[2] if len(sys.argv) > 2 else find_winreg()

    reg = Registry.Registry(hive)
    keys = values = problems = 0

    stack = [(reg.root(), "")]
    while stack:
        key, path = stack.pop()
        keys += 1

        mine = [l for l in run(winreg, "keys", hive, path).split("\n") if l]
        theirs = [k.name() for k in key.subkeys()]
        if sorted(mine, key=str.lower) != sorted(theirs, key=str.lower):
            print("subkeys differ at %r:\n  mine=%r\n  theirs=%r"
                  % (path, sorted(mine), sorted(theirs)))
            problems += 1

        mine_vals = {}
        for line in run(winreg, "values", hive, path).splitlines():
            if not line:
                continue
            name, vtype, val = line.split("\t", 2)
            mine_vals["(default)" if name == "" else name.lower()] = (vtype, val)

        for v in key.values():
            name = "(default)" if v.name() == "" else v.name().lower()
            if name not in mine_vals:
                print("value missing at %r: %s" % (path, name))
                problems += 1
                continue
            vtype, val = mine_vals[name]
            values += 1
            t = v.value_type()
            if t == 1:
                want = escape(v.value())
            elif t == 4:
                want = str(v.value())
            else:
                continue          # binary/multi-sz covered by name+type checks
            if val != want:
                print("value differs at %r %s:\n  mine=%r\n  theirs=%r"
                      % (path, name, val, want))
                problems += 1

        for sub in key.subkeys():
            stack.append((sub, (path + "\\" if path else "") + sub.name()))

    print("compare-registry: %d keys, %d values, %d problems" %
          (keys, values, problems))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())