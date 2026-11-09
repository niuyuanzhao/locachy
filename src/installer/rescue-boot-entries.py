#!/usr/bin/env python3
"""Derive rescue entries from the packaged archiso kernel and media options."""
from pathlib import Path
import re
import sys


def derive(profile):
    profile = Path(profile)
    entries = profile / "efiboot/loader/entries"
    candidates = []
    for path in sorted(entries.glob("*.conf")):
        text = path.read_text()
        if re.search(r"(?mi)^linux\s+", text):
            candidates.append((path, text))
    if len(candidates) != 1:
        raise ValueError("expected one packaged UEFI Linux entry to derive Rescue from")
    source, text = candidates[0]
    required = {key: list(re.finditer(r"(?mi)^" + key + r"\s+([^\n]+)$", text))
                for key in ("title", "linux", "initrd", "options")}
    if any(len(rows) != 1 for rows in required.values()) or "locachy.mode=" in text:
        raise ValueError("packaged UEFI Linux entry changed; Rescue entry not generated")
    output = entries / "02-locachy-rescue.conf"
    if output.exists():
        raise ValueError("Rescue UEFI destination already exists")
    rescue = re.sub(r"(?mi)^title\s+[^\n]+$", "title   LOCachy Rescue (%ARCH%, UEFI)", text)
    rescue = re.sub(r"(?mi)^(options\s+[^\n]+)$", r"\1 locachy.mode=rescue", rescue)

    config = profile / "syslinux/syslinux-linux.cfg"
    bios = config.read_text()
    blocks = re.split(r"(?mi)(?=^LABEL\s+)", bios)
    choices = [block for block in blocks if re.search(r"(?mi)^LINUX\s+", block)]
    if len(choices) != 1:
        raise ValueError("expected one packaged BIOS Linux entry to derive Rescue from")
    block = choices[0]
    for key in ("LABEL", "MENU LABEL", "LINUX", "INITRD", "APPEND"):
        if len(re.findall(r"(?mi)^" + key + r"\s+[^\n]+$", block)) != 1:
            raise ValueError("packaged BIOS Linux entry changed; Rescue entry not generated")
    if "locachy.mode=" in bios or re.search(r"(?mi)^LABEL\s+locachy-rescue\s*$", bios):
        raise ValueError("Rescue BIOS destination already exists")
    rescue_bios = re.sub(r"(?mi)^LABEL\s+[^\n]+$", "LABEL locachy-rescue", block)
    rescue_bios = re.sub(r"(?mi)^MENU LABEL\s+[^\n]+$", "MENU LABEL LOCachy Rescue (%ARCH%, BIOS)", rescue_bios)
    rescue_bios = re.sub(r"(?mi)^(APPEND\s+[^\n]+)$", r"\1 locachy.mode=rescue", rescue_bios)
    # Validate both templates before changing either destination.
    output.write_text(rescue.rstrip() + "\n")
    config.write_text(bios.rstrip() + "\n\n" + rescue_bios.rstrip() + "\n")


if __name__ == "__main__":
    try:
        if len(sys.argv) != 2:
            raise ValueError("usage: rescue-boot-entries.py ARCHISO_PROFILE")
        derive(sys.argv[1])
    except (OSError, ValueError) as error:
        sys.exit("rescue-boot-entries: " + str(error))
