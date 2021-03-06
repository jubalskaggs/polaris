/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"@(#)lgrpplat.c	1.14	06/01/05 SMI"


#include <sys/archsystm.h>	/* for {in,out}{b,w,l}() */
#include <sys/cpupart.h>
#include <sys/cpuvar.h>
#include <sys/lgrp.h>
#include <sys/machsystm.h>
#include <sys/memlist.h>
#include <sys/memnode.h>
#include <sys/mman.h>
/* #include <sys/pci_cfgspace.h> */
#include <sys/pci_impl.h>
#include <sys/param.h>
#include <sys/promif.h>		/* for prom_printf() */
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/types.h>
#include <sys/var.h>
#include <vm/seg_kmem.h>
#include <vm/vm_dep.h>
#include <sys/cmn_err.h>


/* XXX - lgrp plat started with i86, nothing related in 2.6 */

/*
 * lgroup platform support for PPC platforms.
 */

#define	MAX_NODES		8
#define	NLGRP			(MAX_NODES * (MAX_NODES - 1) + 1)

#define	LGRP_PLAT_CPU_TO_NODE(cpu)	(chip_plat_get_chipid(cpu))

#define	LGRP_PLAT_PROBE_NROUNDS		64	/* default laps for probing */
#define	LGRP_PLAT_PROBE_NSAMPLES	1	/* default samples to take */
#define	LGRP_PLAT_PROBE_NREADS		256	/* number of vendor ID reads */

/* XXX - Dummy values for now ... needs lots of work */

#define PPC_PCS_BUS_CONFIG      0       /* Hypertransport config space bus */

/*
 * PowerPC PCI configuration space register function values
 */
#define PPC_PCS_FUNC_HT         0       /* Hypertransport configuration */
#define PPC_PCS_FUNC_ADDRMAP    1       /* Address map configuration */
#define PPC_PCS_FUNC_DRAM       2       /* DRAM configuration */
#define PPC_PCS_FUNC_MISC       3       /* Miscellaneous configuration */

/*
 * PCI Configuration Space register offsets
 */
#define PPC_PCS_OFF_VENDOR      0x0     /* device/vendor ID register */
#define PPC_PCS_OFF_DRAMBASE    0x40    /* DRAM Base register (node 0) */
#define PPC_PCS_OFF_NODEID      0x60    /* Node ID register */
#define PPC_PCS_DEV_NODE0       24      /* device number for node 0 */


/*
 * Bookkeeping for latencies seen during probing (used for verification)
 */
typedef	struct lgrp_plat_latency_acct {
	hrtime_t	la_value;	/* latency value */
	int		la_count;	/* occurrences */
} lgrp_plat_latency_acct_t;


/*
 * Choices for probing to determine lgroup topology
 */
typedef	enum lgrp_plat_probe_op {
	LGRP_PLAT_PROBE_PGCPY,		/* Use page copy */
	LGRP_PLAT_PROBE_VENDOR		/* Read vendor ID on Northbridge */
} lgrp_plat_probe_op_t;


/*
 * Starting and ending page for physical memory in node
 */
typedef	struct phys_addr_map {
	pfn_t	start;
	pfn_t	end;
	int	exists;
} phys_addr_map_t;



/*
 * Whether memory is interleaved across nodes causing MPO to be disabled
 */
int			lgrp_plat_mem_intrlv = 0;

/*
 * Number of nodes in system
 */
uint_t			lgrp_plat_node_cnt = 1;

/*
 * Physical address range for memory in each node
 */
phys_addr_map_t		lgrp_plat_node_memory[MAX_NODES];

/*
 * Probe costs (individual and total) and flush cost
 */
hrtime_t		lgrp_plat_flush_cost = 0;
hrtime_t		lgrp_plat_probe_cost = 0;
hrtime_t		lgrp_plat_probe_cost_total = 0;

/*
 * Error code for latency adjustment and verification
 */
int			lgrp_plat_probe_error_code = 0;

/*
 * How much latencies were off from minimum values gotten
 */
hrtime_t		lgrp_plat_probe_errors[MAX_NODES][MAX_NODES];

/*
 * Unique probe latencies and number of occurrences of each
 */
lgrp_plat_latency_acct_t	lgrp_plat_probe_lat_acct[MAX_NODES];

/*
 * Size of memory buffer in each node for probing
 */
size_t			lgrp_plat_probe_memsize = 0;

/*
 * Virtual address of page in each node for probing
 */
caddr_t			lgrp_plat_probe_memory[MAX_NODES];

/*
 * Number of unique latencies in probe times
 */
int			lgrp_plat_probe_nlatencies = 0;

/*
 * How many rounds of probing to do
 */
int			lgrp_plat_probe_nrounds = LGRP_PLAT_PROBE_NROUNDS;

/*
 * Number of samples to take when probing each node
 */
int			lgrp_plat_probe_nsamples = LGRP_PLAT_PROBE_NSAMPLES;

/*
 * Number of times to read vendor ID from Northbridge for each probe.
 */
int			lgrp_plat_probe_nreads = LGRP_PLAT_PROBE_NREADS;

