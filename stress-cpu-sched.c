/*
 * Copyright (C) 2024      Colin Ian King.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */
#include "stress-ng.h"
#include "core-asm-generic.h"
#include "core-builtin.h"
#include "core-capabilities.h"
#include "core-killpid.h"
#include "core-numa.h"
#include "core-out-of-memory.h"

#include <sched.h>

#if defined(__NR_set_mempolicy)
#define HAVE_SET_MEMPOLICY
#endif

#if defined(HAVE_SCHED_SETAFFINITY) &&					     \
    (defined(_POSIX_PRIORITY_SCHEDULING) || defined(__linux__)) &&	     \
    (defined(SCHED_OTHER) || defined(SCHED_BATCH) || defined(SCHED_IDLE)) && \
    !defined(__OpenBSD__) &&						     \
    !defined(__minix__) &&						     \
    !defined(__APPLE__)
#define HAVE_SCHEDULING
#endif

#define MAX_CPU_SCHED_PROCS		(16)

static const stress_help_t help[] = {
	{ NULL,	"cpu-sched N",		"start N workers that exercise cpu affinity" },
	{ NULL,	"cpu-sched-ops N",	"stop after N bogo cpu affinity operations" },
	{ NULL,	NULL,			NULL }
};

#if defined(HAVE_SCHEDULING) &&		\
    defined(HAVE_SCHED_SETSCHEDULER)

static stress_pid_t stress_cpu_sched_pids[MAX_CPU_SCHED_PROCS];

#if defined(HAVE_SET_MEMPOLICY)
static int max_numa_node;
static size_t node_mask_size;
static unsigned long *node_mask;

static const int mpol_modes[] = {
	0,
#if defined(MPOL_BIND)
	MPOL_BIND,
#if defined(MPOL_F_NUMA_BALANCING)
	MPOL_BIND | MPOL_F_NUMA_BALANCING,
#endif
#endif
#if defined(MPOL_INTERLEAVE)
	MPOL_INTERLEAVE,
#endif
#if defined(MPOL_PREFERRED)
	MPOL_PREFERRED,
#endif
#if defined(MPOL_LOCAL)
	MPOL_LOCAL,
#endif
};
#endif

/*
 *  "Normal" non-realtime scheduling policies
 */
static const int normal_policies[] = {
#if defined(SCHED_OTHER)
		SCHED_OTHER,
#endif
#if defined(SCHED_BATCH)
		SCHED_BATCH,
#endif
#if defined(SCHED_IDLE)
		SCHED_IDLE,
#endif
};

static int stress_cpu_sched_setaffinity(
	stress_args_t *args,
	const pid_t pid,
	const int cpu)
{
	cpu_set_t cpu_set;
	int ret;

	CPU_ZERO(&cpu_set);
	CPU_SET(cpu, &cpu_set);
	ret = sched_setaffinity(pid, sizeof(cpu_set), &cpu_set);
	if (ret == 0) {
		CPU_ZERO(&cpu_set);
		ret = sched_getaffinity(pid, sizeof(cpu_set), &cpu_set);
		if ((ret < 0) && (errno != ESRCH)) {
			pr_fail("%s: sched_getaffinity failed on PID %jd, errno=%d (%s)\n",
				args->name, (intmax_t)pid, errno, strerror(errno));
			return ret;
		}
	}
	return 0;
}

static int stress_cpu_sched_setscheduler(
	stress_args_t *args,
	const pid_t pid)
{
	struct sched_param param;
	const uint32_t i = stress_mwc8modn((uint8_t)SIZEOF_ARRAY(normal_policies));
	int ret;

	(void)shim_memset(&param, 0, sizeof(param));
	param.sched_priority = 0;
	ret = sched_setscheduler(pid, normal_policies[i], &param);
	if (ret == 0) {
		ret = sched_getscheduler(pid);
		if ((ret < 0) && (errno != ESRCH)) {
			pr_fail("%s: sched_getscheduler failed on PID %jd, errno=%d (%s)\n",
				args->name, (intmax_t)pid, errno, strerror(errno));
			return ret;
		}
	}
	return 0;
}

