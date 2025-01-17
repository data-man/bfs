#!/usr/bin/env bash

############################################################################
# bfs                                                                      #
# Copyright (C) 2019 Tavian Barnes <tavianator@tavianator.com>             #
#                                                                          #
# Permission to use, copy, modify, and/or distribute this software for any #
# purpose with or without fee is hereby granted.                           #
#                                                                          #
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES #
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF         #
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR  #
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES   #
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN    #
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF  #
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.           #
############################################################################

set -e

L=
COLOR=
ARGS=()
for ARG; do
    case "$ARG" in
        -L)
            L="$ARG"
            ;;
        -color)
            COLOR=y
            ;;
        *)
            ARGS+=("$ARG")
            ;;
    esac
done

LS_COLOR="${BASH_SOURCE%/*}/ls-color.sh"

if [ "$COLOR" ]; then
    find "${ARGS[@]}" -exec "$LS_COLOR" $L {} \;
else
    find "${ARGS[@]}"
fi