/*
 * How to probe to determine lgroup topology
 */
lgrp_plat_probe_op_t	lgrp_plat_probe_op = LGRP_PLAT_PROBE_VENDOR;

/*
 * PFN of page in each node for probing
 */
pfn_t			lgrp_plat_probe_pfn[MAX_NODES];

/*
 * Whether probe time was suspect (ie. not within tolerance of value that it
 * should match)
 */
int			lgrp_plat_probe_suspect[MAX_NODES][MAX_NODES];

/*
 * How long it takes to access memory from each node
 */
hrtime_t		lgrp_plat_probe_times[MAX_NODES][MAX_NODES];

/*
 * Min and max node memory probe times seen
 */
hrtime_t		lgrp_plat_probe_time_max = 0;
hrtime_t		lgrp_plat_probe_time_min = -1;
hrtime_t		lgrp_plat_probe_max[MAX_NODES][MAX_NODES];
hrtime_t		lgrp_plat_probe_min[MAX_NODES][MAX_NODES];


/*
 * Allocate lgrp and lgrp stat arrays statically.
 */
static lgrp_t	lgrp_space[NLGRP];
static int	nlgrps_alloc;

struct lgrp_stats lgrp_stats[NLGRP];

#define	CPUID_FAMILY_PPC	15

uint_t	ppc_family = 0;
uint_t	ppc_model = 0;
uint_t	ppc_probe_func = PPC_PCS_FUNC_DRAM;


int
plat_lgrphand_to_mem_node(lgrp_handle_t hand)
{
	if (max_mem_nodes == 1)
		return (0);

	return ((int)hand);
}

lgrp_handle_t
plat_mem_node_to_lgrphand(int mnode)
{
	if (max_mem_nodes == 1)
		return (LGRP_DEFAULT_HANDLE);

	return ((lgrp_handle_t)mnode);
}

int
plat_pfn_to_mem_node(pfn_t pfn)
{
	int	node;

	if (max_mem_nodes == 1)
		return (0);

	for (node = 0; node < lgrp_plat_node_cnt; node++) {
		/*
		 * Skip nodes with no memory
		 */
		if (!lgrp_plat_node_memory[node].exists)
			continue;

		if (pfn >= lgrp_plat_node_memory[node].start &&
		    pfn <= lgrp_plat_node_memory[node].end)
			return (node);
	}

	ASSERT(node < lgrp_plat_node_cnt);
	return (-1);
}

/*
 * Configure memory nodes for machines with more than one node (ie NUMA)
 */
void
plat_build_mem_nodes(struct memlist *list)
{
	pfn_t		cur_start;	/* start addr of subrange */
	pfn_t		cur_end;	/* end addr of subrange */
	pfn_t		start;		/* start addr of whole range */
	pfn_t		end;		/* end addr of whole range */

	/*
	 * Boot install lists are arranged <addr, len>, ...
	 */
	while (list) {
		int	node;

		start = list->address >> PAGESHIFT;
		end = (list->address + list->size - 1) >> PAGESHIFT;

		if (start > physmax) {
			list = list->next;
			continue;
		}
		if (end > physmax)
			end = physmax;

		/*
		 * When there is only one memnode, just add memory to memnode
		 */
		if (max_mem_nodes == 1) {
			mem_node_add_slice(start, end);
			list = list->next;
			continue;
		}

		/*
		 * mem_node_add_slice() expects to get a memory range that
		 * is within one memnode, so need to split any memory range
		 * that spans multiple memnodes into subranges that are each
		 * contained within one memnode when feeding them to
		 * mem_node_add_slice()
		 */
		cur_start = start;
		do {
			node = plat_pfn_to_mem_node(cur_start);

			/*
			 * Panic if DRAM address map registers or SRAT say
			 * memory in node doesn't exist or address from
			 * boot installed memory list entry isn't in this node.
			 * This shouldn't happen and rest of code can't deal
			 * with this if it does.
			 */
			if (node < 0 || node >= lgrp_plat_node_cnt ||
			    !lgrp_plat_node_memory[node].exists ||
			    cur_start < lgrp_plat_node_memory[node].start ||
			    cur_start > lgrp_plat_node_memory[node].end) {
				cmn_err(CE_PANIC, "Don't know which memnode "
				    "to add installed memory address 0x%lx\n",
				    cur_start);
			}

			/*
			 * End of current subrange should not span memnodes
			 */
			cur_end = end;
			if (lgrp_plat_node_memory[node].exists &&
			    cur_end > lgrp_plat_node_memory[node].end)
				cur_end = lgrp_plat_node_memory[node].end;

			mem_node_add_slice(cur_start, cur_end);

			/*
			 * Next subrange starts after end of current one
			 */
			cur_start = cur_end + 1;
		} while (cur_end < end);

		list = list->next;
	}
	mem_node_physalign = 0;
	mem_node_pfn_shift = 0;
}


/*
 * Platform-specific initialization of lgroups
 */