/*
 *  stress_cpu_sched_mix_pids()
 *	change order of pids
 */
static void stress_cpu_sched_mix_pids(stress_pid_t *mix_pids, stress_pid_t *orig_pids, const size_t n)
{
	register int i;
	register size_t j;

	switch (stress_mwc8modn(3)) {
	case 0:
		/* In order */
		(void)memcpy(mix_pids, orig_pids, n * sizeof(*mix_pids));
		break;
	case 1:
		/* Shuffle */
		(void)memcpy(mix_pids, orig_pids, n * sizeof(*mix_pids));
		for (i = 0; i < 3; i++) {
			for (j = 0; j < n; j++) {
				stress_pid_t tmp;

				size_t k = stress_mwc8modn(n);
				tmp = mix_pids[j];
				mix_pids[j] = mix_pids[k];
				mix_pids[k] = tmp;
			}
		}
		break;
	case 2:
		/* Reverse order */
		for (j = 0; j < n; j++)
			mix_pids[j] = orig_pids[(n - 1) - j];
		break;
	}
}

/*
 *  stress_cpu_sched_next_cpu()
 *	select next cpu
 */
int stress_cpu_sched_next_cpu(const int instance, const int last_cpu, const int cpus)
{
	struct timeval now;
	int cpu;

	if (gettimeofday(&now, NULL) < 0)
		return stress_mwc32modn(cpus);

	switch (now.tv_sec & 7) {
	default:
	case 0:
		/* random selection */
		return stress_mwc32modn(cpus);
	case 1:
		/* next cpu */
		cpu = last_cpu + 1;
		return cpu >= cpus ? 0 : cpu;
	case 2:
		/* prev cpu */
		cpu = last_cpu - 1;
		return cpu < 0 ? cpus - 1 : cpu;
	case 3:
		/* based on seconds past EPOCH */
		return now.tv_sec % cpus;
	case 4:
		/* instance and seconds past EPOCH */
		return (instance + now.tv_sec) % cpus;
	case 5:
		/* stride cpus by instance number */
		return (last_cpu + instance + 1) % cpus;
	case 6:
		/* based on 10th of second */
		return (now.tv_usec / 100000) % cpus;
	case 7:
		/* ping pong from last cpu */
		return (cpus - 1) - last_cpu;
	}
	return last_cpu;
}

