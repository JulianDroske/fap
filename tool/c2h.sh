#!/bin/sh

ARGS="$*"

SRC="${1}"
DEST="${2}"
MACRO="${3}"
shift 3

(
  printf '/* auto generated with: %s */\n\n' "${ARGS}"
  printf '#ifndef %s\n#define %s\n\n' "${MACRO}" "${MACRO}"
  for opt in "$@"; do printf '#include "%s"\n' "${opt}"; done
  echo ''
  grep '^[[:alpha:]].*(.*)' "${SRC}" | sed 's/ {$/;/g'
  printf '\n#endif\n'
) > "${DEST}"

