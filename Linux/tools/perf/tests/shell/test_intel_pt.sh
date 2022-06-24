#!/bin/sh
# Miscellaneous Intel PT testing
# SPDX-License-Identifier: GPL-2.0

set -e

# Skip if no Intel PT
perf list | grep -q 'intel_pt//' || exit 2

skip_cnt=0
ok_cnt=0
err_cnt=0

tmpfile=`mktemp`
perfdatafile=`mktemp`

can_cpu_wide()
{
	perf record -o ${tmpfile} -B -N --no-bpf-event -e dummy:u -C $1 true 2>&1 >/dev/null || return 2
	return 0
}

test_system_wide_side_band()
{
	# Need CPU 0 and CPU 1
	can_cpu_wide 0 || return $?
	can_cpu_wide 1 || return $?

	# Record on CPU 0 a task running on CPU 1
	perf record -B -N --no-bpf-event -o ${perfdatafile} -e intel_pt//u -C 0 -- taskset --cpu-list 1 uname

	# Should get MMAP events from CPU 1 because they can be needed to decode
	mmap_cnt=`perf script -i ${perfdatafile} --no-itrace --show-mmap-events -C 1 2>/dev/null | grep MMAP | wc -l`

	if [ ${mmap_cnt} -gt 0 ] ; then
		return 0
	fi

	echo "Failed to record MMAP events on CPU 1 when tracing CPU 0"
	return 1
}

count_result()
{
	if [ $1 -eq 2 ] ; then
		skip_cnt=`expr ${skip_cnt} \+ 1`
		return
	fi
	if [ $1 -eq 0 ] ; then
		ok_cnt=`expr ${ok_cnt} \+ 1`
		return
	fi
	err_cnt=`expr ${err_cnt} \+ 1`
}

test_system_wide_side_band

count_result $?

rm -f ${tmpfile}
rm -f ${perfdatafile}

if [ ${err_cnt} -gt 0 ] ; then
	exit 1
fi

if [ ${ok_cnt} -gt 0 ] ; then
	exit 0
fi

exit 2
