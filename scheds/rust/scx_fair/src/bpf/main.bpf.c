/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2024 Andrea Righi <andrea.righi@linux.dev>
 */
#include <scx/common.bpf.h>
#include "intf.h"

char _license[] SEC("license") = "GPL";

/*
 * Global DSQ used to dispatch tasks.
 */
#define SHARED_DSQ		0

/*
 * Maximum frequency of task wakeup events.
 */
#define MAX_WAKEUP_FREQ		1024

/*
 * Maximum task weight multiplier.
 */
#define MAX_WEIGHT_MUL		10

/*
 * Maximum task weight.
 */
#define MAX_TASK_WEIGHT		10000

/*
 * Task time slice range.
 */
const volatile u64 slice_max = 20ULL * NSEC_PER_MSEC;
const volatile u64 slice_min = 1ULL * NSEC_PER_MSEC;
const volatile u64 slice_lag = 20ULL * NSEC_PER_MSEC;

/*
 * When enabled always dispatch all kthreads directly.
 *
 * This allows to prioritize critical kernel threads that may potentially slow
 * down the entire system if they are blocked for too long, but it may also
 * introduce interactivity issues or unfairness in scenarios with high kthread
 * activity, such as heavy I/O or network traffic.
 */
const volatile bool local_kthreads;

/*
 * Scheduling statistics.
 */
volatile u64 nr_kthread_dispatches, nr_direct_dispatches, nr_shared_dispatches;

/*
 * Exit information.
 */
UEI_DEFINE(uei);

/*
 * Amount of possible CPUs in the system.
 */
static u64 nr_possible_cpus;

/*
 * CPUs in the system have SMT is enabled.
 */
const volatile bool smt_enabled = true;

/*
 * Current global vruntime.
 */
static u64 vtime_now;

/*
 * Per-CPU context.
 */
struct cpu_ctx {
	u64 tot_runtime;
	u64 prev_runtime;
	u64 last_running;
	struct bpf_cpumask __kptr *llc_mask;
};

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__type(key, u32);
	__type(value, struct cpu_ctx);
	__uint(max_entries, 1);
} cpu_ctx_stor SEC(".maps");

/*
 * Return a CPU context.
 */
struct cpu_ctx *try_lookup_cpu_ctx(s32 cpu)
{
	const u32 idx = 0;
	return bpf_map_lookup_percpu_elem(&cpu_ctx_stor, &idx, cpu);
}

/*
 * Per-task local storage.
 *
 * This contain all the per-task information used internally by the BPF code.
 */
struct task_ctx {
	/*
	 * Temporary cpumask for calculating scheduling domains.
	 */
	struct bpf_cpumask __kptr *llc_mask;

	/*
	 * Voluntary context switches metrics.
	 */
	u64 nvcsw;
	u64 nvcsw_ts;
	u64 avg_nvcsw;

	/*
	 * Task's average used time slice.
	 */
	u64 avg_runtime;
	u64 sum_runtime;
	u64 last_run_at;

	/*
	 * Frequency with which a task is blocked (consumer).
	 */
	u64 blocked_freq;
	u64 last_blocked_at;

	/*
	 * Frequency with which a task wakes other tasks (producer).
	 */
	u64 waker_freq;
	u64 last_woke_at;

	/*
	 * Task's deadline.
	 */
	u64 deadline;
};

/* Map that contains task-local storage. */
struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct task_ctx);
} task_ctx_stor SEC(".maps");

/*
 * Return a local task context from a generic task.
 */
struct task_ctx *try_lookup_task_ctx(const struct task_struct *p)
{
	return bpf_task_storage_get(&task_ctx_stor,
					(struct task_struct *)p, 0, 0);
}

/*
 * Allocate/re-allocate a new cpumask.
 */
static int calloc_cpumask(struct bpf_cpumask **p_cpumask)
{
	struct bpf_cpumask *cpumask;

	cpumask = bpf_cpumask_create();
	if (!cpumask)
		return -ENOMEM;

	cpumask = bpf_kptr_xchg(p_cpumask, cpumask);
	if (cpumask)
		bpf_cpumask_release(cpumask);

	return 0;
}

/*
 * Exponential weighted moving average (EWMA).
 *
 * Copied from scx_lavd. Returns the new average as:
 *
 *	new_avg := (old_avg * .75) + (new_val * .25);
 */
