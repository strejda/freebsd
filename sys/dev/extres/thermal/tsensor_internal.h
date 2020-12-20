/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright 2020 Michal Meloun <mmel@FreeBSD.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef DEV_EXTRES_TSENSOR_INTERNAL_H
#define DEV_EXTRES_TSENSOR_INTERNAL_H

/* Forward declarations. */
struct tsensor;
struct tsnode;

typedef TAILQ_HEAD(tsnode_list, tsnode) tsnode_list_t;
typedef TAILQ_HEAD(tsensor_list, tsensor) tsensor_list_t;

SYSCTL_DECL(_hw_temperature);
/*
 * tsensor node
 */
struct tsnode {
	KOBJ_FIELDS;

	TAILQ_ENTRY(tsnode)	tslist_link; /* Global list entry */
	tsensor_list_t		consumers_list;	/* Consumers list */


	/* Details of this device. */
	const char		*name;		/* Globally unique name */

	device_t		pdev;		/* Producer device_t */
	void			*softc;		/* Producer softc */
	intptr_t		id;		/* Per producer unique id */
#ifdef FDT
	 phandle_t		ofw_node;	/* OFW node of tsensor */
#endif
	struct sx		lock;		/* Lock for this tsensor */
	int			ref_cnt;	/* Reference counter */
	struct sysctl_ctx_list	sysctl_ctx;
};

struct tsensor {
	device_t		cdev;		/* consumer device*/
	struct tsnode		*tsnode;
	TAILQ_ENTRY(tsensor)	link;		/* Consumers list entry */
};


#define TSENSOR_TOPO_SLOCK()	sx_slock(&tsnode_topo_lock)
#define TSENSOR_TOPO_XLOCK()	sx_xlock(&tsnode_topo_lock)
#define TSENSOR_TOPO_UNLOCK()	sx_unlock(&tsnode_topo_lock)
#define TSENSOR_TOPO_ASSERT()	sx_assert(&tsnode_topo_lock, SA_LOCKED)
#define TSENSOR_TOPO_XASSERT() 	sx_assert(&tsnode_topo_lock, SA_XLOCKED)

#define TSNODE_SLOCK(_sc)	sx_slock(&((_sc)->lock))
#define TSNODE_XLOCK(_sc)	sx_xlock(&((_sc)->lock))
#define TSNODE_UNLOCK(_sc)	sx_unlock(&((_sc)->lock))

extern struct sx tsnode_topo_lock;

#endif /* DEV_EXTRES_TSENSOR_INTERNAL_H */
