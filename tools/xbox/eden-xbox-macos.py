#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify, deploy, launch, and collect diagnostics for Eden Xbox from macOS."""

from __future__ import annotations

import argparse
import base64
import getpass
import hashlib
import http.client
import http.cookies
import ipaddress
import json
import os
import pathlib
import re
import socket
import ssl
import sys
import tempfile
import time
import urllib.parse
import uuid
import xml.etree.ElementTree as ET
import zipfile


APP_IDENTITY = "EdenEmuProject.EdenXbox"
APP_PUBLISHER = "CN=EdenXboxDev"
MAX_JSON_BYTES = 16 * 1024 * 1024
MAX_LOG_BYTES = 96 * 1024 * 1024
LOGS = (
    ("eden_uwp_diag.txt", ""),
    ("eden_log.txt", "eden/log"),
    ("eden_graphics_bugs.log", "eden/log"),
)


class PortalError(RuntimeError):
    pass


class PinnedHTTPSConnection(http.client.HTTPSConnection):
    def __init__(self, host: str, port: int, *, expected_sha256: str, **kwargs):
        super().__init__(host, port, **kwargs)
        self.expected_sha256 = expected_sha256

    def connect(self) -> None:
        super().connect()
        try:
            certificate_der = self.sock.getpeercert(binary_form=True)
            actual = hashlib.sha256(certificate_der).hexdigest()
            if actual != self.expected_sha256:
                raise PortalError("The Xbox TLS certificate changed after pinning; no request was sent.")
            with tempfile.NamedTemporaryFile(suffix=".pem") as certificate_file:
                certificate_file.write(ssl.DER_cert_to_PEM_cert(certificate_der).encode("ascii"))
                certificate_file.flush()
                certificate = ssl._ssl._test_decode_cert(certificate_file.name)
            expected_ip = ipaddress.ip_address(self.host)
            sans = certificate.get("subjectAltName", ())
            if not any(
                kind == "IP Address" and ipaddress.ip_address(value) == expected_ip
                for kind, value in sans
            ):
                raise PortalError("The pinned Xbox certificate does not contain this IP address in its SAN.")
            now = time.time()
            if (
                now < ssl.cert_time_to_seconds(certificate["notBefore"])
                or now > ssl.cert_time_to_seconds(certificate["notAfter"])
            ):
                raise PortalError("The pinned Xbox TLS certificate is outside its validity period.")
        except PortalError:
            self.close()
            raise
        except (OSError, ssl.SSLError, ValueError, KeyError, TypeError) as exc:
            self.close()
            raise PortalError("The pinned Xbox TLS certificate could not be validated.") from exc


def parse_portal(raw: str) -> tuple[str, int, str]:
    parsed = urllib.parse.urlsplit(raw)
    if parsed.scheme.lower() != "https" or not parsed.hostname:
        raise PortalError("Device Portal must use an https:// address.")
    if parsed.username is not None or parsed.password is not None:
        raise PortalError("Do not put credentials in the portal URL.")
    if parsed.path not in ("", "/") or parsed.query or parsed.fragment:
        raise PortalError("Provide only the portal origin, without a path, query, or fragment.")
    try:
        port = parsed.port or 11443
    except ValueError as exc:
        raise PortalError("The portal URL has an invalid port.") from exc
    if port != 11443:
        raise PortalError("Xbox Device Portal must use port 11443.")
    host = parsed.hostname
    try:
        address = ipaddress.ip_address(host)
    except ValueError as exc:
        raise PortalError("Use the Xbox's private LAN IP address; public hostnames are refused.") from exc
    if not (address.is_private or address.is_link_local):
        raise PortalError("Device Portal connections are restricted to private or link-local IP addresses.")
    authority = f"[{host}]:{port}" if ":" in host else f"{host}:{port}"
    return host, port, f"https://{authority}"


def trust_path(host: str, port: int) -> pathlib.Path:
    config = pathlib.Path.home() / "Library" / "Application Support" / "Eden Xbox"
    token = hashlib.sha256(f"{host}:{port}".encode("utf-8")).hexdigest()[:20]
    return config / f"device-portal-{token}.pem"