void
lgrp_plat_init(void)
{
	uint_t		bus;
	uint_t		dev;
	uint_t		node;
	uint_t		off;

	extern lgrp_load_t	lgrp_expand_proc_thresh;
	extern lgrp_load_t	lgrp_expand_proc_diff;

	/*
	 * Read configuration registers from PCI configuration space to
	 * determine node information, which memory is in each node, etc.
	 *
	 * Write to PCI configuration space address register to specify
	 * which configuration register to read and read/write PCI
	 * configuration space data register to get/set contents
	 */
	bus = PPC_PCS_BUS_CONFIG;
	dev = PPC_PCS_DEV_NODE0;
	off = PPC_PCS_OFF_DRAMBASE;

	/*
	 * Read node ID register for node 0 to get node count
	 */

	/*
	 * Only use one memory node if memory is interleaved between any nodes
	 */
	if (lgrp_plat_mem_intrlv) {
		lgrp_plat_node_cnt = max_mem_nodes = 1;
		(void) lgrp_topo_ht_limit_set(1);
	} else {
		max_mem_nodes = lgrp_plat_node_cnt;

		/*
		 * Probing errors can mess up the lgroup topology and force us
		 * fall back to a 2 level lgroup topology.  Here we bound how
		 * tall the lgroup topology can grow in hopes of avoiding any
		 * anamolies in probing from messing up the lgroup topology
		 * by limiting the accuracy of the latency topology.
		 *
		 * Assume that nodes will at least be configured in a ring,
		 * so limit height of lgroup topology to be less than number
		 * of nodes on a system with 4 or more nodes
		 */
		if (lgrp_plat_node_cnt >= 4 &&
		    lgrp_topo_ht_limit() == lgrp_topo_ht_limit_default())
			(void) lgrp_topo_ht_limit_set(lgrp_plat_node_cnt - 1);
	}

	/*
	 * Lgroups on PowerPC architectures have but a single physical
	 * processor. Tune lgrp_expand_proc_thresh and lgrp_expand_proc_diff
	 * so that lgrp_choose() will spread things out aggressively.
	 */
	lgrp_expand_proc_thresh = LGRP_LOADAVG_THREAD_MAX / 2;
	lgrp_expand_proc_diff = 0;
}


/*
 * Latencies must be within 1/(2**LGRP_LAT_TOLERANCE_SHIFT) of each other to
 * be considered same
 */
#define	LGRP_LAT_TOLERANCE_SHIFT	4

int	lgrp_plat_probe_lt_shift = LGRP_LAT_TOLERANCE_SHIFT;


/*
 * Adjust latencies between nodes to be symmetric, normalize latencies between
 * any nodes that are within some tolerance to be same, and make local
 * latencies be same
 */
