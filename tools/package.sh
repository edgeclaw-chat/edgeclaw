#!/bin/sh
# Build the release tarball with a FRESH AI-model catalog.
#
# models_catalog.json is a GENERATED build input, NOT committed (see .gitignore): it is
# fetched from models.dev, tested per provider, and written right before the build, so
# every package ships current data. Bazel cannot do this itself -- its actions have no
# network -- which is why this wrapper exists. Run it (not `bazel build //:edgeclaw`)
# when producing a release.
set -e

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

python3 tools/gen_models.py
exec bazel build //:edgeclaw "$@"
