#!/usr/bin/env bash
# gradle-test.sh — wrapper: runs gradle from the extracted distribution
# (same as the CI/dev box; the wrapper jar is not committed).
set -euo pipefail
GRADLE_HOME="${GRADLE_HOME:-/opt/data/.tools/gradle-home/wrapper/dists/gradle-9.4.1-bin/gradle-9.4.1}"
export JAVA_HOME="${JAVA_HOME:-/opt/data/.tools/jdk17}"
export PATH="$JAVA_HOME/bin:$PATH"
exec "$GRADLE_HOME/bin/gradle" "$@"
