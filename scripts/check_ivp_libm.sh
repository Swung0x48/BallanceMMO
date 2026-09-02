#!/bin/sh
# Fails if any IVP source calls a transcendental libm function directly
# instead of through ivp_libm (engine change #3 in docs/engine-changes.md).
# qhull has no such calls; havana and 3dsimport are not compiled.
IVP="$(dirname "$0")/../submodule/Ballanced/Source/BuildingBlocks/physics_RT/ivp"
hits=$(grep -rnE "(^|[^A-Za-z0-9_:.>])(sin|cos|tan|asin|acos|atan|atan2|exp|log|pow|sinf|cosf|tanf|asinf|acosf|atanf|atan2f|expf|logf|powf)[[:space:]]*\(" \
    --include='*.cxx' --include='*.hxx' "$IVP" \
    | grep -v 'qhull\|havana\|3dsimport\|ivu_libm.hxx' \
    | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|\*|/\*)' \
    | grep -vE '//.*(sin|cos|tan|exp|log|pow)\(')
if [ -n "$hits" ]; then
    echo "raw libm calls in IVP sources:"; echo "$hits"; exit 1
fi
echo "ok: every IVP transcendental call goes through ivp_libm"
