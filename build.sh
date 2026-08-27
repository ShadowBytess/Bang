#!/bin/sh

set -eu

repo_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
cd -- "$repo_dir"

build_jobs=1
if command -v nproc >/dev/null 2>&1; then
    build_jobs="$(nproc)"
fi

build() {
    cmake --preset "$1"
    cmake --build --preset "$1" --parallel "$build_jobs"
}

usage() {
    printf 'usage: %s [test|run|install|update]\n' "${0##*/}" >&2
}

if [ "$#" -gt 1 ]; then
    usage
    exit 2
fi

case "${1-}" in
    '')
        build release
        ;;
    test)
        build debug
        exec ctest --test-dir "$repo_dir/build/debug" --output-on-failure
        ;;
    run)
        build release
        exec "$repo_dir/build/release/bang"
        ;;
    install)
        if [ "$(id -u)" -eq 0 ]; then
            printf 'error: install is per-user; run sh build.sh install without sudo\n' >&2
            exit 1
        fi
        build release
        cmake --install "$repo_dir/build/release" --prefix "$HOME/.local"
        ;;
    update)
        if ! git diff --quiet || ! git diff --cached --quiet; then
            printf 'error: commit or stash tracked local changes before updating\n' >&2
            exit 1
        fi
        git pull --ff-only
        ;;
    *)
        usage
        exit 2
        ;;
esac
