/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIC_LOG_H
#define AIC_LOG_H

#include <linux/printk.h>

#define LOGERROR	0x0001
#define LOGINFO		0x0002
#define LOGTRACE	0x0004
#define LOGDEBUG	0x0008
#define LOGDATA		0x0010
#define LOGSTEER	0x0020
#define LOGFW		0x0200

#define AIC_KERN_LOGERROR	KERN_ERR
#define AIC_KERN_LOGINFO	KERN_INFO
#define AIC_KERN_LOGTRACE	KERN_DEBUG
#define AIC_KERN_LOGDEBUG	KERN_DEBUG
#define AIC_KERN_LOGDATA	KERN_DEBUG
#define AIC_KERN_LOGSTEER	KERN_DEBUG
#define AIC_KERN_LOGFW		KERN_DEBUG

extern int aicwf_dbg_level;

#define AICWFDBG(level, fmt, ...)					\
	do {								\
		if (aicwf_dbg_level & (level))				\
			printk(AIC_KERN_##level "AICWFDBG(" #level ")\t"	\
			       fmt, ##__VA_ARGS__);			\
	} while (0)

#define RWNX_DBG(fmt, ...) AICWFDBG(LOGTRACE, fmt, ##__VA_ARGS__)

#endif
