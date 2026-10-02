#!/usr/bin/env bash
# =============================================================================
# scripts/setup-pfs-tool.sh - fetch MkPFS into .deps/ and print a runner path.
#
# Mirrors ProsperoLight's tools/setup-packaging-dependencies.sh (ffpfsc branch):
# a pinned checkout of github.com/PSBrew/MkPFS installed into an isolated venv.
# Used by scripts/package-app.sh --ffpfsc to PFS-pack the app folder into a
# single .ffpfsc image, the same container ProsperoLight ships.
#
# Prints the runner path on stdout; everything else goes to stderr.
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REVISION="6cb8313dfe0c988ac52617794553f343243d3a56"
CHECKOUT="${ROOT}/.deps/MkPFS"

git config --global --add safe.directory "${CHECKOUT}" 2>/dev/null || true

for cmd in git python3; do
    command -v "$cmd" >/dev/null || { echo "missing required command: $cmd" >&2; exit 2; }
done

if [[ ! -d "${CHECKOUT}/.git" ]]; then
    mkdir -p "${CHECKOUT}"
    git -C "${CHECKOUT}" init --quiet
    git -C "${CHECKOUT}" remote add origin https://github.com/PSBrew/MkPFS.git
    git -C "${CHECKOUT}" fetch --quiet --depth 1 origin "${REVISION}"
    git -C "${CHECKOUT}" checkout --quiet --detach FETCH_HEAD
fi

actual="$(git -C "${CHECKOUT}" rev-parse HEAD)"
[[ "${actual}" == "${REVISION}" ]] || { echo "MkPFS cache at ${actual}, expected ${REVISION}" >&2; exit 2; }

VENV="${CHECKOUT}/.venv-linux"
PY="${VENV}/bin/python"
STAMP="${VENV}/.evo-revision"

if [[ ! -x "${PY}" ]]; then
    rm -rf -- "${VENV}"
    python3 -m venv "${VENV}" || { echo "python3-venv is required" >&2; exit 2; }
fi
if [[ ! -f "${STAMP}" || "$(<"${STAMP}")" != "${REVISION}" ]]; then
    "${PY}" -m pip install --disable-pip-version-check --quiet --upgrade pip >&2 || true
    "${PY}" -m pip install --disable-pip-version-check --quiet "${CHECKOUT}" >&2
    printf '%s\n' "${REVISION}" > "${STAMP}"
fi

RUNNER="${CHECKOUT}/.mkpfs-run"
printf '#!/bin/sh\nexec "%s" -m mkpfs "$@"\n' "${PY}" > "${RUNNER}"
chmod +x "${RUNNER}"
"${RUNNER}" --help >/dev/null

printf '%s\n' "${RUNNER}"