static u64 calc_avg(u64 old_val, u64 new_val)
{
	return (old_val - (old_val >> 2)) + (new_val >> 2);
}

/*
 * Evaluate the EWMA limited to the range [low ... high]
 */
static u64 calc_avg_clamp(u64 old_val, u64 new_val, u64 low, u64 high)
{
	return CLAMP(calc_avg(old_val, new_val), low, high);
}

/*
 * Evaluate the average frequency of an event over time.
 */
static u64 update_freq(u64 freq, u64 delta)
{
	u64 new_freq;

	new_freq = NSEC_PER_SEC / delta;
	return calc_avg(freq, new_freq);
}

/*
 * Compare two vruntime values, returns true if the first value is less than
 * the second one.
 *
 * Copied from scx_simple.
 */
static inline bool vtime_before(u64 a, u64 b)
{
	return (s64)(a - b) < 0;
}

/*
 * Return true if the target task @p is a kernel thread, false instead.
 */
static inline bool is_kthread(const struct task_struct *p)
{
	return p->flags & PF_KTHREAD;
}

/*
 * Return the amount of tasks that are waiting to run.
 */
static inline u64 nr_tasks_waiting(void)
{
	return scx_bpf_dsq_nr_queued(SHARED_DSQ) + 1;
}

/*
 * Return task's dynamic weight.
 */
static u64 task_weight(const struct task_struct *p, const struct task_ctx *tctx)
{
	u64 prio = p->scx.weight * CLAMP(tctx->avg_nvcsw, 1, 100);

	return CLAMP(prio, 1, MAX_TASK_WEIGHT);
}

/*
 * Return a value proportionally scaled to the task's priority.
 */
static u64 scale_up_fair(const struct task_struct *p,
			 const struct task_ctx *tctx, u64 value)
{
	return value * task_weight(p, tctx) / 100;
}

/*
 * Return a value inversely proportional to the task's priority.
 */
static u64 scale_inverse_fair(const struct task_struct *p,
			      const struct task_ctx *tctx, u64 value)
{
	return value * 100 / task_weight(p, tctx);
}

/*
 * Return the task's allowed lag: used to determine how early its vruntime can
 * be.
 */
static u64 task_lag(const struct task_struct *p, const struct task_ctx *tctx)
{
	return scale_up_fair(p, tctx, slice_lag);
}

/*
 * ** Taken directly from fair.c in the Linux kernel **
 *
 * The "10% effect" is relative and cumulative: from _any_ nice level,
 * if you go up 1 level, it's -10% CPU usage, if you go down 1 level
 * it's +10% CPU usage. (to achieve that we use a multiplier of 1.25.
 * If a task goes up by ~10% and another task goes down by ~10% then
 * the relative distance between them is ~25%.)
 */
const int sched_prio_to_weight[40] = {
 /* -20 */     88761,     71755,     56483,     46273,     36291,
 /* -15 */     29154,     23254,     18705,     14949,     11916,
 /* -10 */      9548,      7620,      6100,      4904,      3906,
 /*  -5 */      3121,      2501,      1991,      1586,      1277,
 /*   0 */      1024,       820,       655,       526,       423,
 /*   5 */       335,       272,       215,       172,       137,
 /*  10 */       110,        87,        70,        56,        45,
 /*  15 */        36,        29,        23,        18,        15,
};

static u64 max_sched_prio(void)
{
	return ARRAY_SIZE(sched_prio_to_weight);
}

/*
 * Convert task priority to weight (following fair.c logic).
 */
static u64 sched_prio_to_latency_weight(u64 prio)
{
	u64 max_prio = max_sched_prio();

	if (prio >= max_prio) {
		scx_bpf_error("invalid priority");
		return 0;
	}

	return sched_prio_to_weight[max_prio - prio - 1];
}

/*
 * Evaluate task's deadline.
 *
 * Reuse a logic similar to scx_rusty or scx_lavd and evaluate the deadline as
 * a function of the waiting and wake-up events and the average task's runtime.
 */