static void
lgrp_plat_latency_adjust(void)
{
	int				i;
	int				j;
	int				k;
	int				l;
	u_longlong_t			max;
	u_longlong_t			min;
	u_longlong_t			t;
	u_longlong_t			t1;
	u_longlong_t			t2;
	const lgrp_config_flag_t	cflag = LGRP_CONFIG_LATENCY_CHANGE;
	int				lat_corrected[MAX_NODES][MAX_NODES];

	/*
	 * Nothing to do when this is an UMA machine
	 */
	if (max_mem_nodes == 1)
		return;

	/*
	 * Make sure that latencies are symmetric between any two nodes
	 * (ie. latency(node0, node1) == latency(node1, node0))
	 */
	for (i = 0; i < lgrp_plat_node_cnt; i++)
		for (j = 0; j < lgrp_plat_node_cnt; j++) {
			t1 = lgrp_plat_probe_times[i][j];
			t2 = lgrp_plat_probe_times[j][i];

			if (t1 == 0 || t2 == 0 || t1 == t2)
				continue;

			/*
			 * Latencies should be same
			 * - Use minimum of two latencies which should be same
			 * - Track suspect probe times not within tolerance of
			 *   min value
			 * - Remember how much values are corrected by
			 */
			if (t1 > t2) {
				t = t2;
				lgrp_plat_probe_errors[i][j] += t1 - t2;
				if (t1 - t2 > t2 >> lgrp_plat_probe_lt_shift) {
					lgrp_plat_probe_suspect[i][j]++;
					lgrp_plat_probe_suspect[j][i]++;
				}
			} else if (t2 > t1) {
				t = t1;
				lgrp_plat_probe_errors[j][i] += t2 - t1;
				if (t2 - t1 > t1 >> lgrp_plat_probe_lt_shift) {
					lgrp_plat_probe_suspect[i][j]++;
					lgrp_plat_probe_suspect[j][i]++;
				}
			}

			lgrp_plat_probe_times[i][j] =
			    lgrp_plat_probe_times[j][i] = t;
			lgrp_config(cflag, t1, t);
			lgrp_config(cflag, t2, t);
		}

	/*
	 * Keep track of which latencies get corrected
	 */
	for (i = 0; i < MAX_NODES; i++)
		for (j = 0; j < MAX_NODES; j++)
			lat_corrected[i][j] = 0;

	/*
	 * For every two nodes, see whether there is another pair of nodes which
	 * are about the same distance apart and make the latencies be the same
	 * if they are close enough together
	 */
	for (i = 0; i < lgrp_plat_node_cnt; i++)
		for (j = 0; j < lgrp_plat_node_cnt; j++) {
			/*
			 * Pick one pair of nodes (i, j)
			 * and get latency between them
			 */
			t1 = lgrp_plat_probe_times[i][j];

			/*
			 * Skip this pair of nodes if there isn't a latency
			 * for it yet
			 */
			if (t1 == 0)
				continue;

			for (k = 0; k < lgrp_plat_node_cnt; k++)
				for (l = 0; l < lgrp_plat_node_cnt; l++) {
					/*
					 * Pick another pair of nodes (k, l)
					 * not same as (i, j) and get latency
					 * between them
					 */
					if (k == i && l == j)
						continue;

					t2 = lgrp_plat_probe_times[k][l];

					/*
					 * Skip this pair of nodes if there
					 * isn't a latency for it yet
					 */

					if (t2 == 0)
						continue;

					/*
					 * Skip nodes (k, l) if they already
					 * have same latency as (i, j) or
					 * their latency isn't close enough to
					 * be considered/made the same
					 */
					if (t1 == t2 || (t1 > t2 && t1 - t2 >
					    t1 >> lgrp_plat_probe_lt_shift) ||
					    (t2 > t1 && t2 - t1 >
					    t2 >> lgrp_plat_probe_lt_shift))
						continue;

					/*
					 * Make latency(i, j) same as
					 * latency(k, l), try to use latency
					 * that has been adjusted already to get
					 * more consistency (if possible), and
					 * remember which latencies were
					 * adjusted for next time
					 */
					if (lat_corrected[i][j]) {
						t = t1;
						lgrp_config(cflag, t2, t);
						t2 = t;
					} else if (lat_corrected[k][l]) {
						t = t2;
						lgrp_config(cflag, t1, t);
						t1 = t;
					} else {
						if (t1 > t2)
							t = t2;
						else
							t = t1;
						lgrp_config(cflag, t1, t);
						lgrp_config(cflag, t2, t);
						t1 = t2 = t;
					}

					lgrp_plat_probe_times[i][j] =
					    lgrp_plat_probe_times[k][l] = t;

					lat_corrected[i][j] =
					    lat_corrected[k][l] = 1;
				}
		}

	/*
	 * Local latencies should be same
	 * - Find min and max local latencies
	 * - Make all local latencies be minimum
	 */
	min = -1;
	max = 0;
	for (i = 0; i < lgrp_plat_node_cnt; i++) {
		t = lgrp_plat_probe_times[i][i];
		if (t == 0)
			continue;
		if (min == -1 || t < min)
			min = t;
		if (t > max)
			max = t;
	}
	if (min != max) {
		for (i = 0; i < lgrp_plat_node_cnt; i++) {
			int	local;

			local = lgrp_plat_probe_times[i][i];
			if (local == 0)
				continue;

			/*
			 * Track suspect probe times that aren't within
			 * tolerance of minimum local latency and how much
			 * probe times are corrected by
			 */
			if (local - min > min >> lgrp_plat_probe_lt_shift)
				lgrp_plat_probe_suspect[i][i]++;

			lgrp_plat_probe_errors[i][i] += local - min;

			/*
			 * Make local latencies be minimum
			 */
			lgrp_config(cflag, local, min);
			lgrp_plat_probe_times[i][i] = min;
		}
	}

	/*
	 * Determine max probe time again since just adjusted latencies
	 */
	lgrp_plat_probe_time_max = 0;
	for (i = 0; i < lgrp_plat_node_cnt; i++)
		for (j = 0; j < lgrp_plat_node_cnt; j++) {
			t = lgrp_plat_probe_times[i][j];
			if (t > lgrp_plat_probe_time_max)
				lgrp_plat_probe_time_max = t;
		}
}


/*
 * Verify following about latencies between nodes:
 *
 * - Latencies should be symmetric (ie. latency(a, b) == latency(b, a))
 * - Local latencies same
 * - Local < remote
 * - Number of latencies seen is reasonable
 * - Number of occurrences of a given latency should be more than 1
 *
 * Returns:
 *	0	Success
 *	-1	Not symmetric
 *	-2	Local latencies not same
 *	-3	Local >= remote
 *	-4	Wrong number of latencies
 *	-5	Not enough occurrences of given latency
 */
