/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MEMCOMP_H
#define _LINUX_MEMCOMP_H

#include <linux/types.h>

/* The encoding reservation alone does not provide a compression backend. */
static inline bool memcomp_available(void)
{
	return false;
}

#endif /* _LINUX_MEMCOMP_H */
