#!/usr/bin/env python3
# ABOUTME: Turn a KEY=VALUE Wi-Fi env file (SSID, PSK) into the TT7's wpa_supplicant.conf, written to stdout.
# ABOUTME: Used by scripts/wifi-setup.sh; the PSK never appears in argv, only in the file and on stdout.
"""Quoting rules, from wpa_supplicant 2.10's parser (src/utils/common.c
wpa_config_parse_string, src/utils/config.c wpa_config_get_line):
  - "..." values run to the LAST double quote and take backslashes literally,
    but the comment stripper pairs quotes to decide whether a '#' starts a
    comment, so an embedded '"' can turn a later '#' into a comment.
  - So: SSID is written quoted when it is printable ASCII without '"',
    otherwise as unquoted hex (ssid=<hex>, which wpa_supplicant accepts).
  - A passphrase (8..63 printable ASCII) is written quoted when it has no '"',
    otherwise as the derived 256-bit PSK in hex: PBKDF2-HMAC-SHA1(passphrase,
    ssid, 4096, 32), exactly what wpa_supplicant computes from the passphrase.
  - A 64-hex-digit PSK is taken as the raw key and written unquoted.

Env file: one KEY=VALUE per line; blank lines and '#' lines are skipped; an
optional leading 'export ' is allowed; one pair of matching surrounding quotes
('...' or "...") is removed; nothing else is unescaped. Keys: SSID, PSK.
"""
import hashlib
import re
import sys

KEYS = ("SSID", "PSK")
HEX64 = re.compile(r"[0-9a-fA-F]{64}")


class ConfError(ValueError):
    pass


def parse_env(text):
    values = {}
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.rstrip("\r")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if line.startswith("export "):
            line = line[len("export "):]
        key, sep, value = line.partition("=")
        key = key.strip()
        if not sep or key not in KEYS:
            # Don't quote the line back: a malformed line may be the passphrase.
            raise ConfError(f"line {n}: expected SSID=... or PSK=...")
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "'\"":
            value = value[1:-1]
        values[key] = value
    missing = [k for k in KEYS if k not in values]
    if missing:
        raise ConfError(f"missing {', '.join(missing)}")
    return values["SSID"], values["PSK"]


def printable_ascii(s):
    return all(0x20 <= ord(c) <= 0x7E for c in s)


def pmk(passphrase, ssid_bytes):
    """The WPA2-PSK key wpa_supplicant derives from a passphrase (IEEE 802.11i H.4)."""
    return hashlib.pbkdf2_hmac("sha1", passphrase.encode("ascii"), ssid_bytes, 4096, 32)


def network_lines(ssid, psk):
    """The ssid= and psk= lines for one network (validated and quoted safely)."""
    ssid_bytes = ssid.encode("utf-8")
    if not 1 <= len(ssid_bytes) <= 32:
        raise ConfError(f"SSID must be 1..32 bytes (got {len(ssid_bytes)})")
    if printable_ascii(ssid) and '"' not in ssid:
        ssid_line = f'ssid="{ssid}"'
    else:
        ssid_line = f"ssid={ssid_bytes.hex()}"

    if HEX64.fullmatch(psk):
        psk_line = f"psk={psk.lower()}"
    elif not 8 <= len(psk) <= 63:
        raise ConfError(f"PSK must be an 8..63 character passphrase or 64 hex digits (got {len(psk)} characters)")
    elif not printable_ascii(psk):
        raise ConfError("PSK passphrase may only contain printable ASCII (WPA2 rule)")
    elif '"' in psk:
        psk_line = f"psk={pmk(psk, ssid_bytes).hex()}"
    else:
        psk_line = f'psk="{psk}"'
    return ssid_line, psk_line


def make_conf(networks):
    """networks: [(ssid, psk), ...] in preference order. With more than one,
    each block gets a priority (the first listed is highest), so the panel
    joins the preferred network when several are in range."""
    if not networks:
        raise ConfError("no networks")
    seen = set()
    blocks = []
    for i, (ssid, psk) in enumerate(networks):
        if ssid in seen:
            raise ConfError(f"SSID listed twice (network {i + 1})")
        seen.add(ssid)
        ssid_line, psk_line = network_lines(ssid, psk)
        priority = f"\tpriority={len(networks) - i}\n" if len(networks) > 1 else ""
        blocks.append("network={\n"
                      f"\t{ssid_line}\n"
                      "\tscan_ssid=1\n"
                      "\tkey_mgmt=WPA-PSK\n"
                      f"\t{psk_line}\n"
                      f"{priority}"
                      "}\n")
    return ("ctrl_interface=/var/run/wpa_supplicant\n"
            "update_config=0\n"
            "\n" + "\n".join(blocks))


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print("usage: wifi_conf.py <env-file>...   (one file per network, preferred first;"
              " writes wpa_supplicant.conf to stdout)", file=sys.stderr)
        sys.exit(2)
    networks = []
    path = None
    try:
        for path in sys.argv[1:]:
            with open(path, encoding="utf-8") as f:
                networks.append(parse_env(f.read()))
        sys.stdout.write(make_conf(networks))
    except (OSError, UnicodeDecodeError, ConfError) as e:
        # Never include the PSK in an error.
        print(f"wifi_conf: {path}: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
