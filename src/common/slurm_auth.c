/*****************************************************************************\
 *  slurm_auth.c - implementation-independent authentication API definitions
 *****************************************************************************
 *  Copyright (C) 2002-2007 The Regents of the University of California.
 *  Copyright (C) 2008-2009 Lawrence Livermore National Security.
 *  Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 *  Written by Jay Windley <jwindley@lnxi.com>
 *  CODE-OCEC-09-009. All rights reserved.
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include <stdlib.h>
#include <string.h>

#include <pthread.h>

#include "src/common/macros.h"
#include "src/common/plugin.h"
#include "src/common/plugrack.h"
#include "src/common/slurm_auth.h"
#include "src/common/slurm_protocol_api.h"
#include "src/common/xassert.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

static bool init_run = false;

typedef struct {
	uint32_t	(*plugin_id);
	char		(*plugin_type);
	void *		(*create)	(char *auth_info);
	int		(*destroy)	(void *cred);
	int		(*verify)	(void *cred, char *auth_info);
	uid_t		(*get_uid)	(void *cred, char *auth_info);
	gid_t		(*get_gid)	(void *cred, char *auth_info);
	char *		(*get_host)	(void *cred, char *auth_info);
	int		(*pack)		(void *cred, Buf buf,
					 uint16_t protocol_version);
	void *		(*unpack)	(Buf buf, uint16_t protocol_version);
	int		(*print)	(void *cred, FILE *fp);
	int		(*sa_errno)	(void *cred);
	const char *	(*sa_errstr)	(int slurm_errno);
} slurm_auth_ops_t;
/*
 * These strings must be kept in the same order as the fields
 * declared for slurm_auth_ops_t.
 */
static const char *syms[] = {
	"plugin_id",
	"plugin_type",
	"slurm_auth_create",
	"slurm_auth_destroy",
	"slurm_auth_verify",
	"slurm_auth_get_uid",
	"slurm_auth_get_gid",
	"slurm_auth_get_host",
	"slurm_auth_pack",
	"slurm_auth_unpack",
	"slurm_auth_print",
	"slurm_auth_errno",
	"slurm_auth_errstr",
};

/*
 * A global authentication context.  "Global" in the sense that there's
 * only one, with static bindings.  We don't export it.
 */
static slurm_auth_ops_t *ops = NULL;
static plugin_context_t **g_context = NULL;
static int g_context_num = -1;
static pthread_mutex_t context_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *slurm_auth_generic_errstr(int slurm_errno)
{
	static struct {
		int err;
		const char *msg;
	} generic_table[] = {
		{ SLURM_SUCCESS, "no error" },
		{ SLURM_ERROR, "unknown error" },
		{ SLURM_AUTH_NOPLUGIN, "no authentication plugin installed" },
		{ SLURM_AUTH_BADARG, "bad argument to plugin function" },
		{ SLURM_AUTH_MEMORY, "memory management error" },
		{ SLURM_AUTH_NOUSER, "no such user" },
		{ SLURM_AUTH_INVALID, "authentication credential invalid" },
		{ SLURM_AUTH_MISMATCH, "authentication type mismatch" },
		{ SLURM_AUTH_VERSION, "authentication version too old" },
		{ 0, NULL }
	};

	int i;

	for (i = 0; ; ++i) {
		if (generic_table[i].msg == NULL)
			return NULL;
		if (generic_table[i].err == slurm_errno)
			return generic_table[i].msg;
	}
}

extern int slurm_auth_init(char *auth_type)
{
	int retval = SLURM_SUCCESS;
	char *type = NULL;
	char *plugin_type = "auth";

	if (init_run && (g_context_num > 0))
		return retval;

	slurm_mutex_lock(&context_lock);

	if (g_context_num > 0)
		goto done;

	if (auth_type)
		slurm_set_auth_type(auth_type);

	type = slurm_get_auth_type();

	g_context_num = 0;

	xrealloc(ops, sizeof(slurm_auth_ops_t) * (g_context_num + 1));
	xrealloc(g_context, sizeof(plugin_context_t) * (g_context_num + 1));

	g_context[g_context_num] = plugin_context_create(
		plugin_type, type, (void **)ops, syms, sizeof(syms));

	if (!g_context[g_context_num]) {
		error("cannot create %s context for %s", plugin_type, type);
		retval = SLURM_ERROR;
		goto done;
	}
	g_context_num++;
	init_run = true;

done:
	xfree(type);
	slurm_mutex_unlock(&context_lock);
	return retval;
}

/* Release all global memory associated with the plugin */
extern int slurm_auth_fini(void)
{
	int i, rc = SLURM_SUCCESS, rc2;

	slurm_mutex_lock(&context_lock);
	if (!g_context)
		goto done;

	init_run = false;

	for (i = 0; i < g_context_num; i++) {
		rc2 = plugin_context_destroy(g_context[i]);
		if (rc2) {
			debug("%s: %s: %s",
			      __func__, g_context[i]->type,
			      slurm_strerror(rc2));
			rc = SLURM_ERROR;
		}
	}

	xfree(ops);
	xfree(g_context);
	g_context_num = -1;

done:
	slurm_mutex_unlock(&context_lock);
	return rc;
}

