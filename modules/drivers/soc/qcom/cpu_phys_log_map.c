// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/cpu_phys_log_map.h>
#include <linux/topology.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>

#define NUM_CLUSTER_CPUS NR_CPUS

#ifndef topology_cluster_id
#define topology_cluster_id(cpu) topology_physical_package_id(cpu)
#endif

static int cpu_phys_log_map_client_inited = -EPROBE_DEFER;
static uint32_t physical_cpus[NR_CPUS];

int cpu_logical_to_phys(int cpu)
{
	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_possible(cpu))
		return -EINVAL;

	if (likely(cpu_phys_log_map_client_inited > 0))
		return physical_cpus[cpu];

	pr_warn("cpu_phys_log_map has not been inited yet.\n");
	return cpu_phys_log_map_client_inited;
}
EXPORT_SYMBOL_GPL(cpu_logical_to_phys);

static int get_phy_cpu_logic(int clusters_len, u32 *cluster_cpus,
			     u32 *cummulative_cluster_cpus)
{
	int i, cpu, cluster_index, got_cpu;

	for (i = 0; i < NR_CPUS; i++)
		physical_cpus[i] = -1;

	for_each_possible_cpu(cpu) {
		cluster_index = topology_cluster_id(cpu);
		if (cluster_index < 0 || cluster_index >= clusters_len ||
		    topology_core_id(cpu) < 0 ||
		    topology_core_id(cpu) >= cluster_cpus[cluster_index])
			return -EINVAL;
		got_cpu = cummulative_cluster_cpus[cluster_index] + topology_core_id(cpu);
		physical_cpus[cpu] = got_cpu;
	}
	return 0;
}

static int qcom_cpu_logical_map(void)
{
	int i, ret, clusters_len;
	char name[20];
	struct device_node *cn, *map_handle, *child;
	u32 cluster_cpus[NUM_CLUSTER_CPUS] = {0};
	u32 cummulative_cluster_cpus[NUM_CLUSTER_CPUS] = {0};

	cn = of_find_node_by_path("/cpus");
	if (!cn)
		return -ENODEV;

	map_handle = of_get_child_by_name(cn, "cpu-map");
	of_node_put(cn);
	if (!map_handle)
		return -ENODEV;

	clusters_len = of_get_child_count(map_handle);
	if (!clusters_len || clusters_len > ARRAY_SIZE(cluster_cpus)) {
		ret = -EINVAL;
		goto out;
	}

	for (i = 0; i < clusters_len; i++) {
		snprintf(name, sizeof(name), "cluster%d", i);
		child = of_get_child_by_name(map_handle, name);
		if (!child) {
			ret = -EINVAL;
			goto out;
		}
		cluster_cpus[i] = of_get_child_count(child);
		of_node_put(child);
		if (!cluster_cpus[i]) {
			ret = -EINVAL;
			goto out;
		}
		if (i)
			cummulative_cluster_cpus[i] = cummulative_cluster_cpus[i - 1] +
						     cluster_cpus[i - 1];
	}

	ret = get_phy_cpu_logic(clusters_len, cluster_cpus, cummulative_cluster_cpus);
	if (!ret)
		cpu_phys_log_map_client_inited = 1;
out:
	of_node_put(map_handle);
	return ret;
}

module_init(qcom_cpu_logical_map);
MODULE_DESCRIPTION("Qualcomm Technologies, Inc. CPU Logical Map Module");
MODULE_LICENSE("GPL");
