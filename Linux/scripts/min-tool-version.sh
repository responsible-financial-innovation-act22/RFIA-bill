#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# Print the minimum supported version of the given tool.
# When you raise the minimum version, please update
# Documentation/process/changes.rst as well.

set -e

if [ $# != 1 ]; then
	echo "Usage: $0 toolname" >&2
	exit 1
fi

case "$1" in
binutils)
	echo 2.23.0
	;;
gcc)
	echo 5.1.0
	;;
icc)
	# temporary
	echo 16.0.3
	;;
llvm)
	if [ "$SRCARCH" = s390 ]; then
		echo 14.0.0
	else
		echo 11.0.0
	fi
	;;
*)
	echo "$1: unknown tool" >&2
	exit 1
	;;
esac
