#!/bin/bash
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
umask 077

repo='temamumtaza/eden-xbox'
for tool in openssl gh shasum rg; do
    command -v "$tool" >/dev/null || { printf 'Missing required command: %s\n' "$tool" >&2; exit 1; }
done
gh auth status --hostname github.com >/dev/null

existing="$(gh secret list --repo "$repo" --json name --jq '.[].name')"
for name in XBOX_SIGNING_PFX_BASE64 XBOX_SIGNING_PASSWORD XBOX_SIGNING_THUMBPRINT; do
    if printf '%s\n' "$existing" | rg -q "^${name}$"; then
        printf 'Secret %s already exists. Refusing to rotate the sideload identity.\n' "$name" >&2
        exit 2
    fi
done

scratch="$(mktemp -d "${TMPDIR:-/tmp}/eden-xbox-signing.XXXXXX")"
trap 'rm -rf "$scratch"' EXIT
openssl rand -hex 40 > "$scratch/password"
openssl req -x509 -newkey rsa:3072 -nodes -sha256 -days 1095 \
    -keyout "$scratch/eden-signing.key.pem" \
    -out "$scratch/eden-signing.cert.pem" \
    -subj '/CN=EdenXboxDev' \
    -addext 'basicConstraints=critical,CA:FALSE' \
    -addext 'keyUsage=critical,digitalSignature' \
    -addext 'extendedKeyUsage=critical,codeSigning' \
    >/dev/null 2>&1
openssl pkcs12 -export -out "$scratch/eden-signing.pfx" \
    -inkey "$scratch/eden-signing.key.pem" -in "$scratch/eden-signing.cert.pem" \
    -name 'Eden Xbox development signer' -passout "file:$scratch/password" \
    >/dev/null 2>&1
base64 < "$scratch/eden-signing.pfx" | tr -d '\n' > "$scratch/pfx.base64"
thumbprint="$(openssl x509 -in "$scratch/eden-signing.cert.pem" -noout -fingerprint -sha1 | cut -d= -f2 | tr -d ':')"

# Values are streamed into GitHub's encrypted secret store; no key material or password is printed.
gh secret set XBOX_SIGNING_PFX_BASE64 --repo "$repo" < "$scratch/pfx.base64"
gh secret set XBOX_SIGNING_PASSWORD --repo "$repo" < "$scratch/password"
printf '%s' "$thumbprint" | gh secret set XBOX_SIGNING_THUMBPRINT --repo "$repo"

printf 'Development signing identity provisioned for %s. Certificate SHA-1 thumbprint: %s\n' "$repo" "$thumbprint"
printf 'The encrypted PFX and random password were sent directly to GitHub Actions Secrets.\n'
