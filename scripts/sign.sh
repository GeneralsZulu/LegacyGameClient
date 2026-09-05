#!/usr/bin/env bash
# Authenticode-sign Windows PE files (installer, uninstaller, game exe,
# launcher) with Azure Artifact Signing (formerly Trusted Signing), driven from Linux via jsign.
#
#   scripts/sign.sh FILE...        sign each file in place
#   scripts/sign.sh --status       print the resolved config and tool checks
#
# Called by the Makefile for the staged exe/launcher and by Zulu.nsi's
# !finalize / !uninstfinalize hooks for the installer and uninstaller.
#
# Configuration comes from the environment, falling back to
# ~/.config/zulu/signing.env (override the path with ZULU_SIGN_CONFIG):
#
#   ZULU_SIGN_ACCOUNT    Artifact Signing account name           (required)
#   ZULU_SIGN_PROFILE    certificate profile name in that account (required)
#   ZULU_SIGN_ENDPOINT   regional endpoint, default eus.codesigning.azure.net
#                        (wus2 / weu / neu / jpe ... match the account's region)
#   ZULU_SIGN_TOKEN      pre-fetched Azure access token; when unset the script
#                        asks the Azure CLI (`az login` must have been run)
#   ZULU_SIGN=0          skip signing even if configured
#   ZULU_SIGN_REQUIRED=1 fail instead of skipping when signing is not possible
#                        (set by `make installer-release` so a release is never
#                        silently shipped unsigned)
#
# Unsigned dev builds keep working with no config at all: with nothing
# configured the script prints one warning line and exits 0.
set -euo pipefail

JSIGN_VERSION="7.5"
JSIGN_SHA256="602a51c3545a6dc4fb99bd2ea7152b26d1345916d0c93ddfbd5936cb735af91c"
JSIGN_URL="https://github.com/ebourg/jsign/releases/download/${JSIGN_VERSION}/jsign-${JSIGN_VERSION}.jar"
# Artifact Signing certs live for 3 days, so every signature needs a
# countersigned timestamp. Microsoft's TSA is the one that pairs with the
# service; jsign would pick one automatically but we pin it to be explicit.
TSA_URL="http://timestamp.acs.microsoft.com"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JSIGN_JAR="${REPO_ROOT}/build/jsign/jsign-${JSIGN_VERSION}.jar"

CONFIG="${ZULU_SIGN_CONFIG:-$HOME/.config/zulu/signing.env}"
VARS="ZULU_SIGN ZULU_SIGN_REQUIRED ZULU_SIGN_ENDPOINT ZULU_SIGN_ACCOUNT ZULU_SIGN_PROFILE ZULU_SIGN_TOKEN"
if [ -f "$CONFIG" ]; then
    # The file is plain `KEY=value` shell. Anything already set in the
    # environment wins over it so a one-off override on the make command
    # line still works.
    for v in $VARS; do
        [ -n "${!v:-}" ] && eval "saved_$v=\${$v}"
    done
    # shellcheck disable=SC1090
    . "$CONFIG"
    for v in $VARS; do
        s="saved_$v"
        [ -n "${!s:-}" ] && eval "$v=\${$s}"
    done
fi

ZULU_SIGN="${ZULU_SIGN:-1}"
ZULU_SIGN_REQUIRED="${ZULU_SIGN_REQUIRED:-0}"
ZULU_SIGN_ENDPOINT="${ZULU_SIGN_ENDPOINT:-eus.codesigning.azure.net}"
ZULU_SIGN_ACCOUNT="${ZULU_SIGN_ACCOUNT:-}"
ZULU_SIGN_PROFILE="${ZULU_SIGN_PROFILE:-}"
ZULU_SIGN_TOKEN="${ZULU_SIGN_TOKEN:-}"

log()  { printf '[sign] %s\n' "$*" >&2; }
die()  { log "ERROR: $*"; exit 1; }

# A signing problem is fatal for releases and a warning otherwise.
bail() {
    if [ "$ZULU_SIGN_REQUIRED" = "1" ]; then
        die "$*"
    fi
    log "skipping signing: $*"
    exit 0
}