static u64 task_deadline(struct task_struct *p, struct task_ctx *tctx)
{
	u64 waker_freq, blocked_freq;
	u64 lat_prio, lat_weight;
	u64 avg_run_scaled, avg_run;
	u64 freq_factor;

	/*
	 * Limit the wait and wake-up frequencies to prevent spikes.
	 */
	waker_freq = CLAMP(tctx->waker_freq, 1, MAX_WAKEUP_FREQ);
	blocked_freq = CLAMP(tctx->blocked_freq, 1, MAX_WAKEUP_FREQ);

	/*
	 * We want to prioritize producers (waker tasks) more than consumers
	 * (blocked tasks), using the following formula:
	 *
	 *   freq_factor = blocked_freq * waker_freq^2
	 *
	 * This seems to improve the overall responsiveness of
	 * producer/consumer pipelines.
	 */
	freq_factor = blocked_freq * waker_freq * waker_freq;

	/*
	 * Evaluate the "latency priority" as a function of the wake-up, block
	 * frequencies and average runtime, using the following formula:
	 *
	 *   lat_prio = log(freq_factor / avg_run_scaled)
	 *
	 * Frequencies can grow very quickly, almost exponential, so use
	 * log2_u64() to get a more linear metric that can be used as a
	 * priority.
	 *
	 * The avg_run_scaled component is used to scale the latency priority
	 * proportionally to the task's weight and inversely proportional to
	 * its runtime, so that a task with a higher weight / shorter runtime
	 * gets a higher latency priority than a task with a lower weight /
	 * higher runtime.
	 */
	avg_run_scaled = scale_inverse_fair(p, tctx, tctx->avg_runtime);
	avg_run = log2_u64(avg_run_scaled + 1);

	lat_prio = log2_u64(freq_factor);
	lat_prio = MIN(lat_prio, max_sched_prio());

	if (lat_prio >= avg_run)
		lat_prio -= avg_run;
	else
		lat_prio = 0;

	/*
	 * Lastly, translate the latency priority into a weight and apply it to
	 * the task's average runtime to determine the task's deadline.
	 */
	lat_weight = sched_prio_to_latency_weight(lat_prio);

	return tctx->avg_runtime * 100 / lat_weight;
}

/*
 * Return task's evaluated deadline applied to its vruntime.
 */
static u64 task_vtime(struct task_struct *p, struct task_ctx *tctx)
{
	u64 min_vruntime = vtime_now - task_lag(p, tctx);

	/*
	 * Limit the vruntime to to avoid excessively penalizing tasks.
	 */
	if (vtime_before(p->scx.dsq_vtime, min_vruntime)) {
		p->scx.dsq_vtime = min_vruntime;
		tctx->deadline = p->scx.dsq_vtime + task_deadline(p, tctx);
	}

	return tctx->deadline;
}

/*
 * Evaluate task's time slice in function of the total amount of tasks that are
 * waiting to be dispatched and the task's weight.
 */
static void task_refill_slice(struct task_struct *p)
{
	p->scx.slice = CLAMP(slice_max / nr_tasks_waiting(), slice_min, slice_max);
}

static void task_set_domain(struct task_struct *p, s32 cpu,
			    const struct cpumask *cpumask)
{
	const struct cpumask *llc_domain;
	struct bpf_cpumask *llc_mask;
	struct task_ctx *tctx;
	struct cpu_ctx *cctx;

	tctx = try_lookup_task_ctx(p);
	if (!tctx)
		return;

	cctx = try_lookup_cpu_ctx(cpu);
	if (!cctx)
		return;

	llc_domain = cast_mask(cctx->llc_mask);
	if (!llc_domain)
		llc_domain = p->cpus_ptr;

	llc_mask = tctx->llc_mask;
	if (!llc_mask) {
		scx_bpf_error("LLC cpumask not initialized");
		return;
	}

	/*
	 * Determine the LLC cache domain as the intersection of the task's
	 * primary cpumask and the LLC cache domain mask of the previously used
	 * CPU.
	 */
	bpf_cpumask_and(llc_mask, p->cpus_ptr, llc_domain);
}

/*
 * Find an idle CPU in the system.
 *
 * NOTE: the idle CPU selection doesn't need to be formally perfect, it is
 * totally fine to accept racy conditions and potentially make mistakes, by
 * picking CPUs that are not idle or even offline, the logic has been designed
 * to handle these mistakes in favor of a more efficient response and a reduced
 * scheduling overhead.
 */
