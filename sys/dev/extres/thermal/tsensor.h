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

#ifndef DEV_EXTRES_TSENSOR_H
#define DEV_EXTRES_TSENSOR_H
#include "opt_platform.h"

#include <sys/kobj.h>
#ifdef FDT
#include <dev/ofw/ofw_bus.h>
#endif
#include "tsnode_if.h"

typedef struct tsensor *tsensor_t;

#define TSENSOR_FLAGS_STATIC		0x00000001  /* Static strings */

/* Initialization parameters. */
struct tsnode_init_def {
	char			*name;		/* Sensor name */
	intptr_t		id;		/* Sensor ID */
	int			flags;		/* Flags */
#ifdef FDT
	phandle_t 		ofw_node;	/* OFW node of sensor */
#endif
};

/*
 * Shorthands for constructing method tables.
 */
#define	TSNODEMETHOD		KOBJMETHOD
#define	TSNODEMETHOD_END	KOBJMETHOD_END
#define tsnode_method_t		kobj_method_t
#define tsnode_class_t		kobj_class_t
DECLARE_CLASS(tsnode_class);

/*
 * Provider interface
 */
struct tsnode *tsnode_create(device_t pdev, tsnode_class_t tsnode_class,
    struct tsnode_init_def *def);
struct tsnode *tsnode_register(struct tsnode *tsnode);
void *tsnode_get_softc(struct tsnode *tsnode);
device_t tsnode_get_device(struct tsnode *tsnode);
intptr_t tsnode_get_id(struct tsnode *tsnode);
int tsnode_temperature(struct tsnode *tsnode, int *value);
#ifdef FDT
phandle_t tsnode_get_ofw_node(struct tsnode *tsnode);
#endif

/*
 * Consumer interface
 */
int tsensor_get_by_id(device_t consumer_dev, device_t provider_dev, intptr_t id,
    tsensor_t *tsensor);
void tsensor_release(tsensor_t tsensor);
int tsensor_temperature(tsensor_t tsensor, int *value);

#ifdef FDT
int tsensor_get_by_ofw_name(device_t consumer, phandle_t node, char *name,
    tsensor_t *tsensor);
int tsensor_get_by_ofw_idx(device_t consumer, phandle_t node, int idx,
    tsensor_t *tsensor);
int tsensor_get_by_ofw_property(device_t consumer, phandle_t node, char *name,
    tsensor_t *tsensor);
#endif

#endif /* DEV_EXTRES_TSENSOR_H */