status() {
    printf 'config file:   %s (%s)\n' "$CONFIG" "$([ -f "$CONFIG" ] && echo present || echo missing)"
    printf 'ZULU_SIGN:     %s\n' "$ZULU_SIGN"
    printf 'endpoint:      %s\n' "$ZULU_SIGN_ENDPOINT"
    printf 'account:       %s\n' "${ZULU_SIGN_ACCOUNT:-<unset>}"
    printf 'profile:       %s\n' "${ZULU_SIGN_PROFILE:-<unset>}"
    printf 'token:         %s\n' "$([ -n "$ZULU_SIGN_TOKEN" ] && echo 'from env' || echo 'via az CLI')"
    printf 'java:          %s\n' "$(command -v java || echo MISSING)"
    printf 'az:            %s\n' "$(command -v az || echo MISSING)"
    printf 'jsign jar:     %s (%s)\n' "$JSIGN_JAR" "$([ -f "$JSIGN_JAR" ] && echo present || echo 'will download')"
    if command -v az >/dev/null 2>&1; then
        if az account show >/dev/null 2>&1; then
            printf 'az login:      ok (%s)\n' "$(az account show --query user.name -o tsv 2>/dev/null)"
        else
            printf 'az login:      NOT logged in (run: az login)\n'
        fi
    fi
}

ensure_jsign() {
    if [ ! -f "$JSIGN_JAR" ]; then
        log "downloading jsign ${JSIGN_VERSION}..."
        mkdir -p "$(dirname "$JSIGN_JAR")"
        curl -fsSL -o "${JSIGN_JAR}.tmp" "$JSIGN_URL" || die "download of $JSIGN_URL failed"
        mv "${JSIGN_JAR}.tmp" "$JSIGN_JAR"
    fi
    local have
    have="$(sha256sum "$JSIGN_JAR" | cut -d' ' -f1)"
    [ "$have" = "$JSIGN_SHA256" ] \
        || die "jsign jar checksum mismatch: got $have, want $JSIGN_SHA256 (delete $JSIGN_JAR and retry)"
}

fetch_token() {
    command -v az >/dev/null 2>&1 \
        || bail "Azure CLI (az) not installed; install azure-cli and run 'az login'"
    ZULU_SIGN_TOKEN="$(az account get-access-token \
        --resource https://codesigning.azure.net \
        --query accessToken -o tsv 2>/dev/null)" \
        || bail "az account get-access-token failed; run 'az login' first"
    [ -n "$ZULU_SIGN_TOKEN" ] || bail "az returned an empty access token"
}

case "${1:-}" in
    --status) status; exit 0 ;;
    -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
    "") die "no files given (see --help)" ;;
esac

# An explicit opt-out wins even for releases: it is the escape hatch for
# shipping while Azure is unreachable. `make installer-release ZULU_SIGN=0`.
if [ "$ZULU_SIGN" = "0" ]; then
    log "WARNING: ZULU_SIGN=0, shipping UNSIGNED"
    exit 0
fi
[ -n "$ZULU_SIGN_ACCOUNT" ] && [ -n "$ZULU_SIGN_PROFILE" ] \
    || bail "ZULU_SIGN_ACCOUNT / ZULU_SIGN_PROFILE not set (see $CONFIG)"
command -v java >/dev/null 2>&1 || bail "java not installed (jsign needs a JRE)"

for f in "$@"; do
    [ -f "$f" ] || die "no such file: $f"
done

ensure_jsign
[ -n "$ZULU_SIGN_TOKEN" ] || fetch_token

# One jsign invocation for all files: jsign fetches the certificate chain
# once per run, and that fetch counts against the monthly signing quota.
# The service occasionally answers the certificate-chain fetch with a bare
# "InternalError" (seen right after the profile was created); a retry a few
# seconds later succeeds. jsign signs in place, so a failed attempt leaves
# the input untouched and the retry is safe.
log "signing $# file(s) as ${ZULU_SIGN_ACCOUNT}/${ZULU_SIGN_PROFILE} via ${ZULU_SIGN_ENDPOINT}"
attempt=1
until java -jar "$JSIGN_JAR" \
        --storetype TRUSTEDSIGNING \
        --keystore "$ZULU_SIGN_ENDPOINT" \
        --storepass "$ZULU_SIGN_TOKEN" \
        --alias "${ZULU_SIGN_ACCOUNT}/${ZULU_SIGN_PROFILE}" \
        --tsaurl "$TSA_URL" \
        --tsmode RFC3161 \
        --name "Zulu" \
        --url "https://github.com/GeneralsZulu/LegacyGameClient" \
        "$@"; do
    [ "$attempt" -lt 4 ] || die "signing failed after $attempt attempts"
    log "attempt $attempt failed, retrying in 15s..."
    sleep 15
    attempt=$((attempt + 1))
done
for f in "$@"; do log "signed $f"; done
