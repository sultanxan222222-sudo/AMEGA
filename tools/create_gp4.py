#!/usr/bin/env python3
"""Create a reproducible OpenOrbis GP4 project file for AMEGA."""
from __future__ import annotations

import argparse
import xml.etree.ElementTree as ET
from pathlib import Path


PACKAGE_FILES = (
    "eboot.bin",
    "sce_sys/icon0.png",
    "sce_sys/param.sfo",
    "sce_sys/about/right.sprx",
    "sce_module/libc.prx",
    "sce_module/libSceFios2.prx",
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--template", required=True, type=Path)
    parser.add_argument("--content-id", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    if not args.template.is_file():
        parser.error(f"OpenOrbis GP4 template not found: {args.template}")
    if len(args.content_id) != 36 or not args.content_id.startswith("IV0000-"):
        parser.error("content ID must match the AMEGA IV0000-... format")

    tree = ET.parse(args.template)
    root = tree.getroot()
    volume = root.find("volume")
    if volume is None:
        parser.error("invalid OpenOrbis GP4 template: missing <volume>")
    package = volume.find("package")
    if package is None:
        parser.error("invalid OpenOrbis GP4 template: missing <package>")
    files = root.find("files")
    if files is None:
        parser.error("invalid OpenOrbis GP4 template: missing <files>")
    rootdir = root.find("rootdir")
    if rootdir is None:
        parser.error("invalid OpenOrbis GP4 template: missing <rootdir>")

    package.set("content_id", args.content_id)
    package.set("passcode", "00000000000000000000000000000000")
    package.set("storage_type", "digital50")
    package.set("app_type", "full")

    # GP4 separates the files that are staged into the package from the
    # directory layout exposed by the installed app image.
    for child in list(files):
        files.remove(child)
    for relative_path in PACKAGE_FILES:
        ET.SubElement(
            files,
            "file",
            {"targ_path": relative_path, "orig_path": relative_path},
        )

    for child in list(rootdir):
        rootdir.remove(child)
    sce_sys = ET.SubElement(rootdir, "dir", {"targ_name": "sce_sys"})
    ET.SubElement(sce_sys, "dir", {"targ_name": "about"})
    ET.SubElement(rootdir, "dir", {"targ_name": "sce_module"})

    args.output.parent.mkdir(parents=True, exist_ok=True)
    tree.write(args.output, encoding="utf-8", xml_declaration=True)
    print(f"Created GP4 manifest with {len(PACKAGE_FILES)} payload files: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
