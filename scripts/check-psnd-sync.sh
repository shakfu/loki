#!/bin/sh
# Fail if a generic-core file differs between loki and psnd (upstream).
# Usage: scripts/check-psnd-sync.sh [PSND_DIR]   (default: ../psnd)
set -eu

LOKI=$(cd "$(dirname "$0")/.." && pwd)/src
PSND=${1:-$(cd "$(dirname "$0")/../.." && pwd)/psnd}/source/core/loki

# loki path [psnd path, if named differently]
FILES='
command.h
command/file.c
command/goto.c
command/substitute.c
event.c
host.c
host.h
indent.c
indent.h
json.c
json.h
jsonrpc.c
jsonrpc.h
languages.h
search.c
search.h
selection.c
selection.h
serialize.c
syntax/lang_c.h
syntax/lang_lua.h
syntax/lang_markdown.h
syntax/lang_python.h
terminal.c terminal_posix.c
undo.c
undo.h
'

[ -d "$PSND" ] || { echo "psnd sources not found: $PSND" >&2; exit 2; }

drift=0
while read -r a b; do
    [ -n "$a" ] || continue
    b=${b:-$a}
    if ! cmp -s "$LOKI/$a" "$PSND/$b"; then
        echo "drift: src/$a != psnd/source/core/loki/$b"
        drift=$((drift + 1))
    fi
done <<EOF
$FILES
EOF

[ "$drift" -eq 0 ] || exit 1
echo "generic core in sync with psnd"
