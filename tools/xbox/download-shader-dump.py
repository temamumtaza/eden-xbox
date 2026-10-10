#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Download one selected Eden D3D12 shader's IR/SPIR-V files from Xbox LocalState."""

from __future__ import annotations

import argparse
import datetime
import importlib.util
import os
import pathlib
import re
import sys
import urllib.parse


def load_portal_helper():
    helper_path = pathlib.Path(__file__).with_name("eden-xbox-macos.py")
    spec = importlib.util.spec_from_file_location("eden_xbox_macos_helper", helper_path)
    if spec is None or spec.loader is None:
        raise RuntimeError("Could not load the verified Xbox Device Portal helper.")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def download(shader_hash: str, portal: str, output: pathlib.Path) -> list[pathlib.Path]:
    if not re.fullmatch(r"[0-9a-fA-F]{16}", shader_hash):
        raise ValueError("Shader hash must be exactly 16 hexadecimal digits.")

    helper = load_portal_helper()
    client = helper.require_portal_client(portal)
    item = helper.app_package(helper.installed_packages(client))
    if not item:
        raise helper.PortalError("Eden is not listed as installed on this Xbox.")
    package_name = item.get("PackageFullName")
    if not isinstance(package_name, str) or not package_name:
        raise helper.PortalError("Device Portal did not return Eden's package full name.")

    capture = output / (
        f"shader-{shader_hash.lower()}-"
        f"{datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')}"
    )
    capture.mkdir(mode=0o700, parents=True, exist_ok=False)
    downloaded: list[pathlib.Path] = []
    for stage in range(5):
        for suffix in ("ir.txt", "spv"):
            filename = f"shader_{shader_hash.lower()}_{stage}.{suffix}"
            query = urllib.parse.urlencode(
                {
                    "knownfolderid": "LocalAppData",
                    "packagefullname": package_name,
                    "path": r"\LocalState\eden\log",
                    "filename": filename,
                }
            )
            try:
                _, content = client.download(
                    "/api/filesystem/apps/file?" + query, max_bytes=32 * 1024 * 1024
                )
            except helper.PortalError as exc:
                if "HTTP 404" in str(exc):
                    continue
                raise
            destination = capture / filename
            destination.write_bytes(content)
            os.chmod(destination, 0o600)
            downloaded.append(destination)

    if not downloaded:
        capture.rmdir()
        raise helper.PortalError(
            "No dump files found for this hash. Confirm Eden ran with dump_shader=<hash> and built a pipeline using it."
        )
    return downloaded


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Download Eden IR/SPIR-V files for one shader hash from Xbox LocalState."
    )
    parser.add_argument(
        "--portal",
        required=True,
        help="Xbox Device Portal origin, for example https://192.168.1.20:11443",
    )
    parser.add_argument(
        "--shader-hash", required=True, help="16-digit VS or PS hash from a D3D12 pipeline log"
    )
    parser.add_argument(
        "--output-dir",
        type=pathlib.Path,
        default=pathlib.Path.home() / "Downloads" / "EdenXboxShaderDumps",
    )
    args = parser.parse_args()
    try:
        paths = download(args.shader_hash, args.portal, args.output_dir)
    except ValueError as exc:
        parser.error(str(exc))
    except (RuntimeError, OSError) as exc:
        print(f"Shader dump download failed: {exc}", file=sys.stderr)
        return 2
    print("Downloaded selected shader diagnostics (file contents were not printed):")
    for path in paths:
        print(f"  {path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