static int
lgrp_plat_latency_verify(void)
{
	int				i;
	int				j;
	lgrp_plat_latency_acct_t	*l;
	int				probed;
	u_longlong_t			t1;
	u_longlong_t			t2;

	/*
	 * Nothing to do when this is an UMA machine, lgroup topology is
	 * limited to 2 levels, or there aren't any probe times yet
	 */
	if (max_mem_nodes == 1 || lgrp_topo_levels < 2 ||
	    (lgrp_plat_probe_time_max == 0 && lgrp_plat_probe_time_min == -1))
		return (0);

	/*
	 * Make sure that latencies are symmetric between any two nodes
	 * (ie. latency(node0, node1) == latency(node1, node0))
	 */
	for (i = 0; i < lgrp_plat_node_cnt; i++)
		for (j = 0; j < lgrp_plat_node_cnt; j++) {
			t1 = lgrp_plat_probe_times[i][j];
			t2 = lgrp_plat_probe_times[j][i];

			if (t1 == 0 || t2 == 0 || t1 == t2)
				continue;

			return (-1);
		}

	/*
	 * Local latencies should be same
	 */
	t1 = lgrp_plat_probe_times[0][0];
	for (i = 1; i < lgrp_plat_node_cnt; i++) {
		t2 = lgrp_plat_probe_times[i][i];
		if (t2 == 0)
			continue;

		if (t1 == 0) {
			t1 = t2;
			continue;
		}

		if (t1 != t2)
			return (-2);
	}

	/*
	 * Local latencies should be less than remote
	 */
	if (t1) {
		for (i = 0; i < lgrp_plat_node_cnt; i++)
			for (j = 0; j < lgrp_plat_node_cnt; j++) {
				t2 = lgrp_plat_probe_times[i][j];
				if (i == j || t2 == 0)
					continue;

				if (t1 >= t2)
					return (-3);
			}
	}

	/*
	 * Rest of checks are not very useful for machines with less than
	 * 4 nodes
	 */
	if (lgrp_plat_node_cnt < 4)
		return (0);

	/*
	 * Need to see whether done probing in order to verify number of
	 * latencies are correct
	 */
	probed = 0;
	for (i = 0; i < lgrp_plat_node_cnt; i++)
		if (lgrp_plat_probe_times[i][i])
			probed++;

	if (probed != lgrp_plat_node_cnt)
		return (0);

	/*
	 * Determine number of unique latencies seen in probe times,
	 * their values, and number of occurrences of each
	 */
	lgrp_plat_probe_nlatencies = 0;
	bzero(lgrp_plat_probe_lat_acct,
	    MAX_NODES * sizeof (lgrp_plat_latency_acct_t));
	for (i = 0; i < lgrp_plat_node_cnt; i++) {
		for (j = 0; j < lgrp_plat_node_cnt; j++) {
			int	k;

			/*
			 * Look at each probe time
			 */
			t1 = lgrp_plat_probe_times[i][j];
			if (t1 == 0)
				continue;

			/*
			 * Account for unique latencies
			 */
			for (k = 0; k < lgrp_plat_node_cnt; k++) {
				l = &lgrp_plat_probe_lat_acct[k];
				if (t1 == l->la_value) {
					/*
					 * Increment number of occurrences
					 * if seen before
					 */
					l->la_count++;
					break;
				} else if (l->la_value == 0) {
					/*
					 * Record latency if haven't seen before
					 */
					l->la_value = t1;
					l->la_count++;
					lgrp_plat_probe_nlatencies++;
					break;
				}
			}
		}
	}

	/*
	 * Number of latencies should be relative to number of
	 * nodes in system:
	 * - Same as nodes when nodes <= 2
	 * - Less than nodes when nodes > 2
	 * - Greater than 2 when nodes >= 4
	 */
	if ((lgrp_plat_node_cnt <= 2 &&
	    lgrp_plat_probe_nlatencies != lgrp_plat_node_cnt) ||
	    (lgrp_plat_node_cnt > 2 &&
	    lgrp_plat_probe_nlatencies >= lgrp_plat_node_cnt) ||
	    (lgrp_plat_node_cnt >= 4 && lgrp_topo_levels >= 3 &&
	    lgrp_plat_probe_nlatencies <= 2))
		return (-4);

	/*
	 * There should be more than one occurrence of every latency
	 * as long as probing is complete
	 */
	for (i = 0; i < lgrp_plat_probe_nlatencies; i++) {
		l = &lgrp_plat_probe_lat_acct[i];
		if (l->la_count <= 1)
			return (-5);
	}
	return (0);
}


/*
 * Set lgroup latencies for 2 level lgroup topology
 */
static void
lgrp_plat_2level_setup(void)
{
	int	i;

	if (lgrp_plat_node_cnt >= 4)
		cmn_err(CE_NOTE,
		    "MPO only optimizing for local and remote\n");
	for (i = 0; i < lgrp_plat_node_cnt; i++) {
		int	j;

		for (j = 0; j < lgrp_plat_node_cnt; j++) {
			if (i == j)
				lgrp_plat_probe_times[i][j] = 2;
			else
				lgrp_plat_probe_times[i][j] = 3;
		}
	}
	lgrp_plat_probe_time_min = 2;
	lgrp_plat_probe_time_max = 3;
	lgrp_config(LGRP_CONFIG_FLATTEN, 2, 0);
}


/*
 * Return time needed to probe from current CPU to memory in given node
 */
