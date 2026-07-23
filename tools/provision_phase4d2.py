#!/usr/bin/env python3
"""Create ESP-IDF encrypted Phase 4D.2 initial-security artifacts.

Secrets are read with getpass, used only in a restricted temporary directory,
and never passed as command-line arguments or printed. The resulting key and
encrypted NVS images remain sensitive manufacturing artifacts.
"""

from __future__ import annotations

import argparse
import getpass
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

SCHEMA_VERSION = 1
RECORD_VERSION = 1
SLOT_MAGIC = 0x41524753
VALID_MARKER = 0x53454355
ITERATIONS_MIN = 10_000
ITERATIONS_MAX = 500_000
ITERATIONS_DEFAULT = 25_000
PARTITION_SIZE = 0x40000
ROLE_COUNT = 6
ROLE_FORMAT = "<BBBB37sQQ"
AP_FORMAT = "<BBBBI64s"
VERIFIER_FORMAT = "<BBBBI16s32s"


def bounded_ascii(value: str, minimum: int, maximum: int, label: str) -> bytes:
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as exc:
        raise ValueError(f"{label} must be printable ASCII") from exc
    if not minimum <= len(encoded) <= maximum:
        raise ValueError(f"{label} length must be {minimum}..{maximum}")
    if any(byte < 0x20 or byte > 0x7E for byte in encoded):
        raise ValueError(f"{label} must be printable ASCII")
    return encoded


def role_record(index: int, identifier: str, permissions: int) -> bytes:
    nondelegable = sum(1 << bit for bit in (8, 16, 17, 18, 19, 20, 22))
    ident = identifier.encode("ascii") + b"\0"
    ident = ident.ljust(37, b"\0")
    return struct.pack(
        ROLE_FORMAT, RECORD_VERSION, index, 1, int(index == 0), ident,
        permissions, permissions & ~nondelegable,
    )


def ap_record(secret: bytes) -> bytes:
    return struct.pack(
        AP_FORMAT, RECORD_VERSION, 1, len(secret), 0, 1,
        (secret + b"\0").ljust(64, b"\0"),
    )


def verifier_record(password: bytes, iterations: int) -> bytes:
    salt = os.urandom(16)
    verifier = hashlib.pbkdf2_hmac("sha256", password, salt, iterations, 32)
    return struct.pack(VERIFIER_FORMAT, 1, 1, 16, 32, iterations,
                       salt, verifier)


def manifest(factory: bytes, active: bytes, console: bytes,
             iterations: int) -> bytes:
    operational = sum(1 << bit for bit in range(6))
    client_admin = operational | sum(1 << bit for bit in (6, 7, 9, 10, 11))
    roles = (
        role_record(0, "argus_personnel", (1 << 23) - 1) +
        role_record(1, "client_admin", client_admin) +
        role_record(2, "supervisor", operational) +
        role_record(3, "operator", operational) +
        role_record(4, "viewer", 1) +
        role_record(5, "machine_identity", 0)
    )
    header = struct.pack(
        "<HHIBBBBIBBH", SCHEMA_VERSION, 0, 2, ROLE_COUNT, 0, 0, 1,
        1, 2, 0, 0,
    )
    payload = (header + roles + ap_record(factory) + ap_record(active) +
               verifier_record(console, iterations))
    if len(payload) != 562:
        raise RuntimeError(f"schema packing drift: payload={len(payload)}")
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    slot = struct.pack(
        "<IHHIII", SLOT_MAGIC, SCHEMA_VERSION, len(payload), 1, crc,
        VALID_MARKER,
    ) + payload
    if len(slot) != 582:
        raise RuntimeError(f"schema packing drift: slot={len(slot)}")
    return slot


def generator_path() -> Path:
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        candidate = Path(r"C:\esp\v5.5.3\esp-idf")
        if candidate.is_dir():
            idf_path = str(candidate)
    if not idf_path:
        raise RuntimeError("ESP-IDF environment is not active")
    path = (Path(idf_path) / "components" / "nvs_flash" /
            "nvs_partition_generator" / "nvs_partition_gen.py")
    if not path.is_file():
        raise RuntimeError("ESP-IDF NVS generator was not found")
    return path


def run_generator(arguments: list[str]) -> None:
    completed = subprocess.run(
        [sys.executable, str(generator_path()), *arguments],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True,
    )
    if completed.returncode != 0:
        raise RuntimeError("ESP-IDF NVS generator failed without publishing secrets")


