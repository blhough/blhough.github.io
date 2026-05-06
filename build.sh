#!/bin/sh
# Build site into out/. See USAGE.txt. Extra args go to tool/gen_site.
set -e
cd "$(CDPATH= cd -- "$(dirname "$0")" && pwd)"

clang -O2 -Wall -Wextra -Wpedantic -std=c99 -o tool/gen_site tool/gen_site.c
tool/gen_site "$@"

if [ -f feed.xml ] && [ -d out ]; then
  cp feed.xml out/feed.xml
fi