static s32 pick_idle_cpu(struct task_struct *p, s32 prev_cpu, u64 wake_flags, bool *is_idle)
{
	const struct cpumask *idle_smtmask, *idle_cpumask;
	const struct cpumask *llc_mask;
	struct task_ctx *tctx;
	bool is_prev_llc_affine = false;
	s32 cpu;

	*is_idle = false;

	/*
	 * For tasks that can run only on a single CPU, we can simply verify if
	 * their only allowed CPU is still idle.
	 */
	if (p->nr_cpus_allowed == 1 || p->migration_disabled) {
		if (scx_bpf_test_and_clear_cpu_idle(prev_cpu))
			*is_idle = true;
		return prev_cpu;
	}

	tctx = try_lookup_task_ctx(p);
	if (!tctx)
		return -ENOENT;

	/*
	 * Acquire the CPU masks to determine the idle CPUs in the system.
	 */
	idle_smtmask = scx_bpf_get_idle_smtmask();
	idle_cpumask = scx_bpf_get_idle_cpumask();

	/*
	 * Task's scheduling domains.
	 */
	llc_mask = cast_mask(tctx->llc_mask);
	if (!llc_mask) {
		scx_bpf_error("LLC cpumask not initialized");
		cpu = -EINVAL;
		goto out_put_cpumask;
	}

	/*
	 * Check if the previously used CPU is still in the LLC task domain. If
	 * not, we may want to move the task back to its original LLC domain.
	 */
	is_prev_llc_affine = bpf_cpumask_test_cpu(prev_cpu, llc_mask);

	/*
	 * If the current task is waking up another task and releasing the CPU
	 * (WAKE_SYNC), attempt to migrate the wakee on the same CPU as the
	 * waker.
	 */
	if (wake_flags & SCX_WAKE_SYNC) {
		struct task_struct *current = (void *)bpf_get_current_task_btf();
		const struct cpumask *curr_llc_domain;
		struct cpu_ctx *cctx;
		bool share_llc, has_idle;

		/*
		 * Determine waker CPU scheduling domain.
		 */
		cpu = bpf_get_smp_processor_id();
		cctx = try_lookup_cpu_ctx(cpu);
		if (!cctx) {
			cpu = -EINVAL;
			goto out_put_cpumask;
		}

		curr_llc_domain = cast_mask(cctx->llc_mask);
		if (!curr_llc_domain)
			curr_llc_domain = p->cpus_ptr;

		/*
		 * If both the waker and wakee share the same LLC domain keep
		 * using the same CPU if possible.
		 */
		share_llc = bpf_cpumask_test_cpu(prev_cpu, curr_llc_domain);
		if (share_llc &&
		    scx_bpf_test_and_clear_cpu_idle(prev_cpu)) {
			cpu = prev_cpu;
			*is_idle = true;
			goto out_put_cpumask;
		}

		/*
		 * If the waker's LLC domain is not saturated attempt to migrate
		 * the wakee on the same CPU as the waker (since it's going to
		 * block and release the current CPU).
		 */
		has_idle = bpf_cpumask_intersects(curr_llc_domain, idle_cpumask);
		if (has_idle &&
		    bpf_cpumask_test_cpu(cpu, p->cpus_ptr) &&
		    !(current->flags & PF_EXITING) &&
		    scx_bpf_dsq_nr_queued(SCX_DSQ_LOCAL_ON | cpu) == 0) {
			*is_idle = true;
			goto out_put_cpumask;
		}
	}

	/*
	 * Find the best idle CPU, prioritizing full idle cores in SMT systems.
	 */
	if (smt_enabled) {
		/*
		 * If the task can still run on the previously used CPU and
		 * it's a full-idle core, keep using it.
		 */
		if (is_prev_llc_affine &&
		    bpf_cpumask_test_cpu(prev_cpu, idle_smtmask) &&
		    scx_bpf_test_and_clear_cpu_idle(prev_cpu)) {
			cpu = prev_cpu;
			*is_idle = true;
			goto out_put_cpumask;
		}

		/*
		 * Search for any full-idle CPU in the primary domain that
		 * shares the same LLC domain.
		 */
		cpu = scx_bpf_pick_idle_cpu(llc_mask, SCX_PICK_IDLE_CORE);
		if (cpu >= 0) {
			*is_idle = true;
			goto out_put_cpumask;
		}

		/*
		 * Search for any other full-idle core in the task's domain.
		 */
		cpu = scx_bpf_pick_idle_cpu(p->cpus_ptr, SCX_PICK_IDLE_CORE);
		if (cpu >= 0) {
			*is_idle = true;
			goto out_put_cpumask;
		}
	}

	/*
	 * If a full-idle core can't be found (or if this is not an SMT system)
	 * try to re-use the same CPU, even if it's not in a full-idle core.
	 */
	if (is_prev_llc_affine &&
	    scx_bpf_test_and_clear_cpu_idle(prev_cpu)) {
		cpu = prev_cpu;
		*is_idle = true;
		goto out_put_cpumask;
	}

	/*
	 * Search for any idle CPU in the primary domain that shares the same
	 * LLC domain.
	 */
	cpu = scx_bpf_pick_idle_cpu(llc_mask, 0);
	if (cpu >= 0) {
		*is_idle = true;
		goto out_put_cpumask;
	}

	/*
	 * Search for any idle CPU in the task's scheduling domain.
	 */
	cpu = scx_bpf_pick_idle_cpu(p->cpus_ptr, 0);
	if (cpu >= 0) {
		*is_idle = true;
		goto out_put_cpumask;
	}

	/*
	 * We couldn't find any idle CPU, return the previous CPU if it is in
	 * the task's LLC domain, otherwise pick any other CPU in the LLC
	 * domain.
	 */
	if (is_prev_llc_affine)
		cpu = prev_cpu;
	else
		cpu = scx_bpf_pick_any_cpu(llc_mask, 0);

out_put_cpumask:
	scx_bpf_put_cpumask(idle_cpumask);
	scx_bpf_put_cpumask(idle_smtmask);

	/*
	 * If we couldn't find any CPU, or in case of error, return the
	 * previously used CPU.
	 */
	if (cpu < 0)
		cpu = prev_cpu;

	return cpu;
}