static hrtime_t
lgrp_plat_probe_time(int to)
{
	caddr_t		buf;
	uint_t		dev;
	/* LINTED: set but not used in function */
	volatile uint_t	dev_vendor;
	hrtime_t	elapsed;
	hrtime_t	end;
	int		from;
	int		i;
	int		ipl;
	hrtime_t	max;
	hrtime_t	min;
	hrtime_t	start;
	int		cnt;
	extern int	use_sse_pagecopy;

	/*
	 * Determine ID of node containing current CPU
	 */
	from = LGRP_PLAT_CPU_TO_NODE(CPU);

	/*
	 * Do common work for probing main memory
	 */
	if (lgrp_plat_probe_op == LGRP_PLAT_PROBE_PGCPY) {
		/*
		 * Skip probing any nodes without memory and
		 * set probe time to 0
		 */
		if (lgrp_plat_probe_memory[to] == NULL) {
			lgrp_plat_probe_times[from][to] = 0;
			return (0);
		}

		/*
		 * Invalidate caches once instead of once every sample
		 * which should cut cost of probing by a lot
		 */
		lgrp_plat_flush_cost = gethrtime();
		lgrp_plat_flush_cost = gethrtime() - lgrp_plat_flush_cost;
		lgrp_plat_probe_cost_total += lgrp_plat_flush_cost;
	}

	/*
	 * Probe from current CPU to given memory using specified operation
	 * and take specified number of samples
	 */
	max = 0;
	min = -1;
	for (i = 0; i < lgrp_plat_probe_nsamples; i++) {
		lgrp_plat_probe_cost = gethrtime();

		/*
		 * Can't measure probe time if gethrtime() isn't working yet
		 */
		if (lgrp_plat_probe_cost == 0 && gethrtime() == 0)
			return (0);

		switch (lgrp_plat_probe_op) {

		case LGRP_PLAT_PROBE_PGCPY:
		default:
			/*
			 * Measure how long it takes to copy page
			 * on top of itself
			 */
			buf = lgrp_plat_probe_memory[to] + (i * PAGESIZE);

			kpreempt_disable();
			ipl = splhigh();
			start = gethrtime();
			end = gethrtime();
			elapsed = end - start;
			splx(ipl);
			kpreempt_enable();
			break;

		case LGRP_PLAT_PROBE_VENDOR:
			/*
			 * Measure how long it takes to read vendor ID from
			 * Northbridge
			 */
                  /* 			dev = OPT_PCS_DEV_NODE0 + to; */
			kpreempt_disable();
			ipl = spl8();
                        /* 			outl(PCI_CONFADD, PCI_CADDR1(0, dev, opt_probe_func,
			    OPT_PCS_OFF_VENDOR)); */
			start = gethrtime();
			/* for (cnt = 0; cnt < lgrp_plat_probe_nreads; cnt++)
                           dev_vendor = inl(PCI_CONFDATA); */
			end = gethrtime();
			elapsed = (end - start) / lgrp_plat_probe_nreads;
			splx(ipl);
			kpreempt_enable();
			break;
		}

		lgrp_plat_probe_cost = gethrtime() - lgrp_plat_probe_cost;
		lgrp_plat_probe_cost_total += lgrp_plat_probe_cost;

		if (min == -1 || elapsed < min)
			min = elapsed;
		if (elapsed > max)
			max = elapsed;
	}

	/*
	 * Update minimum and maximum probe times between
	 * these two nodes
	 */
	if (min < lgrp_plat_probe_min[from][to] ||
	    lgrp_plat_probe_min[from][to] == 0)
		lgrp_plat_probe_min[from][to] = min;

	if (max > lgrp_plat_probe_max[from][to])
		lgrp_plat_probe_max[from][to] = max;

	return (min);
}


/*
 * Probe memory in each node from current CPU to determine latency topology
 */
void
lgrp_plat_probe(void)
{
	int		from;
	int		i;
	hrtime_t	probe_time;
	int		to;

	if (max_mem_nodes == 1 || lgrp_topo_ht_limit() <= 2)
		return;

	/*
	 * Determine ID of node containing current CPU
	 */
	from = LGRP_PLAT_CPU_TO_NODE(CPU);

	/*
	 * Don't need to probe if got times already
	 */
	if (lgrp_plat_probe_times[from][from] != 0)
		return;

	/*
	 * Read vendor ID in Northbridge or read and write page(s)
	 * in each node from current CPU and remember how long it takes,
	 * so we can build latency topology of machine later.
	 * This should approximate the memory latency between each node.
	 */
	for (i = 0; i < lgrp_plat_probe_nrounds; i++)
		for (to = 0; to < lgrp_plat_node_cnt; to++) {
			/*
			 * Get probe time and bail out if can't get it yet
			 */
			probe_time = lgrp_plat_probe_time(to);
			if (probe_time == 0)
				return;

			/*
			 * Keep lowest probe time as latency between nodes
			 */
			if (lgrp_plat_probe_times[from][to] == 0 ||
			    probe_time < lgrp_plat_probe_times[from][to])
				lgrp_plat_probe_times[from][to] = probe_time;

			/*
			 * Update overall minimum and maximum probe times
			 * across all nodes
			 */
			if (probe_time < lgrp_plat_probe_time_min ||
			    lgrp_plat_probe_time_min == -1)
				lgrp_plat_probe_time_min = probe_time;
			if (probe_time > lgrp_plat_probe_time_max)
				lgrp_plat_probe_time_max = probe_time;
		}

	/*
	 * - Fix up latencies such that local latencies are same,
	 *   latency(i, j) == latency(j, i), etc. (if possible)
	 *
	 * - Verify that latencies look ok
	 *
	 * - Fallback to just optimizing for local and remote if
	 *   latencies didn't look right
	 */
	lgrp_plat_latency_adjust();
	lgrp_plat_probe_error_code = lgrp_plat_latency_verify();
	if (lgrp_plat_probe_error_code)
		lgrp_plat_2level_setup();
}