def build_artifacts(output_dir: Path, environment: str, factory: bytes,
                    active: bytes, console: bytes, iterations: int,
                    replace: bool) -> None:
    store_output = output_dir / "sec_store.bin"
    keys_output = output_dir / "sec_keys.bin"
    if (store_output.exists() or keys_output.exists()) and not replace:
        raise RuntimeError("provisioning artifacts already exist; explicit replacement required")
    if environment == "production" and replace:
        confirmation = getpass.getpass(
            "Type REPLACE PRODUCTION SECURITY ARTIFACTS (hidden): ")
        if confirmation != "REPLACE PRODUCTION SECURITY ARTIFACTS":
            raise RuntimeError("production replacement was not confirmed")

    output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="argus-sec-") as temporary:
        temp = Path(temporary)
        try:
            os.chmod(temp, 0o700)
        except OSError:
            pass
        slot = manifest(factory, active, console, iterations)
        csv_path = temp / "security.csv"
        csv_path.write_text(
            "key,type,encoding,value\n"
            "argus_sec,namespace,,\n"
            f"manifest_a,data,hex2bin,{slot.hex()}\n"
            "manifest_sel,data,u8,0\n",
            encoding="ascii",
        )
        try:
            os.chmod(csv_path, 0o600)
        except OSError:
            pass
        plain = temp / "plain.bin"
        encrypted = temp / "encrypted.bin"
        keys = temp / "keys.bin"
        decrypted = temp / "decrypted.bin"
        run_generator(["generate", str(csv_path), str(plain),
                       hex(PARTITION_SIZE)])
        run_generator(["encrypt", "--keygen", "--keyfile", str(keys),
                       str(csv_path), str(encrypted), hex(PARTITION_SIZE)])
        run_generator(["decrypt", str(encrypted), str(keys),
                       str(decrypted)])
        if plain.read_bytes() != decrypted.read_bytes():
            raise RuntimeError("encrypted provisioning readback verification failed")
        shutil.copyfile(encrypted, store_output)
        shutil.copyfile(keys, keys_output)
    print(
        f"PROVISIONED environment={environment} schema={SCHEMA_VERSION} "
        f"iterations={iterations} encrypted=true readback_verified=true"
    )
    print("Artifacts are sensitive, ignored, and must remain under manufacturing control.")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--environment", choices=("development", "production"),
                        default="development")
    parser.add_argument("--output-dir", type=Path,
                        default=Path(".local/provisioning"))
    parser.add_argument("--iterations", type=int, default=ITERATIONS_DEFAULT)
    parser.add_argument("--initialize", action="store_true")
    parser.add_argument("--replace", action="store_true")
    parser.add_argument("--synthetic-test", action="store_true")
    args = parser.parse_args()
    if not ITERATIONS_MIN <= args.iterations <= ITERATIONS_MAX:
        parser.error(f"iterations must be {ITERATIONS_MIN}..{ITERATIONS_MAX}")
    if not args.initialize and not args.synthetic_test:
        parser.error("--initialize is required")
    try:
        if args.synthetic_test:
            factory_text = "synthetic-factory-ap"
            active_text = "synthetic-active-ap"
            console_text = "synthetic-console-password"
        else:
            factory_text = getpass.getpass("Factory AP credential (hidden): ")
            active_text = getpass.getpass("Active AP credential (hidden): ")
            console_text = getpass.getpass("Argus console password (hidden): ")
        factory = bytearray(bounded_ascii(factory_text, 8, 63, "factory AP"))
        active = bytearray(bounded_ascii(active_text, 8, 63, "active AP"))
        console = bytearray(bounded_ascii(console_text, 1, 64, "console password"))
        if args.synthetic_test:
            with tempfile.TemporaryDirectory(prefix="argus-sec-test-") as temp:
                build_artifacts(Path(temp), args.environment, bytes(factory),
                                bytes(active), bytes(console), args.iterations,
                                False)
        else:
            build_artifacts(args.output_dir, args.environment, bytes(factory),
                            bytes(active), bytes(console), args.iterations,
                            args.replace)
        return 0
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"PROVISIONING REJECTED: {exc}", file=sys.stderr)
        return 1
    finally:
        for name in ("factory", "active", "console"):
            value = locals().get(name)
            if isinstance(value, bytearray):
                value[:] = b"\0" * len(value)


if __name__ == "__main__":
    raise SystemExit(main())