/*
 * Pick a target CPU for a task which is being woken up.
 *
 * If a task is dispatched here, ops.enqueue() will be skipped: task will be
 * dispatched directly to the CPU returned by this callback.
 */
s32 BPF_STRUCT_OPS(fair_select_cpu, struct task_struct *p,
			s32 prev_cpu, u64 wake_flags)
{
	bool is_idle = false;
	s32 cpu;

	cpu = pick_idle_cpu(p, prev_cpu, wake_flags, &is_idle);
	if (is_idle) {
		scx_bpf_dispatch(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL, 0);
		__sync_fetch_and_add(&nr_direct_dispatches, 1);
	}

	return cpu;
}

/*
 * Wake up an idle CPU for task @p.
 */
static void kick_task_cpu(struct task_struct *p)
{
	s32 cpu = scx_bpf_task_cpu(p);
	bool is_idle = false;

	cpu = pick_idle_cpu(p, cpu, 0, &is_idle);
	if (is_idle)
		scx_bpf_kick_cpu(cpu, 0);
}

/*
 * Dispatch all the other tasks that were not dispatched directly in
 * select_cpu().
 */
void BPF_STRUCT_OPS(fair_enqueue, struct task_struct *p, u64 enq_flags)
{
	struct task_ctx *tctx;

	tctx = try_lookup_task_ctx(p);
	if (!tctx)
		return;

	/*
	 * Per-CPU kthreads can be critical for system responsiveness, when
	 * local_kthreads is specified they are always dispatched directly
	 * before any other task.
	 */
	if (local_kthreads && is_kthread(p) && p->nr_cpus_allowed == 1) {
		scx_bpf_dispatch(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL,
				 enq_flags | SCX_ENQ_PREEMPT);
		__sync_fetch_and_add(&nr_kthread_dispatches, 1);
		return;
	}

	/*
	 * Enqueue the task to the global DSQ. The task will be dispatched on
	 * the first CPU that becomes available.
	 */
	scx_bpf_dispatch_vtime(p, SHARED_DSQ, SCX_SLICE_DFL,
			       task_vtime(p, tctx), enq_flags);
	__sync_fetch_and_add(&nr_shared_dispatches, 1);

	/*
	 * If there is an idle CPU available for the task, wake it up so it can
	 * consume the task immediately.
	 */
	kick_task_cpu(p);
}

void BPF_STRUCT_OPS(fair_dispatch, s32 cpu, struct task_struct *prev)
{
	if (scx_bpf_consume(SHARED_DSQ))
		return;

	/*
	 * If the current task expired its time slice and no other task wants
	 * to run, simply replenish its time slice and let it run for another
	 * round on the same CPU.
	 */
	if (prev && (prev->scx.flags & SCX_TASK_QUEUED))
		task_refill_slice(prev);
}

