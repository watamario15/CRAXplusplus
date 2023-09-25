#!/usr/bin/env bash
# Copyright 2021-2022 Software Quality Laboratory, NYCU.

S2E_ROOT="$HOME/s2e"
CRAX_ROOT="$S2E_ROOT/source/CRAXplusplus"
CWD="$(cd "$(dirname "$0")"; pwd)"

function current_dir() {
    declare i=$(( ${#CWD} - 1 ))

    while true; do
        if [ $i -lt 0 ]; then
            echo "$CWD"
            return
        fi

        if [ "${CWD:$i:1}" = '/' ]; then
            echo "${CWD:(( $i + 1 ))}"
            return
        fi

        (( --i ))
    done
}

function usage() {
    echo "usage: $0 [-s|--staging] <target>"
    echo "examples:"
    echo "$0 unexploitable"
    echo "$0 --staging b64"
}

function create_symlinks() {
    # $1: directory name (either examples or examples-staging)
    # $2: target binary name (e.g., unexploitable)
    ln -sfv "$CRAX_ROOT/$1/$2/$2" "target"
    ln -sfv "$CRAX_ROOT/$1/$2/poc" "poc"
    ln -sfv "$CRAX_ROOT/$1/$2/s2e-config.template.lua" "s2e-config.template.lua"
    if [ -e "$CRAX_ROOT/$1/$2/bootstrap.sh" ]; then
        ln -sfv "$CRAX_ROOT/$1/$2/bootstrap.sh" "bootstrap.sh"
    else
        ln -sfv "$CRAX_ROOT"/proxies/"$(current_dir)"/bootstrap.sh "bootstrap.sh"
    fi
}


if [ $# -lt 1 ]; then
    usage
    exit 0
fi

# Parse command-line options
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            usage
            exit 0
            ;;
        -s|--staging)
            create_symlinks "examples-staging" $2
            exit 0
            ;;
        -*|--*)
            echo "Unknown option: $1"
            exit 1
            ;;
        *)
            create_symlinks "examples" $1
            exit 0
            ;;
    esac
done
