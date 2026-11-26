#!/usr/bin/env bash
# Copies the six HyperLPR3 MNN models into <data_dir>/models/hyperlpr3.
#
# The model files are not stored in the repository. Point --source or
# HYPERLPR_MODELS_SOURCE at a directory that contains them (for example the
# upstream HyperLPR3 release/model bundle).
#
# Usage:
#   scripts/fetch-ai-models.sh [--data-dir <dir>] [--source <dir>]
#
# Defaults:
#   --data-dir   $UVNVR_DATA_DIR, else ./data
#   --source     $HYPERLPR_MODELS_SOURCE, then models/hyperlpr3 in this repo,
#                ~/.cache/uvnvr/hyperlpr3, /tmp/hlpr-models
set -euo pipefail

DATA_DIR="${UVNVR_DATA_DIR:-./data}"
SOURCE="${HYPERLPR_MODELS_SOURCE:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

usage() {
  sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --data-dir)
      [[ $# -ge 2 ]] || { echo "missing value for --data-dir" >&2; exit 2; }
      DATA_DIR="$2"
      shift 2
      ;;
    --source)
      [[ $# -ge 2 ]] || { echo "missing value for --source" >&2; exit 2; }
      SOURCE="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

MODELS=(
  b320_backbone_h.mnn
  b320_header_h.mnn
  b640x_backbone_h.mnn
  b640x_head_h.mnn
  litemodel_cls_96xh.mnn
  rpv3_mdict_160h.mnn
)

all_present() {
  local dir="$1"
  local name
  for name in "${MODELS[@]}"; do
    [[ -f "${dir}/${name}" ]] || return 1
  done
  return 0
}

if [[ -n "${SOURCE}" ]]; then
  if ! all_present "${SOURCE}"; then
    echo "source ${SOURCE} does not contain all HyperLPR3 models:" >&2
    printf '  %s\n' "${MODELS[@]}" >&2
    exit 1
  fi
else
  for candidate in \
    "${REPO_DIR}/models/hyperlpr3" \
    "${HOME}/.cache/uvnvr/hyperlpr3" \
    "/tmp/hlpr-models"; do
    if all_present "${candidate}"; then
      SOURCE="${candidate}"
      break
    fi
  done
  if [[ -z "${SOURCE}" ]]; then
    cat >&2 <<'EOF'
HyperLPR3 model files were not found.

Download the HyperLPR3 MNN models (the upstream release contains
b320_backbone_h.mnn, b320_header_h.mnn, b640x_backbone_h.mnn,
b640x_head_h.mnn, litemodel_cls_96xh.mnn and rpv3_mdict_160h.mnn), then run:

  scripts/fetch-ai-models.sh --source /path/to/models --data-dir /path/to/data

or set HYPERLPR_MODELS_SOURCE.
EOF
    exit 1
  fi
fi

DEST="${DATA_DIR%/}/models/hyperlpr3"
mkdir -p "${DEST}"
echo "source: ${SOURCE}"
echo "destination: ${DEST}"

for name in "${MODELS[@]}"; do
  if [[ "$(cd "${SOURCE}" && pwd -P)" == "$(cd "${DEST}" && pwd -P)" ]]; then
    echo "  ${name}: already in place"
    continue
  fi
  cp -f "${SOURCE}/${name}" "${DEST}/${name}"
  echo "  ${name}: $(stat -c '%s' "${DEST}/${name}") bytes"
done

echo "HyperLPR3 models are ready under ${DEST}"