static int stress_cpu_sched_child(stress_args_t *args, void *context)
{
	/* Child */
	int cpu = 0, cpus = (int)stress_get_processors_configured(), rc = EXIT_SUCCESS;
	const int instance = (int)args->instance;
	size_t i;
	stress_pid_t pids[MAX_CPU_SCHED_PROCS];
	const bool cap_sys_nice = stress_check_capability(SHIM_CAP_SYS_NICE);

	(void)context;

	if (cpus < 1)
		cpus = 1;

	(void)shim_memset(pids, 0, sizeof(pids));
	for (i = 0; i < MAX_CPU_SCHED_PROCS; i++) {
		stress_cpu_sched_pids[i].pid = -1;
	}
	for (i = 0; (i < MAX_CPU_SCHED_PROCS) && stress_continue(args); i++) {
		pid_t pid;

		pid = fork();
		if (pid < 0) {
			stress_cpu_sched_pids[i].pid = -1;
		} else if (pid == 0) {
			pid_t mypid = getpid();
			unsigned int cpu, node;
			int n = (int)mypid % 23;
#if defined(HAVE_SET_MEMPOLICY)
			int mode;
#endif

			/* pid process re-mix mwc */
			while (n-- > 0)
				stress_mwc32();

			shim_nice(1 + stress_mwc8modn(8));
			do {
				switch (stress_mwc8modn(8)) {
				case 0:
					shim_sched_yield();
					break;
				case 1:
					shim_nanosleep_uint64(stress_mwc32modn(25000));
					break;
				case 2:
					if (cap_sys_nice)
						(void)setpriority(PRIO_PROCESS, mypid, 1 + stress_mwc8modn(18));
					break;
				case 3:
					shim_usleep_interruptible(0);
					break;
				case 4:
					(void)shim_getcpu(&cpu, &node, NULL);
					break;
				case 5:
					for (n = 0; n < 1000; n++)
						stress_asm_nop();
					break;
#if defined(HAVE_SET_MEMPOLICY)
				case 6:
					if (node_mask != MAP_FAILED) {
						(void)shim_memset((void *)node_mask, 0, node_mask_size);
						STRESS_SETBIT(node_mask, (int)stress_mwc16modn(max_numa_node));
						mode = mpol_modes[stress_mwc8modn(SIZEOF_ARRAY(mpol_modes))];
						(void)shim_set_mempolicy(mode, node_mask, max_numa_node);
					}
					break;
#endif
				default:
					shim_sched_yield();
					sleep(0);
					break;
				}
			} while (stress_continue(args));
		} else {
			stress_cpu_sched_pids[i].pid = pid;
		}
	}

	do {
		stress_cpu_sched_mix_pids(pids, stress_cpu_sched_pids, MAX_CPU_SCHED_PROCS);

		for (i = 0; (i < MAX_CPU_SCHED_PROCS) && stress_continue(args); i++) {
			pid_t pid = pids[i].pid;

			if (pid == -1)
				continue;

			cpu = stress_cpu_sched_next_cpu(instance, cpu, cpus);

			(void)kill(pid, SIGSTOP);
			if (stress_cpu_sched_setaffinity(args, pid, cpu) < 0) {
				rc = EXIT_FAILURE;
				break;
			}
			if (stress_cpu_sched_setscheduler(args, pid) < 0) {
				rc = EXIT_FAILURE;
				break;
			}
#if defined(HAVE_SETPRIORITY) &&	\
    defined(PRIO_PROCESS)
			if (cap_sys_nice)
				(void)setpriority(PRIO_PROCESS, pid, 1 + stress_mwc8modn(18));
#endif
			(void)kill(pid, SIGCONT);
			stress_bogo_inc(args);
		}
	} while (stress_continue(args));

	(void)stress_kill_and_wait_many(args, stress_cpu_sched_pids, MAX_CPU_SCHED_PROCS, SIGKILL, false);
	return rc;
}

/*
 *  stress_cpu_sched()
 *	stress by cloning and exiting
 */
static int stress_cpu_sched(stress_args_t *args)
{
	int rc;

#if defined(HAVE_SET_MEMPOLICY)
	unsigned long max_nodes = 0;

	max_numa_node = stress_numa_count_mem_nodes(&max_nodes);
	if (max_numa_node > 0) {
		const size_t mask_elements = (max_nodes + NUMA_LONG_BITS - 1) / NUMA_LONG_BITS;
		node_mask_size = mask_elements * sizeof(*node_mask);
		node_mask = (unsigned long *)stress_mmap_populate(NULL, node_mask_size,
				PROT_READ | PROT_WRITE,
				MAP_ANONYMOUS | MAP_SHARED, 0, 0);
	} else {
		max_numa_node = 0;
		node_mask = MAP_FAILED;
		node_mask_size = 0;
	}
#endif

	stress_set_oom_adjustment(args, false);

	stress_set_proc_state(args->name, STRESS_STATE_SYNC_WAIT);
	stress_sync_start_wait(args);
	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	rc = stress_oomable_child(args, NULL, stress_cpu_sched_child, STRESS_OOMABLE_DROP_CAP);
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

#if defined(HAVE_SET_MEMPOLICY)
	if (node_mask != MAP_FAILED)
		(void)munmap((void *)node_mask, node_mask_size);
#endif

	return rc;
}

const stressor_info_t stress_cpu_sched_info = {
	.stressor = stress_cpu_sched,
	.class = CLASS_SCHEDULER | CLASS_OS,
	.verify = VERIFY_ALWAYS,
	.help = help
};

#else

const stressor_info_t stress_cpu_sched_info = {
        .stressor = stress_unimplemented,
	.class = CLASS_SCHEDULER | CLASS_OS,
	.help = help,
	.verify = VERIFY_ALWAYS,
	.unimplemented_reason = "built without Linux scheduling or sched_setscheduler() system call"
};
#endif
