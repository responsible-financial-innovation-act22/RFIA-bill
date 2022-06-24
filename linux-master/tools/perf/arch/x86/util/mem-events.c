// SPDX-License-Identifier: GPL-2.0
#include "util/pmu.h"
#include "map_symbol.h"
#include "mem-events.h"

static char mem_loads_name[100];
static bool mem_loads_name__init;
static char mem_stores_name[100];

#define MEM_LOADS_AUX		0x8203
#define MEM_LOADS_AUX_NAME     "{%s/mem-loads-aux/,%s/mem-loads,ldlat=%u/}:P"

#define E(t, n, s) { .tag = t, .name = n, .sysfs_name = s }

static struct perf_mem_event perf_mem_events[PERF_MEM_EVENTS__MAX] = {
	E("ldlat-loads",	"%s/mem-loads,ldlat=%u/P",	"%s/events/mem-loads"),
	E("ldlat-stores",	"%s/mem-stores/P",		"%s/events/mem-stores"),
	E(NULL,			NULL,				NULL),
};

struct perf_mem_event *perf_mem_events__ptr(int i)
{
	if (i >= PERF_MEM_EVENTS__MAX)
		return NULL;

	return &perf_mem_events[i];
}

bool is_mem_loads_aux_event(struct evsel *leader)
{
	if (perf_pmu__find("cpu")) {
		if (!pmu_have_event("cpu", "mem-loads-aux"))
			return false;
	} else if (perf_pmu__find("cpu_core")) {
		if (!pmu_have_event("cpu_core", "mem-loads-aux"))
			return false;
	}

	return leader->core.attr.config == MEM_LOADS_AUX;
}

char *perf_mem_events__name(int i, char *pmu_name)
{
	struct perf_mem_event *e = perf_mem_events__ptr(i);

	if (!e)
		return NULL;

	if (i == PERF_MEM_EVENTS__LOAD) {
		if (mem_loads_name__init && !pmu_name)
			return mem_loads_name;

		if (!pmu_name) {
			mem_loads_name__init = true;
			pmu_name = (char *)"cpu";
		}

		if (pmu_have_event(pmu_name, "mem-loads-aux")) {
			scnprintf(mem_loads_name, sizeof(mem_loads_name),
				  MEM_LOADS_AUX_NAME, pmu_name, pmu_name,
				  perf_mem_events__loads_ldlat);
		} else {
			scnprintf(mem_loads_name, sizeof(mem_loads_name),
				  e->name, pmu_name,
				  perf_mem_events__loads_ldlat);
		}
		return mem_loads_name;
	}

	if (i == PERF_MEM_EVENTS__STORE) {
		if (!pmu_name)
			pmu_name = (char *)"cpu";

		scnprintf(mem_stores_name, sizeof(mem_stores_name),
			  e->name, pmu_name);
		return mem_stores_name;
	}

	return (char *)e->name;
}