void BPF_STRUCT_OPS(fair_running, struct task_struct *p)
{
	struct task_ctx *tctx;

	/*
	 * Refresh task's time slice immediately before it starts to run on its
	 * assigned CPU.
	 */
	task_refill_slice(p);

	tctx = try_lookup_task_ctx(p);
	if (!tctx)
		return;
	tctx->last_run_at = bpf_ktime_get_ns();

	/*
	 * Update global vruntime.
	 */
	if (vtime_before(vtime_now, p->scx.dsq_vtime))
		vtime_now = p->scx.dsq_vtime;
}

void BPF_STRUCT_OPS(fair_stopping, struct task_struct *p, bool runnable)
{
	u64 now = bpf_ktime_get_ns(), slice;
	s32 cpu = scx_bpf_task_cpu(p);
	s64 delta_t;
	struct cpu_ctx *cctx;
	struct task_ctx *tctx;

	cctx = try_lookup_cpu_ctx(cpu);
	if (cctx)
		cctx->tot_runtime += now - cctx->last_running;

	tctx = try_lookup_task_ctx(p);
	if (!tctx)
		return;

	/*
	 * If the time slice is not fully depleted, it means that the task
	 * voluntarily relased the CPU, therefore update the voluntary context
	 * switch counter.
	 *
	 * NOTE: the sched_ext core implements sched_yield() by setting the
	 * time slice to 0, so we won't boost the priority of tasks that are
	 * explicitly calling sched_yield().
	 *
	 * This is actually a good thing, because we want to prioritize tasks
	 * that are releasing the CPU, because they're doing I/O, waiting for
	 * input or sending output to other tasks.
	 *
	 * Tasks that are using sched_yield() don't really need the priority
	 * boost and when they get the chance to run again they will be
	 * naturally prioritized by the vruntime-based scheduling policy.
	 */
	if (p->scx.slice > 0)
		tctx->nvcsw++;

	/*
	 * Update task's average runtime.
	 */
	slice = now - tctx->last_run_at;
	tctx->sum_runtime += slice;
	tctx->avg_runtime = calc_avg(tctx->avg_runtime, tctx->sum_runtime);

	/*
	 * Update task vruntime charging the weighted used time slice.
	 */
	p->scx.dsq_vtime += scale_inverse_fair(p, tctx, slice);
	tctx->deadline = p->scx.dsq_vtime + task_deadline(p, tctx);

	/*
	 * Refresh voluntary context switch metrics.
	 *
	 * Evaluate the average number of voluntary context switches per second
	 * using an exponentially weighted moving average, see calc_avg().
	 */
	delta_t = (s64)(now - tctx->nvcsw_ts);
	if (delta_t > NSEC_PER_SEC) {
		u64 avg_nvcsw = tctx->nvcsw * NSEC_PER_SEC / delta_t;

		tctx->nvcsw = 0;
		tctx->nvcsw_ts = now;

		/*
		 * Evaluate the latency weight of the task as its average rate
		 * of voluntary context switches (limited to to prevent
		 * excessive spikes).
		 */
		tctx->avg_nvcsw = calc_avg_clamp(tctx->avg_nvcsw, avg_nvcsw,
						 0, MAX_WAKEUP_FREQ);
	}
}

void BPF_STRUCT_OPS(fair_runnable, struct task_struct *p, u64 enq_flags)
{
	u64 now = bpf_ktime_get_ns(), delta;
	struct task_struct *waker;
	struct task_ctx *tctx;

	waker = bpf_get_current_task_btf();
	tctx = try_lookup_task_ctx(waker);
	if (!tctx)
		return;
	tctx->sum_runtime = 0;

	delta = MAX(now - tctx->last_woke_at, 1);
	tctx->waker_freq = update_freq(tctx->waker_freq, delta);
	tctx->last_woke_at = now;
}

void BPF_STRUCT_OPS(fair_quiescent, struct task_struct *p, u64 deq_flags)
{
	u64 now = bpf_ktime_get_ns(), delta;
	struct task_ctx *tctx;

	tctx = try_lookup_task_ctx(p);
	if (!tctx)
		return;

	delta = MAX(now - tctx->last_blocked_at, 1);
	tctx->blocked_freq = update_freq(tctx->blocked_freq, delta);
	tctx->last_blocked_at = now;
}

