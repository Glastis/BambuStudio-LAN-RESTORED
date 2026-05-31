#!/usr/bin/env bash
#
# Build the BambuStudio AppImage inside a disposable, isolated container and
# export ONLY the resulting AppImage to the host (./dist/).
#
# Everything created for the build — an isolated buildx builder, its entire
# cache, and the moby/buildkit image (only if this script pulled it) — is
# removed at the end. Other containers and images are never touched: cleanup
# acts solely on resources this script created, addressed by their exact names.
#
# Re-running is safe and idempotent.
#
# Usage:
#   scripts/build-appimage-docker.sh           # build + export + cleanup
#   KEEP_BUILDER=1 scripts/build-appimage-docker.sh   # keep builder/cache (debug)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILDER="bambu-lan-appimage"     # dedicated, isolated buildx builder
DOCKERFILE="Dockerfile.appimage"
OUT_DIR="$ROOT/dist"
APP="BambuStudio_ubu64.AppImage"

log() { printf '\033[1;34m[build-appimage]\033[0m %s\n' "$*"; }
err() { printf '\033[1;31m[build-appimage]\033[0m %s\n' "$*" >&2; }

# Snapshot moby/buildkit images present BEFORE we start, so cleanup only removes
# one we introduced ourselves.
mapfile -t PRE_BK < <(docker images --format '{{.Repository}}:{{.Tag}}' | grep '^moby/buildkit:' || true)

cleanup() {
  if [[ "${KEEP_BUILDER:-0}" == "1" ]]; then
    log "KEEP_BUILDER=1 set — leaving builder '$BUILDER' and its cache in place."
    return
  fi
  log "Cleanup: removing isolated builder '$BUILDER' and its cache…"
  docker buildx rm "$BUILDER" >/dev/null 2>&1 || true

  # Remove the buildkit backing image only if it was absent before this run.
  mapfile -t POST_BK < <(docker images --format '{{.Repository}}:{{.Tag}}' | grep '^moby/buildkit:' || true)
  for img in "${POST_BK[@]:-}"; do
    [[ -z "$img" ]] && continue
    local was_present=0
    for p in "${PRE_BK[@]:-}"; do [[ "$p" == "$img" ]] && was_present=1; done
    if [[ "$was_present" == "0" ]]; then
      log "Removing build-only image: $img"
      docker rmi "$img" >/dev/null 2>&1 || true
    fi
  done
}
trap cleanup EXIT

mkdir -p "$OUT_DIR"

log "Creating isolated buildx builder '$BUILDER' (docker-container driver)…"
# --bootstrap pulls/starts the buildkit backend now so the snapshot logic above
# captures the image deterministically.
docker buildx create --name "$BUILDER" --driver docker-container --bootstrap >/dev/null

log "Building AppImage — long: bundled deps + app + appimage (tens of minutes)."
docker buildx --builder "$BUILDER" build \
  --file "$DOCKERFILE" \
  --target export \
  --output "type=local,dest=$OUT_DIR" \
  "$ROOT"

if [[ ! -f "$OUT_DIR/$APP" ]]; then
  err "AppImage not found at $OUT_DIR/$APP after build."
  exit 1
fi
chmod +x "$OUT_DIR/$APP"

log "AppImage exported: $OUT_DIR/$APP ($(du -h "$OUT_DIR/$APP" | cut -f1))"
log "Done. The host now holds only the AppImage; build resources are being removed."