/*
 * Static bindings for the global authentication context.  The test
 * of the function pointers is omitted here because the global
 * context initialization includes a test for the completeness of
 * the API function dispatcher.
 */

void *g_slurm_auth_create(char *auth_info)
{
	if (slurm_auth_init(NULL) < 0)
		return NULL;

	return (*(ops[0].create))(auth_info);
}

int g_slurm_auth_destroy(void *cred)
{
	if (slurm_auth_init(NULL) < 0)
		return SLURM_ERROR;

	return (*(ops[0].destroy))(cred);
}

int g_slurm_auth_verify(void *cred, char *auth_info)
{
	if (slurm_auth_init(NULL) < 0)
		return SLURM_ERROR;

	return (*(ops[0].verify))(cred, auth_info);
}

uid_t g_slurm_auth_get_uid(void *cred, char *auth_info)
{
	if (slurm_auth_init(NULL) < 0)
		return SLURM_AUTH_NOBODY;

	return (*(ops[0].get_uid))(cred, auth_info);
}

gid_t g_slurm_auth_get_gid(void *cred, char *auth_info)
{
	if (slurm_auth_init(NULL) < 0)
		return SLURM_AUTH_NOBODY;

	return (*(ops[0].get_gid))(cred, auth_info);
}

char *g_slurm_auth_get_host(void *cred, char *auth_info)
{
	if (slurm_auth_init(NULL) < 0)
		return NULL;

	return (*(ops[0].get_host))(cred, auth_info);
}

int g_slurm_auth_pack(void *cred, Buf buf, uint16_t protocol_version)
{
	if (slurm_auth_init(NULL) < 0)
		return SLURM_ERROR;

	if (protocol_version >= SLURM_19_05_PROTOCOL_VERSION) {
		pack32(*ops[0].plugin_id, buf);
		return (*(ops[0].pack))(cred, buf, protocol_version);
	} else if (protocol_version >= SLURM_MIN_PROTOCOL_VERSION) {
		packstr(ops[0].plugin_type, buf);
		/*
		 * This next field was packed with plugin_version within each
		 * individual auth plugin, but upon unpack was never checked
		 * against anything. Rather than expose the protocol_version
		 * symbol, just pack a zero here instead.
		 */
		pack32(0, buf);
		return (*(ops[0].pack))(cred, buf, protocol_version);
	} else {
		error("%s: protocol_version %hu not supported",
		      __func__, protocol_version);
		return SLURM_ERROR;
	}
}

void *g_slurm_auth_unpack(Buf buf, uint16_t protocol_version)
{
	uint32_t plugin_id = 0;

	if (slurm_auth_init(NULL) < 0)
		return NULL;

	if (protocol_version >= SLURM_19_05_PROTOCOL_VERSION) {
		safe_unpack32(&plugin_id, buf);
		if (plugin_id != *(ops[0].plugin_id)) {
			error("%s: remote plugin_id %u != %u",
			      __func__, plugin_id, *(ops[0].plugin_id));
			return NULL;
		}
		return (*(ops[0].unpack))(buf, protocol_version);
	} else if (protocol_version >= SLURM_MIN_PROTOCOL_VERSION) {
		char *plugin_type;
		uint32_t uint32_tmp, version;
		safe_unpackmem_ptr(&plugin_type, &uint32_tmp, buf);

		if (xstrcmp(plugin_type, ops[0].plugin_type)) {
			error("%s: remote plugin_type `%s` != `%s`",
			      __func__, plugin_type, ops[0].plugin_type);
			return NULL;
		}
		safe_unpack32(&version, buf);
		return (*(ops[0].unpack))(buf, protocol_version);
	} else {
		error("%s: protocol_version %hu not supported",
		      __func__, protocol_version);
		return NULL;
	}

unpack_error:
	return NULL;
}

int g_slurm_auth_print(void *cred, FILE *fp)
{
	if (slurm_auth_init(NULL) < 0)
		return SLURM_ERROR;

	return (*(ops[0].print))(cred, fp);
}

int g_slurm_auth_errno(void *cred)
{
	if (slurm_auth_init(NULL) < 0)
		return SLURM_ERROR;

	return (*(ops[0].sa_errno))(cred);
}

const char *g_slurm_auth_errstr(int slurm_errno)
{
	static char auth_init_msg[] = "authentication initialization failure";
	const char *generic;

	if (slurm_auth_init(NULL) < 0 )
		return auth_init_msg;

	if ((generic = slurm_auth_generic_errstr(slurm_errno)))
		return generic;

	return (*(ops[0].sa_errstr))(slurm_errno);
}
