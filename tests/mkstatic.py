#!/usr/bin/env python3
"""mkstatic.py - build a SYSTEM hive for a machine with a static IPv4 config.

Used by tests/run-tests.sh to cover the "static address" recovery path.

    mkstatic.py OUT.bin
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from mkhive import Hive  # noqa: E402

GUID = "{6B1D2C3A-1111-4C2B-9E8D-0A1B2C3D4E5F}"
CLASS = ("ControlSet001\\Control\\Class\\"
         "{4d36e972-e325-11ce-bfc1-08002be10318}")
NET = ("ControlSet001\\Control\\Network\\"
       "{4d36e972-e325-11ce-bfc1-08002be10318}")


def build(path):
    h = Hive()
    h.set(r"Select", {"Current": ("dword", 1), "Default": ("dword", 1)})
    h.set(r"ControlSet001\Control\ComputerName\ComputerName",
          {"ComputerName": ("sz", "LAB-PC")})
    h.set(NET + "\\" + GUID + r"\Connection",
          {"Name": ("sz", "Ethernet 2")})
    h.set(CLASS + r"\0000",
          {"NetCfgInstanceId": ("sz", GUID),
           "DriverDesc": ("sz", "Intel(R) I219-LM")})
    h.set(r"ControlSet001\Services\Tcpip\Parameters\Interfaces\\" + GUID, {
        "EnableDHCP": ("dword", 0),
        "IPAddress": ("multi", ["192.168.77.10"]),
        "SubnetMask": ("multi", ["255.255.255.0"]),
        "DefaultGateway": ("multi", ["192.168.77.1"]),
        "NameServer": ("sz", "192.168.77.53 8.8.8.8"),
        "Domain": ("sz", "lab.example.com"),
    })
    h.set(r"ControlSet001\Services\Tcpip\Parameters",
          {"Hostname": ("sz", "lab-pc"),
           "Domain": ("sz", "lab.example.com")})
    with open(path, "wb") as f:
        f.write(h.build())


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    build(sys.argv[1])
    print("mkstatic: %s" % sys.argv[1])