#!/usr/bin/env python3
"""Recover an Argus controller's per-device FACTORY portal password.

The device derives its own default portal password on first boot as

    SHA-256( salt || MAC )  ->  12 characters

and never stores, logs, or serves the plaintext. This recomputes it, so an
operator can log into a factory-fresh controller without the firmware ever
having disclosed a credential.

The MAC is not secret: it is the last three bytes of the service SSID
(Argus-Service-AABBCC). The SALT is the entire secret, and it lives in the
untracked local sdkconfig as ARGUS_FACTORY_CREDENTIAL_SALT. Do not paste the
salt into a shell in a shared session - pass it via the environment or let
this script read it from sdkconfig.

Usage
-----
  # MAC in full, salt read from the repo's sdkconfig
  python tools/factory_credential.py 10:20:ba:44:89:94

  # From what the SSID shows, if the first three MAC bytes are known
  python tools/factory_credential.py --ssid Argus-Service-448994 --oui 10:20:ba

  # Salt from the environment instead of sdkconfig
  ARGUS_FACTORY_SALT=... python tools/factory_credential.py 10:20:ba:44:89:94
"""

import argparse
import hashlib
import os
import re
import sys

ALPHABET = "23456789ABCDEFGHJKMNPQRSTUVWXYZ"
LENGTH = 12


def derive(mac_bytes: bytes, salt: str) -> str:
    h = hashlib.sha256()
    if salt:
        h.update(salt.encode("utf-8"))
    h.update(mac_bytes)
    digest = h.digest()
    return "".join(ALPHABET[digest[i] % len(ALPHABET)] for i in range(LENGTH))


def parse_mac(text: str) -> bytes:
    cleaned = re.sub(r"[^0-9a-fA-F]", "", text)
    if len(cleaned) != 12:
        raise ValueError(f"expected 6 MAC bytes, got {len(cleaned) // 2}")
    return bytes.fromhex(cleaned)


def salt_from_sdkconfig(path: str):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                if line.startswith("CONFIG_ARGUS_FACTORY_CREDENTIAL_SALT="):
                    return line.split("=", 1)[1].strip().strip('"')
    except OSError:
        return None
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mac", nargs="?", help="full device MAC, any separator")
    parser.add_argument("--ssid", help="service SSID, e.g. Argus-Service-448994")
    parser.add_argument("--oui", help="first three MAC bytes, used with --ssid")
    parser.add_argument("--sdkconfig", default="sdkconfig",
                        help="path to read the salt from (default: ./sdkconfig)")
    args = parser.parse_args()

    if args.mac:
        mac = parse_mac(args.mac)
    elif args.ssid and args.oui:
        suffix = args.ssid.rsplit("-", 1)[-1]
        mac = parse_mac(args.oui + suffix)
    else:
        parser.error("give a full MAC, or --ssid together with --oui")

    salt = os.environ.get("ARGUS_FACTORY_SALT")
    source = "environment"
    if salt is None:
        salt = salt_from_sdkconfig(args.sdkconfig)
        source = args.sdkconfig
    if salt is None:
        salt = ""
        source = "NONE"

    if not salt:
        print("WARNING: no salt found. This password is derivable by anyone "
              "who can see the device's SSID. It is only correct if the "
              "firmware was also built without a salt.", file=sys.stderr)

    print(f"MAC      : {mac.hex(':')}")
    print(f"SSID     : Argus-Service-{mac[3:].hex().upper()}")
    print(f"Salt     : {source}")
    print(f"Password : {derive(mac, salt)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