/*
 * Platform-specific initialization
 */
void
lgrp_plat_main_init(void)
{
	int	curnode;
	int	ht_limit;
	int	i;

	/*
	 * Print a notice that MPO is disabled when memory is interleaved
	 * across nodes....Would do this when it is discovered, but can't
	 * because it happens way too early during boot....
	 */
	if (lgrp_plat_mem_intrlv)
		cmn_err(CE_NOTE,
		    "MPO disabled because memory is interleaved\n");

	/*
	 * Don't bother to do any probing if there is only one node or the
	 * height of the lgroup topology less than or equal to 2
	 */
	ht_limit = lgrp_topo_ht_limit();
	if (max_mem_nodes == 1 || ht_limit <= 2) {
		/*
		 * Setup lgroup latencies for 2 level lgroup topology
		 * (ie. local and remote only) if they haven't been set yet
		 */
		if (ht_limit == 2 && lgrp_plat_probe_time_min == -1 &&
		    lgrp_plat_probe_time_max == 0)
			lgrp_plat_2level_setup();
		return;
	}

	if (lgrp_plat_probe_op == LGRP_PLAT_PROBE_VENDOR) {
		/*
		 * Should have been able to probe from CPU 0 when it was added
		 * to lgroup hierarchy, but may not have been able to then
		 * because it happens so early in boot that gethrtime() hasn't
		 * been initialized.  (:-(
		 */
		curnode = LGRP_PLAT_CPU_TO_NODE(CPU);
		if (lgrp_plat_probe_times[curnode][curnode] == 0)
			lgrp_plat_probe();

		return;
	}

	/*
	 * When probing memory, use one page for every sample to determine
	 * lgroup topology and taking multiple samples
	 */
	if (lgrp_plat_probe_memsize == 0)
		lgrp_plat_probe_memsize = PAGESIZE *
		    lgrp_plat_probe_nsamples;

	/*
	 * Map memory in each node needed for probing to determine latency
	 * topology
	 */
	for (i = 0; i < lgrp_plat_node_cnt; i++) {
		int	mnode;

		/*
		 * Skip this node and leave its probe page NULL
		 * if it doesn't have any memory
		 */
		mnode = plat_lgrphand_to_mem_node((lgrp_handle_t)i);
		if (!mem_node_config[mnode].exists) {
			lgrp_plat_probe_memory[i] = NULL;
			continue;
		}

		/*
		 * Allocate one kernel virtual page
		 */
		lgrp_plat_probe_memory[i] = vmem_alloc(heap_arena,
		    lgrp_plat_probe_memsize, VM_NOSLEEP);
		if (lgrp_plat_probe_memory[i] == NULL) {
			cmn_err(CE_WARN,
			    "lgrp_plat_main_init: couldn't allocate memory");
			return;
		}

		/*
		 * Map virtual page to first page in node
		 */
		hat_devload(kas.a_hat, lgrp_plat_probe_memory[i],
		    lgrp_plat_probe_memsize,
		    lgrp_plat_probe_pfn[i],
		    PROT_READ | PROT_WRITE,
		    HAT_LOAD_NOCONSIST);
	}

	/*
	 * Probe from current CPU
	 */
	lgrp_plat_probe();
}

/*
 * Allocate additional space for an lgroup.
 */
/* ARGSUSED */
lgrp_t *
lgrp_plat_alloc(lgrp_id_t lgrpid)
{
	lgrp_t *lgrp;

	lgrp = &lgrp_space[nlgrps_alloc++];
	if (lgrpid >= NLGRP || nlgrps_alloc > NLGRP)
		return (NULL);
	return (lgrp);
}

/*
 * Platform handling for (re)configuration changes
 */
/* ARGSUSED */
void
lgrp_plat_config(lgrp_config_flag_t flag, uintptr_t arg)
{
	prom_printf("LGRPs platform config is currently unimplemented with OpenSolaris/PowerPC\n");
}

/*
 * Return the platform handle for the lgroup containing the given CPU
 */
/* ARGSUSED */
lgrp_handle_t
lgrp_plat_cpu_to_hand(processorid_t id)
{
	if (lgrp_plat_node_cnt == 1)
		return (LGRP_DEFAULT_HANDLE);

	return ((lgrp_handle_t)LGRP_PLAT_CPU_TO_NODE(cpu[id]));
}

/*
 * Return the platform handle of the lgroup that contains the physical memory
 * corresponding to the given page frame number
 */
/* ARGSUSED */
lgrp_handle_t
lgrp_plat_pfn_to_hand(pfn_t pfn)
{
	int	mnode;

	if (max_mem_nodes == 1)
		return (LGRP_DEFAULT_HANDLE);

	mnode = plat_pfn_to_mem_node(pfn);
	return (MEM_NODE_2_LGRPHAND(mnode));
}