void BPF_STRUCT_OPS(fair_set_cpumask, struct task_struct *p,
		    const struct cpumask *cpumask)
{
	s32 cpu = bpf_get_smp_processor_id();

	task_set_domain(p, cpu, cpumask);
}

void BPF_STRUCT_OPS(fair_enable, struct task_struct *p)
{
	u64 now = bpf_ktime_get_ns();
	struct task_ctx *tctx;

	p->scx.dsq_vtime = vtime_now;

	tctx = try_lookup_task_ctx(p);
	if (!tctx) {
		scx_bpf_error("incorrectly initialized task: %d (%s)",
			      p->pid, p->comm);
		return;
	}
	tctx->nvcsw_ts = now;
	tctx->last_woke_at = now;
	tctx->last_blocked_at = now;

	tctx->deadline = p->scx.dsq_vtime + task_deadline(p, tctx);
}

s32 BPF_STRUCT_OPS(fair_init_task, struct task_struct *p,
		   struct scx_init_task_args *args)
{
	s32 cpu = bpf_get_smp_processor_id();
	struct task_ctx *tctx;
	struct bpf_cpumask *cpumask;

	tctx = bpf_task_storage_get(&task_ctx_stor, p, 0,
				    BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!tctx)
		return -ENOMEM;
	/*
	 * Create task's LLC cpumask.
	 */
	cpumask = bpf_cpumask_create();
	if (!cpumask)
		return -ENOMEM;
	cpumask = bpf_kptr_xchg(&tctx->llc_mask, cpumask);
	if (cpumask)
		bpf_cpumask_release(cpumask);

	task_set_domain(p, cpu, p->cpus_ptr);

	return 0;
}

static int init_cpumask(struct bpf_cpumask **cpumask)
{
	struct bpf_cpumask *mask;
	int err = 0;

	/*
	 * Do nothing if the mask is already initialized.
	 */
	mask = *cpumask;
	if (mask)
		return 0;
	/*
	 * Create the CPU mask.
	 */
	err = calloc_cpumask(cpumask);
	if (!err)
		mask = *cpumask;
	if (!mask)
		err = -ENOMEM;

	return err;
}

SEC("syscall")
int enable_sibling_cpu(struct domain_arg *input)
{
	struct cpu_ctx *cctx;
	struct bpf_cpumask *mask, **pmask;
	int err = 0;

	cctx = try_lookup_cpu_ctx(input->cpu_id);
	if (!cctx)
		return -ENOENT;

	/* Make sure the target CPU mask is initialized */
	pmask = &cctx->llc_mask;
	err = init_cpumask(pmask);
	if (err)
		return err;

	bpf_rcu_read_lock();
	mask = *pmask;
	if (mask)
		bpf_cpumask_set_cpu(input->sibling_cpu_id, mask);
	bpf_rcu_read_unlock();

	return err;
}

static s32 get_nr_possible_cpus(void)
{
	const struct cpumask *possible_cpumask;
	s32 cnt;

	possible_cpumask = scx_bpf_get_possible_cpumask();
	cnt = bpf_cpumask_weight(possible_cpumask);
	scx_bpf_put_cpumask(possible_cpumask);

	return cnt;
}

s32 BPF_STRUCT_OPS_SLEEPABLE(fair_init)
{
	int err;

	nr_possible_cpus = get_nr_possible_cpus();

	/*
	 * Create the shared DSQ.
	 *
	 * Allocate the new DSQ id to not clash with any valid CPU id.
	 */
	err = scx_bpf_create_dsq(SHARED_DSQ, -1);
	if (err) {
		scx_bpf_error("failed to create shared DSQ: %d", err);
		return err;
	}

	return 0;
}

void BPF_STRUCT_OPS(fair_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(fair_ops,
	       .select_cpu		= (void *)fair_select_cpu,
	       .enqueue			= (void *)fair_enqueue,
	       .dispatch		= (void *)fair_dispatch,
	       .running			= (void *)fair_running,
	       .stopping		= (void *)fair_stopping,
	       .runnable		= (void *)fair_runnable,
	       .quiescent		= (void *)fair_quiescent,
	       .set_cpumask		= (void *)fair_set_cpumask,
	       .enable			= (void *)fair_enable,
	       .init_task		= (void *)fair_init_task,
	       .init			= (void *)fair_init,
	       .exit			= (void *)fair_exit,
	       .flags			= SCX_OPS_ENQ_EXITING,
	       .timeout_ms		= 5000,
	       .name			= "fair");