def file_sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fetch_leaf_after_pin(host: str, port: int, expected_leaf: str) -> bytes:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    try:
        raw = socket.create_connection((host, port), timeout=15)
        with context.wrap_socket(raw, server_hostname=host) as connection:
            leaf = connection.getpeercert(binary_form=True)
            actual = hashlib.sha256(leaf).hexdigest()
            if actual != expected_leaf:
                raise PortalError(
                    "Portal certificate fingerprint does not match the value you supplied; no certificate was trusted. "
                    f"Presented SHA-256: {actual}"
                )
            return ssl.DER_cert_to_PEM_cert(leaf).encode("ascii")
    except (ValueError, ssl.SSLError) as exc:
        raise PortalError("The pinned Device Portal certificate could not be saved.") from exc
    except (OSError, ssl.SSLError, http.client.HTTPException) as exc:
        raise PortalError("Could not establish the pinned TLS connection to Device Portal.") from exc


class PortalClient:
    def __init__(self, host: str, port: int, base: str, ca_file: pathlib.Path):
        if not ca_file.is_file():
            raise PortalError("This portal has no locally pinned CA. Run the trust command first.")
        self.host = host
        self.port = port
        self.base = base
        try:
            pem = ca_file.read_text(encoding="ascii")
            der = ssl.PEM_cert_to_DER_cert(pem)
            self.expected_sha256 = hashlib.sha256(der).hexdigest()
        except (OSError, UnicodeError, ValueError, ssl.SSLError) as exc:
            raise PortalError("The locally pinned Device Portal certificate could not be loaded.") from exc
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        self.context.check_hostname = False
        self.context.verify_mode = ssl.CERT_NONE
        self.cookies: dict[str, str] = {}
        self.username: str | None = None
        self.password: str | None = None
        self.auth_prompted = False

    def _headers(self, extra: dict[str, str] | None = None, mutation: bool = False) -> dict[str, str]:
        headers = {"Accept": "application/json, application/octet-stream, */*", "Connection": "close"}
        if self.cookies:
            headers["Cookie"] = "; ".join(f"{key}={value}" for key, value in self.cookies.items())
        if mutation:
            token = self.cookies.get("CSRF-Token")
            if not token:
                raise PortalError("The portal did not issue its CSRF session cookie; the request was not sent.")
            headers["X-CSRF-Token"] = token
        if self.username is not None and self.password is not None:
            raw = f"{self.username}:{self.password}".encode("utf-8")
            headers["Authorization"] = "Basic " + base64.b64encode(raw).decode("ascii")
        if extra:
            headers.update(extra)
        return headers

    def _remember_cookies(self, response: http.client.HTTPResponse) -> None:
        for line in response.headers.get_all("Set-Cookie", []):
            cookie = http.cookies.SimpleCookie()
            try:
                cookie.load(line)
            except http.cookies.CookieError:
                continue
            for name, morsel in cookie.items():
                self.cookies[name] = morsel.value

    def _prompt_credentials(self) -> None:
        if self.auth_prompted:
            raise PortalError("Device Portal rejected the supplied credentials.")
        self.auth_prompted = True
        self.username = input("Device Portal username: ").strip()
        if not self.username:
            raise PortalError("A username is required by this portal.")
        self.password = getpass.getpass("Device Portal password (input hidden): ")

    def request(
        self,
        method: str,
        target: str,
        body: bytes | None = None,
        headers: dict[str, str] | None = None,
        *,
        mutation: bool = False,
        max_bytes: int = MAX_JSON_BYTES,
        allow_status: tuple[int, ...] = (),
    ) -> tuple[int, bytes]:
        if not target.startswith("/") or target.startswith("//"):
            raise PortalError("Refusing a request outside this Device Portal host.")
        attempts = 0
        while True:
            connection = PinnedHTTPSConnection(
                self.host, self.port, expected_sha256=self.expected_sha256,
                context=self.context, timeout=300
            )
            try:
                connection.request(method, target, body=body, headers=self._headers(headers, mutation))
                response = connection.getresponse()
                self._remember_cookies(response)
                if response.status == 401 and method == "GET" and attempts == 0:
                    response.read(1024)
                    connection.close()
                    self._prompt_credentials()
                    attempts += 1
                    continue
                if response.status >= 400 and response.status not in allow_status:
                    response.read(1024 * 1024)
                    raise PortalError(f"Device Portal returned HTTP {response.status}; response details were withheld.")
                payload = response.read(max_bytes + 1)
                if len(payload) > max_bytes:
                    raise PortalError("Device Portal response exceeded the local safety limit.")
                return response.status, payload
            except PortalError:
                raise
            except (OSError, ssl.SSLError, http.client.HTTPException) as exc:
                raise PortalError("The verified Device Portal request failed.") from exc
            finally:
                connection.close()

    def json(self, method: str, target: str, body: bytes | None = None, mutation: bool = False) -> dict:
        status, payload = self.request(
            method,
            target,
            body,
            {"Content-Type": "application/json"} if body is not None else None,
            mutation=mutation,
        )
        if not payload:
            return {"_http_status": status}
        try:
            parsed = json.loads(payload.decode("utf-8-sig"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise PortalError("Device Portal returned invalid JSON.") from exc
        if not isinstance(parsed, dict):
            raise PortalError("Device Portal returned an unexpected JSON value.")
        parsed["_http_status"] = status
        return parsed

    def ensure_session(self) -> None:
        self.json("GET", "/api/app/packagemanager/packages")
        if "CSRF-Token" not in self.cookies:
            raise PortalError("The Device Portal did not provide its CSRF cookie; no state-changing request was sent.")

    def send_install(self, artifact: dict[str, pathlib.Path]) -> int:
        names = ["eden-xbox.appx", "Microsoft.VCLibs.x64.14.00.appx", "eden-xbox.cer"]
        boundary = uuid.uuid4().hex
        chunks: list[tuple[bytes, pathlib.Path | None]] = []
        total = 0
        for name in names:
            path = artifact[name]
            content_type = "application/x-x509-ca-cert" if name.endswith(".cer") else "application/octet-stream"
            head = (
                f"--{boundary}\r\n"
                f'Content-Disposition: form-data; name="{name}"; filename="{name}"\r\n'
                f"Content-Type: {content_type}\r\n\r\n"
            ).encode("ascii")
            tail = b"\r\n"
            chunks.append((head, None))
            chunks.append((b"", path))
            chunks.append((tail, None))
            total += len(head) + path.stat().st_size + len(tail)
        ending = f"--{boundary}--\r\n".encode("ascii")
        total += len(ending)
        query = urllib.parse.urlencode({"package": "eden-xbox.appx"})
        target = "/api/app/packagemanager/package?" + query
        connection = PinnedHTTPSConnection(
            self.host, self.port, expected_sha256=self.expected_sha256,
            context=self.context, timeout=900
        )
        try:
            connection.putrequest("POST", target)
            headers = self._headers(
                {
                    "Content-Type": f"multipart/form-data; boundary={boundary}",
                    "Content-Length": str(total),
                },
                mutation=True,
            )
            for key, value in headers.items():
                connection.putheader(key, value)
            connection.endheaders()
            for data, path in chunks:
                if data:
                    connection.send(data)
                if path is not None:
                    with path.open("rb") as stream:
                        for block in iter(lambda: stream.read(1024 * 1024), b""):
                            connection.send(block)
            connection.send(ending)
            response = connection.getresponse()
            self._remember_cookies(response)
            if response.status not in (200, 201, 202):
                response.read(1024 * 1024)
                raise PortalError(
                    f"Package deployment request returned HTTP {response.status}; response details were withheld. "
                    "The install request was not retried."
                )
            response.read(1024 * 1024)
            return response.status
        except PortalError:
            raise
        except (OSError, ssl.SSLError, http.client.HTTPException) as exc:
            raise PortalError("The package upload failed; it was not automatically retried.") from exc
        finally:
            connection.close()

    def download(self, target: str, max_bytes: int = MAX_LOG_BYTES) -> tuple[int, bytes]:
        return self.request("GET", target, max_bytes=max_bytes, allow_status=(404,))


def require_portal_client(raw: str) -> PortalClient:
    host, port, base = parse_portal(raw)
    return PortalClient(host, port, base, trust_path(host, port))


def presented_fingerprint(host: str, port: int) -> str:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    try:
        raw = socket.create_connection((host, port), timeout=15)
        with context.wrap_socket(raw, server_hostname=host) as connection:
            return hashlib.sha256(connection.getpeercert(binary_form=True)).hexdigest()
    except (OSError, ssl.SSLError) as exc:
        raise PortalError("Could not read the Device Portal certificate fingerprint.") from exc


def trust_portal(raw: str, fingerprint: str | None, replace: bool) -> None:
    host, port, base = parse_portal(raw)
    if fingerprint is None:
        actual = presented_fingerprint(host, port)
        print(f"Device Portal leaf certificate SHA-256: {actual}")
        fingerprint = input("Type this fingerprint to pin this Xbox on first use: ").strip()
    expected = re.sub(r"[^0-9a-f]", "", fingerprint.lower())
    if not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise PortalError("Enter the portal leaf certificate's full SHA-256 fingerprint (64 hex digits).")
    pem = fetch_leaf_after_pin(host, port, expected)
    destination = trust_path(host, port)
    destination.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(destination.parent, 0o700)
    if destination.exists() and destination.read_bytes() != pem and not replace:
        raise PortalError("A different CA is already pinned for this portal. Use --replace-trust only after checking it.")
    temporary = destination.with_name(destination.name + f".{uuid.uuid4().hex}.tmp")
    try:
        temporary.write_bytes(pem)
        os.chmod(temporary, 0o600)
        client = PortalClient(host, port, base, temporary)
        client.json("GET", "/api/app/packagemanager/packages")
        os.replace(temporary, destination)
        os.chmod(destination, 0o600)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
    print("Device Portal TLS certificate and IP address verified; the certificate is pinned in your macOS user config.")


def version_tuple(text: str) -> tuple[int, int, int, int]:
    parts = text.split(".")
    if len(parts) != 4 or any(not part.isdigit() for part in parts):
        raise PortalError("The APPX has an invalid four-part version.")
    return tuple(int(part) for part in parts)  # type: ignore[return-value]


def verify_artifact(directory: pathlib.Path) -> tuple[dict[str, pathlib.Path], dict, dict]:
    if not directory.is_dir():
        raise PortalError("Artifact directory does not exist.")
    names = (
        "eden-xbox.appx",
        "eden-xbox.cer",
        "Microsoft.VCLibs.x64.14.00.appx",
        "build-metadata.json",
        "SHA256SUMS.txt",
    )
    files = {name: directory / name for name in names}
    missing = [name for name, path in files.items() if not path.is_file()]
    if missing:
        raise PortalError("Artifact is missing required files: " + ", ".join(missing))
    checksums: dict[str, str] = {}
    for line in files["SHA256SUMS.txt"].read_text(encoding="ascii").splitlines():
        match = re.fullmatch(r"([0-9a-fA-F]{64})\s+\*?(.+)", line.strip())
        if not match:
            raise PortalError("SHA256SUMS.txt contains a malformed row.")
        name = pathlib.PurePosixPath(match.group(2)).name
        if name in checksums:
            raise PortalError("SHA256SUMS.txt repeats a file name.")
        checksums[name] = match.group(1).lower()
    required_hashed = set(names) - {"SHA256SUMS.txt"}
    if set(checksums) != required_hashed:
        raise PortalError("SHA256SUMS.txt does not list exactly the install files and build metadata.")
    for name, expected in checksums.items():
        actual = file_sha256(files[name])
        if actual != expected:
            raise PortalError(f"SHA-256 mismatch for {name}; artifact validation stopped.")
    try:
        metadata = json.loads(files["build-metadata.json"].read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise PortalError("Build metadata is not valid JSON.") from exc
    if not re.fullmatch(r"[0-9a-f]{40}", str(metadata.get("source_commit", ""))):
        raise PortalError("Build metadata has no full source commit SHA.")
    package_version = str(metadata.get("package_version", ""))
    version_tuple(package_version)
    try:
        with zipfile.ZipFile(files["eden-xbox.appx"]) as package:
            archive_names = {name.lower(): name for name in package.namelist()}
            manifest_name = archive_names.get("appxmanifest.xml")
            if manifest_name is None or "appxsignature.p7x" not in archive_names:
                raise PortalError("APPX manifest or embedded package signature is missing.")
            forbidden = [
                name
                for name in archive_names
                if "/userdata/" in "/" + name
                or re.search(r"(^|/)(prod\.keys|title\.keys|.*\.(nca|nsp|xci|rom|pfx))$", name)
            ]
            if forbidden:
                raise PortalError("APPX contains user keys, firmware, game dumps, or private signing material.")
            runtime_files = {pathlib.PurePosixPath(name).name.lower() for name in archive_names}
            required_runtime = {"eden-uwp.exe", "spirv_to_dxil.dll", "dxil.dll"}
            if not required_runtime.issubset(runtime_files):
                raise PortalError("APPX is missing a required executable or D3D12 shader runtime DLL.")
            root = ET.fromstring(package.read(manifest_name))
    except (OSError, zipfile.BadZipFile, ET.ParseError) as exc:
        raise PortalError("APPX could not be inspected as a package archive.") from exc
    identity = root.find("{*}Identity")
    if identity is None:
        raise PortalError("APPX manifest has no Identity element.")
    if identity.attrib.get("Name") != APP_IDENTITY or identity.attrib.get("Publisher") != APP_PUBLISHER:
        raise PortalError("APPX manifest identity or publisher does not match the maintained package.")
    if identity.attrib.get("ProcessorArchitecture") != "x64" or identity.attrib.get("Version") != package_version:
        raise PortalError("APPX architecture or manifest version disagrees with build metadata.")
    dependencies = root.find("{*}Dependencies")
    if dependencies is None or not any(
        item.attrib.get("Name") == "Microsoft.VCLibs.140.00"
        for item in dependencies.findall("{*}PackageDependency")
    ):
        raise PortalError("APPX manifest does not declare the required Microsoft VCLibs framework.")
    application = root.find("{*}Applications/{*}Application")
    if application is None or application.attrib.get("Id") != "App":
        raise PortalError("APPX manifest has an unexpected application entry point.")
    print(
        f"Artifact verified: version {package_version}, x64, source {metadata['source_commit'][:12]}, "
        "required runtime files present, and all downloaded SHA-256 values match."
    )
    return files, metadata, {"version": package_version, "app_id": application.attrib["Id"]}


def installed_packages(client: PortalClient) -> list[dict]:
    result = client.json("GET", "/api/app/packagemanager/packages")
    items = result.get("InstalledPackages", [])
    if not isinstance(items, list):
        raise PortalError("Device Portal returned an unexpected installed-package list.")
    return [item for item in items if isinstance(item, dict)]


def app_package(items: list[dict]) -> dict | None:
    for item in items:
        name = str(item.get("Name", ""))
        family = str(item.get("PackageFamilyName", ""))
        if name == APP_IDENTITY or family.startswith(APP_IDENTITY + "_"):
            return item
    return None


def installed_version(item: dict) -> str:
    value = item.get("Version", {})
    if isinstance(value, dict):
        parts = (value.get("Major"), value.get("Minor"), value.get("Build"), value.get("Revision"))
        if all(part is not None for part in parts):
            return ".".join(str(int(part)) for part in parts)
    return "unknown"


def portal_status(client: PortalClient) -> tuple[int, dict | None]:
    status, body = client.request(
        "GET", "/api/app/packagemanager/state", max_bytes=MAX_JSON_BYTES, allow_status=(404,)
    )
    if status == 404:
        return 404, None
    if not body:
        return status, None
    try:
        payload = json.loads(body.decode("utf-8-sig"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        return status, None
    return status, payload if isinstance(payload, dict) else None


def deploy(raw: str, directory: pathlib.Path, timeout: int) -> None:
    files, metadata, package = verify_artifact(directory)
    client = require_portal_client(raw)
    client.ensure_session()
    installed = app_package(installed_packages(client))
    expected = version_tuple(package["version"])
    if installed:
        current = installed_version(installed)
        print(f"Existing Eden package found: version {current}.")
        if current != "unknown" and version_tuple(current) >= expected:
            raise PortalError(
                "The console already has this version or a newer one. Nothing was uninstalled or changed; "
                "build the rollback source again with a higher package version."
            )
    status, _ = portal_status(client)
    if status == 204:
        raise PortalError("Another Device Portal package operation is already running; no install was started.")
    client.ensure_session()
    print("Uploading APPX, VCLibs dependency, and public signer certificate over verified local HTTPS...")
    accepted = client.send_install(files)
    print(f"Device Portal accepted the package request (HTTP {accepted}); waiting for its result.")
    deadline = time.monotonic() + timeout
    last_notice = 0.0
    while time.monotonic() < deadline:
        state_status, state = portal_status(client)
        current_items = installed_packages(client)
        current = app_package(current_items)
        if current and installed_version(current) == package["version"] and state_status != 204:
            print(f"Install verified on Xbox: package version {package['version']} is present.")
            return
        if state_status == 200 and state and state.get("Success") is False:
            raise PortalError("Device Portal reported deployment failure; detailed response text was withheld.")
        now = time.monotonic()
        if now - last_notice >= 30:
            print("Waiting for Xbox package registration...")
            last_notice = now
        time.sleep(5)
    raise PortalError(
        "Timed out waiting for package registration. The signed package was not uninstalled; "
        "run verify to inspect the console and use Device Portal for its detailed deployment message."
    )


def verify_console(raw: str) -> None:
    client = require_portal_client(raw)
    item = app_package(installed_packages(client))
    if not item:
        print("Eden is not currently listed as installed on this Xbox.")
        return
    print(f"Eden is installed: version {installed_version(item)}.")


def launch(raw: str) -> None:
    client = require_portal_client(raw)
    item = app_package(installed_packages(client))
    if not item:
        raise PortalError("Eden is not installed on this Xbox.")
    package_name = item.get("PackageFullName")
    app_id = item.get("PackageRelativeId")
    if not isinstance(package_name, str) or not isinstance(app_id, str) or not package_name or not app_id:
        raise PortalError("Device Portal did not return the package and app IDs needed to launch Eden.")
    encoded_app = base64.b64encode(app_id.encode("utf-8")).decode("ascii")
    encoded_package = base64.b64encode(package_name.encode("utf-8")).decode("ascii")
    query = urllib.parse.urlencode({"appid": encoded_app, "package": encoded_package})
    client.ensure_session()
    client.request("POST", "/api/taskmanager/app?" + query, b"", {"Content-Length": "0"}, mutation=True)
    print("Eden launch request accepted. Check its screen and then run the logs command for startup diagnostics.")


def collect_logs(raw: str, output: pathlib.Path) -> list[pathlib.Path]:
    client = require_portal_client(raw)
    item = app_package(installed_packages(client))
    if not item:
        raise PortalError("Eden is not listed as installed, so its LocalState logs cannot be read.")
    package_full_name = item.get("PackageFullName")
    if not isinstance(package_full_name, str) or not package_full_name:
        raise PortalError("Device Portal did not return Eden's package full name.")
    output.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(output, 0o700)
    downloaded: list[pathlib.Path] = []
    for filename, directory in LOGS:
        query = urllib.parse.urlencode(
            {
                "knownfolderid": "LocalAppData",
                "packagefullname": package_full_name,
                "path": directory,
                "filename": filename,
            }
        )
        try:
            status, content = client.download("/api/filesystem/apps/file?" + query)
        except PortalError:
            raise
        if status == 404:
            continue
        destination = output / filename
        destination.write_bytes(content)
        os.chmod(destination, 0o600)
        downloaded.append(destination)
    if downloaded:
        print("Downloaded logs to the local folder (log contents were not printed):")
        for path in downloaded:
            print(f"  {path}")
    else:
        print("No Eden diagnostic log files were present yet.")
    return downloaded


def analyze_logs(paths: list[pathlib.Path]) -> None:
    if not paths:
        raise PortalError("Supply one or more local log files to analyze.")
    for path in paths:
        if not path.is_file():
            raise PortalError(f"Log file not found: {path}")
        try:
            content = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            raise PortalError(f"Could not read log file {path.name}.") from exc
        print(f"{path.name}:")
        if path.name == "eden_uwp_diag.txt":
            result_codes = re.findall(r"RunHeadlessBoot returned\s+(-?\d+)", content)
            crash_markers = len(re.findall(r"\b(?:CRASH|terminate|unhandled exception)\b", content, re.I))
            print(f"  startup result codes: {', '.join(result_codes) if result_codes else 'not recorded'}")
            print(f"  crash markers: {crash_markers}")
            memory = re.findall(r"app memory\s+(\d+) MiB of (\d+) MiB limit", content)
            if memory:
                print(f"  last recorded app memory: {memory[-1][0]} MiB of {memory[-1][1]} MiB")
        elif path.name == "eden_graphics_bugs.log":
            lines = [line for line in content.splitlines() if line.strip()]
            print(f"  graphics bug records: {len(lines)}")
        else:
            critical_count = len(re.findall(r"\bCritical\b", content, re.I))
            render_errors = len(re.findall(r"\bRender\b.*\b(?:ERROR|CRITICAL)\b", content, re.I))
            shader_ready = len(re.findall(r"D3D12: shader path ready", content))
            print(f"  Critical entries: {critical_count}")
            print(f"  Render error entries: {render_errors}")
            print(f"  shader-path ready markers: {shader_ready}")
            memory = re.findall(r"app memory\s+(\d+) MiB of (\d+) MiB limit", content)
            if memory:
                print(f"  last recorded app memory: {memory[-1][0]} MiB of {memory[-1][1]} MiB")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Eden Xbox macOS deployment and diagnostics helper")
    commands = parser.add_subparsers(dest="command", required=True)

    trust_parser = commands.add_parser("trust", help="pin the local Xbox Device Portal certificate")
    trust_parser.add_argument("--portal", required=True, help="Xbox Device Portal origin, for example https://192.168.1.20:11443")
    trust_parser.add_argument("--leaf-sha256", help="expected leaf certificate fingerprint; omit to confirm the displayed fingerprint interactively")
    trust_parser.add_argument("--replace-trust", action="store_true", help="replace an existing pin after verifying the new leaf fingerprint")

    verify_parser = commands.add_parser("verify-artifact", help="check downloaded artifact hashes and APPX contents")
    verify_parser.add_argument("--artifact-dir", required=True, type=pathlib.Path)

    for name, help_text in (
        ("verify-console", "show whether Eden is installed on the Xbox"),
        ("launch", "start Eden using the Device Portal"),
    ):
        sub = commands.add_parser(name, help=help_text)
        sub.add_argument("--portal", required=True)

    deploy_parser = commands.add_parser("deploy", help="verify and install a signed APPX update")
    deploy_parser.add_argument("--portal", required=True)
    deploy_parser.add_argument("--artifact-dir", required=True, type=pathlib.Path)
    deploy_parser.add_argument("--timeout", type=int, default=1200, help="maximum install wait in seconds (default: 1200)")

    logs_parser = commands.add_parser("logs", help="download Eden logs from Xbox LocalState")
    logs_parser.add_argument("--portal", required=True)
    logs_parser.add_argument(
        "--output-dir",
        type=pathlib.Path,
        default=pathlib.Path.home() / "Downloads" / "EdenXboxLogs",
    )

    analyze_parser = commands.add_parser("analyze-logs", help="summarize local logs without printing their contents")
    analyze_parser.add_argument("files", nargs="+", type=pathlib.Path)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    try:
        if args.command == "trust":
            trust_portal(args.portal, args.leaf_sha256, args.replace_trust)
        elif args.command == "verify-artifact":
            verify_artifact(args.artifact_dir)
        elif args.command == "verify-console":
            verify_console(args.portal)
        elif args.command == "deploy":
            if args.timeout < 60 or args.timeout > 3600:
                raise PortalError("Install timeout must be between 60 and 3600 seconds.")
            deploy(args.portal, args.artifact_dir, args.timeout)
        elif args.command == "launch":
            launch(args.portal)
        elif args.command == "logs":
            collect_logs(args.portal, args.output_dir)
        elif args.command == "analyze-logs":
            analyze_logs(args.files)
        else:
            raise PortalError("Unknown command.")
        return 0
    except (PortalError, KeyboardInterrupt) as exc:
        if isinstance(exc, KeyboardInterrupt):
            print("Interrupted.", file=sys.stderr)
            return 130
        print(f"Error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