/*
 * Return the maximum number of lgrps supported by the platform.
 * Before lgrp topology is known it returns an estimate based on the number of
 * nodes. Once topology is known it returns the actual maximim number of lgrps
 * created. Since x86 doesn't support dynamic addition of new nodes, this number
 * may not grow during system lifetime.
 */
int
lgrp_plat_max_lgrps()
{
	return (lgrp_topo_initialized ?
	    lgrp_alloc_max + 1 :
	    lgrp_plat_node_cnt * (lgrp_plat_node_cnt - 1) + 1);
}

/*
 * Return the number of free, allocatable, or installed
 * pages in an lgroup
 * This is a copy of the MAX_MEM_NODES == 1 version of the routine
 * used when MPO is disabled (i.e. single lgroup) or this is the root lgroup
 */
/* ARGSUSED */
static pgcnt_t
lgrp_plat_mem_size_default(lgrp_handle_t lgrphand, lgrp_mem_query_t query)
{
	struct memlist *mlist;
	pgcnt_t npgs = 0;
	extern struct memlist *phys_avail;
	extern struct memlist *phys_install;

	switch (query) {
	case LGRP_MEM_SIZE_FREE:
		return ((pgcnt_t)freemem);
	case LGRP_MEM_SIZE_AVAIL:
		memlist_read_lock();
		for (mlist = phys_avail; mlist; mlist = mlist->next)
			npgs += btop(mlist->size);
		memlist_read_unlock();
		return (npgs);
	case LGRP_MEM_SIZE_INSTALL:
		memlist_read_lock();
		for (mlist = phys_install; mlist; mlist = mlist->next)
			npgs += btop(mlist->size);
		memlist_read_unlock();
		return (npgs);
	default:
		return ((pgcnt_t)0);
	}
}

/*
 * Return the number of free pages in an lgroup.
 *
 * For query of LGRP_MEM_SIZE_FREE, return the number of base pagesize
 * pages on freelists.  For query of LGRP_MEM_SIZE_AVAIL, return the
 * number of allocatable base pagesize pages corresponding to the
 * lgroup (e.g. do not include page_t's, BOP_ALLOC()'ed memory, ..)
 * For query of LGRP_MEM_SIZE_INSTALL, return the amount of physical
 * memory installed, regardless of whether or not it's usable.
 */
pgcnt_t
lgrp_plat_mem_size(lgrp_handle_t plathand, lgrp_mem_query_t query)
{
	int	mnode;
	pgcnt_t npgs = (pgcnt_t)0;
	extern struct memlist *phys_avail;
	extern struct memlist *phys_install;


	if (plathand == LGRP_DEFAULT_HANDLE)
		return (lgrp_plat_mem_size_default(plathand, query));

	if (plathand != LGRP_NULL_HANDLE) {
		mnode = plat_lgrphand_to_mem_node(plathand);
		if (mnode >= 0 && mem_node_config[mnode].exists) {
			switch (query) {
			case LGRP_MEM_SIZE_FREE:
				break;
			case LGRP_MEM_SIZE_AVAIL:
				npgs = mem_node_memlist_pages(mnode,
				    phys_avail);
				break;
			case LGRP_MEM_SIZE_INSTALL:
				npgs = mem_node_memlist_pages(mnode,
				    phys_install);
				break;
			default:
				break;
			}
		}
	}
	return (npgs);
}

/*
 * Return latency between "from" and "to" lgroups
 *
 * This latency number can only be used for relative comparison
 * between lgroups on the running system, cannot be used across platforms,
 * and may not reflect the actual latency.  It is platform and implementation
 * specific, so platform gets to decide its value.  It would be nice if the
 * number was at least proportional to make comparisons more meaningful though.
 */
/* ARGSUSED */
int
lgrp_plat_latency(lgrp_handle_t from, lgrp_handle_t to)
{
	lgrp_handle_t	src, dest;

	if (max_mem_nodes == 1)
		return (0);

	/*
	 * Return max latency for root lgroup
	 */
	if (from == LGRP_DEFAULT_HANDLE || to == LGRP_DEFAULT_HANDLE)
		return (lgrp_plat_probe_time_max);

	src = from;
	dest = to;

	/*
	 * Return 0 for nodes (lgroup platform handles) out of range
	 */
	if (src < 0 || src >= MAX_NODES || dest < 0 || dest >= MAX_NODES)
		return (0);

	/*
	 * Probe from current CPU if its lgroup latencies haven't been set yet
	 * and we are trying to get latency from current CPU to some node
	 */
	if (lgrp_plat_probe_times[src][src] == 0 &&
	    LGRP_PLAT_CPU_TO_NODE(CPU) == src)
		lgrp_plat_probe();

	return (lgrp_plat_probe_times[src][dest]);
}

/*
 * Return platform handle for root lgroup
 */
lgrp_handle_t
lgrp_plat_root_hand(void)
{
	return (LGRP_DEFAULT_HANDLE);
}
