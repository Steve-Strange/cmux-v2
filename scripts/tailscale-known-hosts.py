#!/usr/bin/env python3
"""Emit OpenSSH known_hosts entries from authenticated Tailscale status JSON."""

import base64
import binascii
import ipaddress
import json
import re
import sys


TAILSCALE_IPV4 = ipaddress.ip_network("100.64.0.0/10")
KEY_TYPE = re.compile(r"^[A-Za-z0-9@._+-]+$")


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"error: {message}")


def main() -> None:
    hostname_only = len(sys.argv) == 3 and sys.argv[1] == "--hostname"
    if not (len(sys.argv) == 2 or hostname_only):
        fail("usage: tailscale-known-hosts.py [--hostname] <tailscale-ip>")
    address_arg = sys.argv[2] if hostname_only else sys.argv[1]

    try:
        address = ipaddress.ip_address(address_arg)
    except ValueError:
        fail(f"invalid Tailscale IP address: {address_arg}")
    if address.version != 4 or address not in TAILSCALE_IPV4:
        fail(f"numeric SSH fallback requires a Tailscale IPv4 address: {address}")

    try:
        status = json.load(sys.stdin)
    except (json.JSONDecodeError, UnicodeDecodeError) as error:
        fail(f"invalid tailscale status JSON: {error}")

    peers = status.get("Peer", {})
    if isinstance(peers, dict):
        candidates = list(peers.values())
    elif isinstance(peers, list):
        candidates = peers
    else:
        fail("tailscale status Peer field is not an object or array")
    if isinstance(status.get("Self"), dict):
        candidates.append(status["Self"])

    target = str(address)
    matches = [
        peer
        for peer in candidates
        if isinstance(peer, dict) and target in peer.get("TailscaleIPs", [])
    ]
    if len(matches) != 1:
        fail(f"expected one Tailscale peer for {target}, found {len(matches)}")

    peer = matches[0]
    if hostname_only:
        hostname = peer.get("HostName")
        if not isinstance(hostname, str) or not hostname.rstrip("."):
            fail(f"Tailscale peer {target} advertises no hostname")
        print(hostname.rstrip("."))
        return

    advertised_keys = peer.get("sshHostKeys", [])
    if not isinstance(advertised_keys, list) or not advertised_keys:
        fail(f"Tailscale peer {target} advertises no SSH host keys")

    verified_keys = []
    seen = set()
    for advertised_key in advertised_keys:
        if not isinstance(advertised_key, str):
            fail(f"Tailscale peer {target} has a non-string SSH host key")
        parts = advertised_key.split()
        if len(parts) < 2 or not KEY_TYPE.fullmatch(parts[0]):
            fail(f"Tailscale peer {target} has a malformed SSH host key")
        try:
            decoded_key = base64.b64decode(parts[1], validate=True)
        except (binascii.Error, ValueError):
            fail(f"Tailscale peer {target} has invalid SSH host-key data")
        if not decoded_key:
            fail(f"Tailscale peer {target} has an empty SSH host key")
        key = (parts[0], parts[1])
        if key not in seen:
            seen.add(key)
            verified_keys.append(key)

    for key_type, key_data in verified_keys:
        print(f"{target} {key_type} {key_data}")


if __name__ == "__main__":
    main()
