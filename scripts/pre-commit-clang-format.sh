#!/usr/bin/env bash
# format staged c/c++/objc
set -e
cd "$(git rev-parse --show-toplevel)"

if ! command -v clang-format &>/dev/null; then
  echo "clang-format not found, skipping (brew install clang-format)"
  exit 0
fi

files=$(git diff --cached --name-only --diff-filter=ACM | grep -E '\.(cpp|hpp|c|h|mm)$' || true)
[ -z "$files" ] && exit 0

changed=
for f in $files; do
  before=$(cat "$f")
  clang-format -i "$f"
  after=$(cat "$f")
  [ "$before" != "$after" ] && changed=1
done

if [ -n "$changed" ]; then
  echo "clang-format made changes. Stage and commit again."
  exit 1
fi
