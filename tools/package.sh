#!/bin/sh
# Build the release tarball with a FRESH AI-model catalog.
#
# models_catalog.json is a GENERATED build input, NOT committed (see .gitignore).
# //:models_catalog_json already fetches it during a normal build, but with --no-test
# (endpoint probes are slow). This wrapper runs the FULL pipeline first -- fetch, probe
# every provider, drop dead ones -- so a release ships validated data. The build then
# uses the written file as-is.
set -e

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

python3 tools/gen_models.py
exec bazel build //:edgeclaw "$@"
