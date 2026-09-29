#!/usr/bin/env sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_url=${LIBUV_REPO_URL:-https://github.com/libuv/libuv.git}
commit=${LIBUV_COMMIT:-840404ce8ba7cc0204be52389a6cfff9f2c90fb6}
patch_file=${PATCH_FILE:-$root/save-peer-in-accept.patch}

require_tool() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Required tool not found: $1" >&2
    exit 2
  fi
}

require_tools() {
  require_tool git
  require_tool cmake
  require_tool make
  require_tool cc
}

ensure_git_checkout() {
  dir=$1

  if [ -e "$root/$dir" ] && [ ! -d "$root/$dir/.git" ]; then
    echo "$dir exists but is not a git checkout" >&2
    exit 2
  fi

  if [ ! -d "$root/$dir/.git" ]; then
    git clone "$repo_url" "$root/$dir"
  fi

  if ! git -C "$root/$dir" cat-file -e "$commit^{commit}" 2>/dev/null; then
    git -C "$root/$dir" fetch --tags origin
  fi
}

checkout_clean_commit() {
  dir=$1

  if [ -n "$(git -C "$root/$dir" status --porcelain)" ]; then
    echo "$dir has local changes; refusing to checkout $commit" >&2
    exit 2
  fi

  git -C "$root/$dir" checkout --detach "$commit"
}

prepare_upstream() {
  ensure_git_checkout libuv-upstream
  checkout_clean_commit libuv-upstream
}

prepare_patched() {
  ensure_git_checkout libuv-patched

  if [ -n "$(git -C "$root/libuv-patched" status --porcelain)" ]; then
    current=$(git -C "$root/libuv-patched" rev-parse HEAD)
    if [ "$current" != "$commit" ]; then
      echo "libuv-patched has local changes but is at $current, expected $commit" >&2
      exit 2
    fi

    if git -C "$root/libuv-patched" apply --reverse --check "$patch_file" >/dev/null 2>&1; then
      echo "libuv-patched already has $patch_file applied"
      return
    fi

    echo "libuv-patched has local changes that are not the expected patch" >&2
    exit 2
  fi

  git -C "$root/libuv-patched" checkout --detach "$commit"

  if git -C "$root/libuv-patched" apply --check "$patch_file"; then
    git -C "$root/libuv-patched" apply "$patch_file"
  elif git -C "$root/libuv-patched" apply --reverse --check "$patch_file" >/dev/null 2>&1; then
    echo "libuv-patched already contains $patch_file"
  else
    echo "Could not apply $patch_file to libuv-patched" >&2
    exit 1
  fi
}

build_libuv() {
  dir=$1
  build="$root/$dir/build"

  cmake -S "$root/$dir" -B "$build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DLIBUV_BUILD_SHARED=OFF \
    -DLIBUV_BUILD_TESTS=OFF \
    -DLIBUV_BUILD_BENCH=OFF
  cmake --build "$build" --config Release --parallel
}

if [ ! -f "$patch_file" ]; then
  echo "Patch file not found: $patch_file" >&2
  exit 2
fi

require_tools
prepare_upstream
prepare_patched
build_libuv libuv-upstream
build_libuv libuv-patched
make -C "$root/bench" all
