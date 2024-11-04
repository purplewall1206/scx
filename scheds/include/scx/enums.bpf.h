/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Convenience macros for getting/setting struct scx_enums instances.
 *
 * Copyright (c) 2024 Meta Platforms, Inc. and affiliates.
 */
#ifndef __SCX_ENUMS_BPF_H
#define __SCX_ENUMS_BPF_H

#include <scx/enums.h>

#define SCX_ENUM_DEFINE() \
	struct scx_enums _scx_enum;

#define SCX_ENUM(name) \
	(_scx_enum.SCX_##name)

#define SCX_ENUM_SET(enums, etype, name)				\
	do {								\
		if (!bpf_core_enum_value_exists(enum etype, name)) { \
			scx_bpf_error("missing enum %s\n", #name); \
			return -EINVAL;					\
		}							\
		(enums)->name = bpf_core_enum_value(enum etype, name); \
	} while (0)

static inline int
scx_enum_init(struct scx_enums *enums)
{
	SCX_ENUM_SET(enums, scx_public_consts, SCX_OPS_NAME_LEN);
	SCX_ENUM_SET(enums, scx_public_consts, SCX_SLICE_DFL);
	SCX_ENUM_SET(enums, scx_public_consts, SCX_SLICE_INF);

	SCX_ENUM_SET(enums, scx_dsq_id_flags, SCX_DSQ_FLAG_BUILTIN);
	SCX_ENUM_SET(enums, scx_dsq_id_flags, SCX_DSQ_FLAG_LOCAL_ON);
	SCX_ENUM_SET(enums, scx_dsq_id_flags, SCX_DSQ_INVALID);
	SCX_ENUM_SET(enums, scx_dsq_id_flags, SCX_DSQ_GLOBAL);
	SCX_ENUM_SET(enums, scx_dsq_id_flags, SCX_DSQ_LOCAL);
	SCX_ENUM_SET(enums, scx_dsq_id_flags, SCX_DSQ_LOCAL_ON);
	SCX_ENUM_SET(enums, scx_dsq_id_flags, SCX_DSQ_LOCAL_CPU_MASK);

	SCX_ENUM_SET(enums, scx_ent_flags, SCX_TASK_QUEUED);
	SCX_ENUM_SET(enums, scx_ent_flags, SCX_TASK_RESET_RUNNABLE_AT);
	SCX_ENUM_SET(enums, scx_ent_flags, SCX_TASK_DEQD_FOR_SLEEP);
	SCX_ENUM_SET(enums, scx_ent_flags, SCX_TASK_STATE_SHIFT);
	SCX_ENUM_SET(enums, scx_ent_flags, SCX_TASK_STATE_BITS);
	SCX_ENUM_SET(enums, scx_ent_flags, SCX_TASK_STATE_MASK);
	SCX_ENUM_SET(enums, scx_ent_flags, SCX_TASK_CURSOR);

	SCX_ENUM_SET(enums, scx_task_state, SCX_TASK_NONE);
	SCX_ENUM_SET(enums, scx_task_state, SCX_TASK_INIT);
	SCX_ENUM_SET(enums, scx_task_state, SCX_TASK_READY);
	SCX_ENUM_SET(enums, scx_task_state, SCX_TASK_ENABLED);
	SCX_ENUM_SET(enums, scx_task_state, SCX_TASK_NR_STATES);

	SCX_ENUM_SET(enums, scx_ent_dsq_flags, SCX_TASK_DSQ_ON_PRIQ);

	SCX_ENUM_SET(enums, scx_kick_flags, SCX_KICK_IDLE);
	SCX_ENUM_SET(enums, scx_kick_flags, SCX_KICK_PREEMPT);
	SCX_ENUM_SET(enums, scx_kick_flags, SCX_KICK_WAIT);

	SCX_ENUM_SET(enums, scx_enq_flags, SCX_ENQ_WAKEUP);
	SCX_ENUM_SET(enums, scx_enq_flags, SCX_ENQ_HEAD);
	SCX_ENUM_SET(enums, scx_enq_flags, SCX_ENQ_PREEMPT);
	SCX_ENUM_SET(enums, scx_enq_flags, SCX_ENQ_REENQ);
	SCX_ENUM_SET(enums, scx_enq_flags, SCX_ENQ_LAST);
	SCX_ENUM_SET(enums, scx_enq_flags, SCX_ENQ_CLEAR_OPSS);
	SCX_ENUM_SET(enums, scx_enq_flags, SCX_ENQ_DSQ_PRIQ);

	return 0;
}

#define SCX_ENUM_INIT() scx_enum_init(&_scx_enum)

#endif /* __SCX_ENUMS_BPF_H */
