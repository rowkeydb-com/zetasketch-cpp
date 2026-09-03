#!/bin/sh
# Runs the bias table check against the reference source fetched by the
# build, so that a hand edit of the generated header is caught here.
set -eu
exec python3 "$1" "$2" "$3"
