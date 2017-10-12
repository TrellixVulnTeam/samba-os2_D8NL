/* 
   Unix SMB/CIFS mplementation.
   DSDB replication service helper function for outgoing traffic
   
   Copyright (C) Stefan Metzmacher 2007
    
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
   
*/

#include "includes.h"
#include "dsdb/samdb/samdb.h"
#include "auth/auth.h"
#include "smbd/service.h"
#include "lib/events/events.h"
#include "dsdb/repl/drepl_service.h"
#include <ldb_errors.h>
#include "../lib/util/dlinklist.h"
#include "librpc/gen_ndr/ndr_misc.h"
#include "librpc/gen_ndr/ndr_drsuapi.h"
#include "librpc/gen_ndr/ndr_drsblobs.h"
#include "libcli/composite/composite.h"
#include "auth/gensec/gensec.h"
#include "param/param.h"
#include "../lib/util/tevent_ntstatus.h"
#include "libcli/security/security.h"

#undef DBGC_CLASS
#define DBGC_CLASS            DBGC_DRS_REPL

struct dreplsrv_out_drsuapi_state {
	struct tevent_context *ev;

	struct dreplsrv_out_connection *conn;

	struct dreplsrv_drsuapi_connection *drsuapi;

	struct drsuapi_DsBindInfoCtr bind_info_ctr;
	struct drsuapi_DsBind bind_r;
};

static void dreplsrv_out_drsuapi_connect_done(struct composite_context *creq);

struct tevent_req *dreplsrv_out_drsuapi_send(TALLOC_CTX *mem_ctx,
					     struct tevent_context *ev,
					     struct dreplsrv_out_connection *conn)
{
	struct tevent_req *req;
	struct dreplsrv_out_drsuapi_state *state;
	struct composite_context *creq;

	req = tevent_req_create(mem_ctx, &state,
				struct dreplsrv_out_drsuapi_state);
	if (req == NULL) {
		return NULL;
	}

	state->ev	= ev;
	state->conn	= conn;
	state->drsuapi	= conn->drsuapi;

	if (state->drsuapi != NULL) {
		struct dcerpc_binding_handle *b =
			state->drsuapi->pipe->binding_handle;
		bool is_connected = dcerpc_binding_handle_is_connected(b);

		if (is_connected) {
			tevent_req_done(req);
			return tevent_req_post(req, ev);
		}

		TALLOC_FREE(conn->drsuapi);
	}

	state->drsuapi = talloc_zero(state, struct dreplsrv_drsuapi_connection);
	if (tevent_req_nomem(state->drsuapi, req)) {
		return tevent_req_post(req, ev);
	}

	creq = dcerpc_pipe_connect_b_send(state, conn->binding, &ndr_table_drsuapi,
					  conn->service->system_session_info->credentials,
					  ev, conn->service->task->lp_ctx);
	if (tevent_req_nomem(creq, req)) {
		return tevent_req_post(req, ev);
	}
	composite_continue(NULL, creq, dreplsrv_out_drsuapi_connect_done, req);

	return req;
}

static void dreplsrv_out_drsuapi_bind_done(struct tevent_req *subreq);

static void dreplsrv_out_drsuapi_connect_done(struct composite_context *creq)
{
	struct tevent_req *req = talloc_get_type(creq->async.private_data,
						 struct tevent_req);
	struct dreplsrv_out_drsuapi_state *state = tevent_req_data(req,
						   struct dreplsrv_out_drsuapi_state);
	NTSTATUS status;
	struct tevent_req *subreq;

	status = dcerpc_pipe_connect_b_recv(creq,
					    state->drsuapi,
					    &state->drsuapi->pipe);
	if (tevent_req_nterror(req, status)) {
		return;
	}

	state->drsuapi->drsuapi_handle = state->drsuapi->pipe->binding_handle;

	status = gensec_session_key(state->drsuapi->pipe->conn->security_state.generic_state,
				    state->drsuapi,
				    &state->drsuapi->gensec_skey);
	if (tevent_req_nterror(req, status)) {
		return;
	}

	state->bind_info_ctr.length		= 28;
	state->bind_info_ctr.info.info28	= state->conn->service->bind_info28;

	state->bind_r.in.bind_guid = &state->conn->service->ntds_guid;
	state->bind_r.in.bind_info = &state->bind_info_ctr;
	state->bind_r.out.bind_handle = &state->drsuapi->bind_handle;

	subreq = dcerpc_drsuapi_DsBind_r_send(state,
					      state->ev,
					      state->drsuapi->drsuapi_handle,
					      &state->bind_r);
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, dreplsrv_out_drsuapi_bind_done, req);
}

static void dreplsrv_out_drsuapi_bind_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(subreq,
				 struct tevent_req);
	struct dreplsrv_out_drsuapi_state *state = tevent_req_data(req,
						   struct dreplsrv_out_drsuapi_state);
	NTSTATUS status;

	status = dcerpc_drsuapi_DsBind_r_recv(subreq, state);
	TALLOC_FREE(subreq);
	if (tevent_req_nterror(req, status)) {
		return;
	}

	if (!W_ERROR_IS_OK(state->bind_r.out.result)) {
		status = werror_to_ntstatus(state->bind_r.out.result);
		tevent_req_nterror(req, status);
		return;
	}

	ZERO_STRUCT(state->drsuapi->remote_info28);
	if (state->bind_r.out.bind_info) {
		struct drsuapi_DsBindInfo28 *info28;
		info28 = &state->drsuapi->remote_info28;

		switch (state->bind_r.out.bind_info->length) {
		case 24: {
			struct drsuapi_DsBindInfo24 *info24;
			info24 = &state->bind_r.out.bind_info->info.info24;

			info28->supported_extensions	= info24->supported_extensions;
			info28->site_guid		= info24->site_guid;
			info28->pid			= info24->pid;
			info28->repl_epoch		= 0;
			break;
		}
		case 28: {
			*info28 = state->bind_r.out.bind_info->info.info28;
			break;
		}
		case 32: {
			struct drsuapi_DsBindInfo32 *info32;
			info32 = &state->bind_r.out.bind_info->info.info32;

			info28->supported_extensions	= info32->supported_extensions;
			info28->site_guid		= info32->site_guid;
			info28->pid			= info32->pid;
			info28->repl_epoch		= info32->repl_epoch;
			break;
		}
		case 48: {
			struct drsuapi_DsBindInfo48 *info48;
			info48 = &state->bind_r.out.bind_info->info.info48;

			info28->supported_extensions	= info48->supported_extensions;
			info28->site_guid		= info48->site_guid;
			info28->pid			= info48->pid;
			info28->repl_epoch		= info48->repl_epoch;
			break;
		}
		case 52: {
			struct drsuapi_DsBindInfo52 *info52;
			info52 = &state->bind_r.out.bind_info->info.info52;

			info28->supported_extensions	= info52->supported_extensions;
			info28->site_guid		= info52->site_guid;
			info28->pid			= info52->pid;
			info28->repl_epoch		= info52->repl_epoch;
			break;
		}
		default:
			DEBUG(1, ("Warning: invalid info length in bind info: %d\n",
				state->bind_r.out.bind_info->length));
			break;
		}
	}

	tevent_req_done(req);
}

NTSTATUS dreplsrv_out_drsuapi_recv(struct tevent_req *req)
{
	struct dreplsrv_out_drsuapi_state *state = tevent_req_data(req,
						   struct dreplsrv_out_drsuapi_state);
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		tevent_req_received(req);
		return status;
	}

	state->conn->drsuapi = talloc_move(state->conn, &state->drsuapi);

	tevent_req_received(req);
	return NT_STATUS_OK;
}

struct dreplsrv_op_pull_source_state {
	struct tevent_context *ev;
	struct dreplsrv_out_operation *op;
	void *ndr_struct_ptr;
	/*
	 * Used when we have to re-try with a different NC, eg for
	 * EXOP retry or to get a current schema first
	 */
	struct dreplsrv_partition_source_dsa *source_dsa_retry;
	enum drsuapi_DsExtendedOperation extended_op_retry;
	bool retry_started;
};

static void dreplsrv_op_pull_source_connect_done(struct tevent_req *subreq);

struct tevent_req *dreplsrv_op_pull_source_send(TALLOC_CTX *mem_ctx,
						struct tevent_context *ev,
						struct dreplsrv_out_operation *op)
{
	struct tevent_req *req;
	struct dreplsrv_op_pull_source_state *state;
	struct tevent_req *subreq;

	req = tevent_req_create(mem_ctx, &state,
				struct dreplsrv_op_pull_source_state);
	if (req == NULL) {
		return NULL;
	}
	state->ev = ev;
	state->op = op;

	subreq = dreplsrv_out_drsuapi_send(state, ev, op->source_dsa->conn);
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, dreplsrv_op_pull_source_connect_done, req);

	return req;
}

static void dreplsrv_op_pull_source_get_changes_trigger(struct tevent_req *req);

static void dreplsrv_op_pull_source_connect_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(subreq,
				 struct tevent_req);
	NTSTATUS status;

	status = dreplsrv_out_drsuapi_recv(subreq);
	TALLOC_FREE(subreq);
	if (tevent_req_nterror(req, status)) {
		return;
	}

	dreplsrv_op_pull_source_get_changes_trigger(req);
}

static void dreplsrv_op_pull_source_get_changes_done(struct tevent_req *subreq);

/*
  get a RODC partial attribute set for a replication call
 */
static NTSTATUS dreplsrv_get_rodc_partial_attribute_set(struct dreplsrv_service *service,
							TALLOC_CTX *mem_ctx,
							struct drsuapi_DsPartialAttributeSet **_pas,
							struct drsuapi_DsReplicaOIDMapping_Ctr **pfm,
							bool for_schema)
{
	struct drsuapi_DsPartialAttributeSet *pas;
	struct dsdb_schema *schema;
	uint32_t i;

	pas = talloc_zero(mem_ctx, struct drsuapi_DsPartialAttributeSet);
	NT_STATUS_HAVE_NO_MEMORY(pas);

	schema = dsdb_get_schema(service->samdb, NULL);

	pas->version = 1;
	pas->attids = talloc_array(pas, enum drsuapi_DsAttributeId, schema->num_attributes);
	if (pas->attids == NULL) {
		TALLOC_FREE(pas);
		return NT_STATUS_NO_MEMORY;
	}

	for (i=0; i<schema->num_attributes; i++) {
		struct dsdb_attribute *a;
		a = schema->attributes_by_attributeID_id[i];
                if (a->systemFlags & (DS_FLAG_ATTR_NOT_REPLICATED | DS_FLAG_ATTR_IS_CONSTRUCTED)) {
			continue;
		}
		if (a->searchFlags & SEARCH_FLAG_RODC_ATTRIBUTE) {
			continue;
		}
		pas->attids[pas->num_attids] = dsdb_attribute_get_attid(a, for_schema);
		pas->num_attids++;
	}

	pas->attids = talloc_realloc(pas, pas->attids, enum drsuapi_DsAttributeId, pas->num_attids);
	if (pas->attids == NULL) {
		TALLOC_FREE(pas);
		return NT_STATUS_NO_MEMORY;
	}

	*_pas = pas;

	if (pfm != NULL) {
		dsdb_get_oid_mappings_drsuapi(schema, true, mem_ctx, pfm);
	}

	return NT_STATUS_OK;
}


/*
  get a GC partial attribute set for a replication call
 */
static NTSTATUS dreplsrv_get_gc_partial_attribute_set(struct dreplsrv_service *service,
						      TALLOC_CTX *mem_ctx,
						      struct drsuapi_DsPartialAttributeSet **_pas,
						      struct drsuapi_DsReplicaOIDMapping_Ctr **pfm)
{
	struct drsuapi_DsPartialAttributeSet *pas;
	struct dsdb_schema *schema;
	uint32_t i;

	pas = talloc_zero(mem_ctx, struct drsuapi_DsPartialAttributeSet);
	NT_STATUS_HAVE_NO_MEMORY(pas);

	schema = dsdb_get_schema(service->samdb, NULL);

	pas->version = 1;
	pas->attids = talloc_array(pas, enum drsuapi_DsAttributeId, schema->num_attributes);
	if (pas->attids == NULL) {
		TALLOC_FREE(pas);
		return NT_STATUS_NO_MEMORY;
	}

	for (i=0; i<schema->num_attributes; i++) {
		struct dsdb_attribute *a;
		a = schema->attributes_by_attributeID_id[i];
                if (a->isMemberOfPartialAttributeSet) {
			pas->attids[pas->num_attids] = dsdb_attribute_get_attid(a, false);
			pas->num_attids++;
		}
	}

	pas->attids = talloc_realloc(pas, pas->attids, enum drsuapi_DsAttributeId, pas->num_attids);
	if (pas->attids == NULL) {
		TALLOC_FREE(pas);
		return NT_STATUS_NO_MEMORY;
	}

	*_pas = pas;

	if (pfm != NULL) {
		dsdb_get_oid_mappings_drsuapi(schema, true, mem_ctx, pfm);
	}

	return NT_STATUS_OK;
}

/*
  convert from one udv format to the other
 */
static WERROR udv_convert(TALLOC_CTX *mem_ctx,
			  const struct replUpToDateVectorCtr2 *udv,
			  struct drsuapi_DsReplicaCursorCtrEx *udv_ex)
{
	uint32_t i;

	udv_ex->version = 2;
	udv_ex->reserved1 = 0;
	udv_ex->reserved2 = 0;
	udv_ex->count = udv->count;
	udv_ex->cursors = talloc_array(mem_ctx, struct drsuapi_DsReplicaCursor, udv->count);
	W_ERROR_HAVE_NO_MEMORY(udv_ex->cursors);

	for (i=0; i<udv->count; i++) {
		udv_ex->cursors[i].source_dsa_invocation_id = udv->cursors[i].source_dsa_invocation_id;
		udv_ex->cursors[i].highest_usn = udv->cursors[i].highest_usn;
	}

	return WERR_OK;
}


static void dreplsrv_op_pull_source_get_changes_trigger(struct tevent_req *req)
{
	struct dreplsrv_op_pull_source_state *state = tevent_req_data(req,
						      struct dreplsrv_op_pull_source_state);
	struct repsFromTo1 *rf1 = state->op->source_dsa->repsFrom1;
	struct dreplsrv_service *service = state->op->service;
	struct dreplsrv_partition *partition = state->op->source_dsa->partition;
	struct dreplsrv_drsuapi_connection *drsuapi = state->op->source_dsa->conn->drsuapi;
	struct drsuapi_DsGetNCChanges *r;
	struct drsuapi_DsReplicaCursorCtrEx *uptodateness_vector;
	struct tevent_req *subreq;
	struct drsuapi_DsPartialAttributeSet *pas = NULL;
	NTSTATUS status;
	uint32_t replica_flags;
	struct drsuapi_DsReplicaHighWaterMark highwatermark;
	struct ldb_dn *schema_dn = ldb_get_schema_basedn(service->samdb);
	struct drsuapi_DsReplicaOIDMapping_Ctr *mappings = NULL;

	r = talloc(state, struct drsuapi_DsGetNCChanges);
	if (tevent_req_nomem(r, req)) {
		return;
	}

	r->out.level_out = talloc(r, uint32_t);
	if (tevent_req_nomem(r->out.level_out, req)) {
		return;
	}
	r->in.req = talloc(r, union drsuapi_DsGetNCChangesRequest);
	if (tevent_req_nomem(r->in.req, req)) {
		return;
	}
	r->out.ctr = talloc(r, union drsuapi_DsGetNCChangesCtr);
	if (tevent_req_nomem(r->out.ctr, req)) {
		return;
	}

	if (partition->uptodatevector.count != 0 &&
	    partition->uptodatevector_ex.count == 0) {
		WERROR werr;
		werr = udv_convert(partition, &partition->uptodatevector, &partition->uptodatevector_ex);
		if (!W_ERROR_IS_OK(werr)) {
			DEBUG(0,(__location__ ": Failed to convert UDV for %s : %s\n",
				 ldb_dn_get_linearized(partition->dn), win_errstr(werr)));
			tevent_req_nterror(req, werror_to_ntstatus(werr));
			return;
		}
	}

	if (partition->uptodatevector_ex.count == 0) {
		uptodateness_vector = NULL;
	} else {
		uptodateness_vector = &partition->uptodatevector_ex;
	}

	replica_flags = rf1->replica_flags;
	highwatermark = rf1->highwatermark;

	if (state->op->options & DRSUAPI_DRS_GET_ANC) {
		replica_flags |= DRSUAPI_DRS_GET_ANC;
	}

	if (state->op->options & DRSUAPI_DRS_SYNC_FORCED) {
		replica_flags |= DRSUAPI_DRS_SYNC_FORCED;
	}

	if (partition->partial_replica) {
		status = dreplsrv_get_gc_partial_attribute_set(service, r,
							       &pas,
							       &mappings);
		if (!NT_STATUS_IS_OK(status)) {
			DEBUG(0,(__location__ ": Failed to construct GC partial attribute set : %s\n", nt_errstr(status)));
			tevent_req_nterror(req, status);
			return;
		}
		replica_flags &= ~DRSUAPI_DRS_WRIT_REP;
	} else if (partition->rodc_replica || state->op->extended_op == DRSUAPI_EXOP_REPL_SECRET) {
		bool for_schema = false;
		if (ldb_dn_compare_base(schema_dn, partition->dn) == 0) {
			for_schema = true;
		}
		status = dreplsrv_get_rodc_partial_attribute_set(service, r,
								 &pas,
								 &mappings,
								 for_schema);
		if (!NT_STATUS_IS_OK(status)) {
			DEBUG(0,(__location__ ": Failed to construct RODC partial attribute set : %s\n", nt_errstr(status)));
			tevent_req_nterror(req, status);
			return;
		}
		replica_flags &= ~DRSUAPI_DRS_WRIT_REP;
		if (state->op->extended_op == DRSUAPI_EXOP_REPL_SECRET) {
			replica_flags &= ~DRSUAPI_DRS_SPECIAL_SECRET_PROCESSING;
		} else {
			replica_flags |= DRSUAPI_DRS_SPECIAL_SECRET_PROCESSING;
		}

		/*
		 * As per MS-DRSR:
		 *
		 * 4.1.10.4
		 * Client Behavior When Sending the IDL_DRSGetNCChanges Request
		 *
		 * 4.1.10.4.1
		 * ReplicateNCRequestMsg
		 */
		replica_flags |= DRSUAPI_DRS_GET_ALL_GROUP_MEMBERSHIP;
	} else {
		replica_flags |= DRSUAPI_DRS_GET_ALL_GROUP_MEMBERSHIP;
	}

	if (state->op->extended_op != DRSUAPI_EXOP_NONE) {
		/*
		 * If it's an exop never set the ADD_REF even if it's in
		 * repsFrom flags.
		 */
		replica_flags &= ~DRSUAPI_DRS_ADD_REF;
	}

	/* is this a full resync of all objects? */
	if (state->op->options & DRSUAPI_DRS_FULL_SYNC_NOW) {
		ZERO_STRUCT(highwatermark);
		/* clear the FULL_SYNC_NOW option for subsequent
		   stages of the replication cycle */
		state->op->options &= ~DRSUAPI_DRS_FULL_SYNC_NOW;
		state->op->options |= DRSUAPI_DRS_FULL_SYNC_IN_PROGRESS;
		replica_flags |= DRSUAPI_DRS_NEVER_SYNCED;
	}
	if (state->op->options & DRSUAPI_DRS_FULL_SYNC_IN_PROGRESS) {
		uptodateness_vector = NULL;
	}

	r->in.bind_handle	= &drsuapi->bind_handle;
	if (drsuapi->remote_info28.supported_extensions & DRSUAPI_SUPPORTED_EXTENSION_GETCHGREQ_V8) {
		r->in.level				= 8;
		r->in.req->req8.destination_dsa_guid	= service->ntds_guid;
		r->in.req->req8.source_dsa_invocation_id= rf1->source_dsa_invocation_id;
		r->in.req->req8.naming_context		= &partition->nc;
		r->in.req->req8.highwatermark		= highwatermark;
		r->in.req->req8.uptodateness_vector	= uptodateness_vector;
		r->in.req->req8.replica_flags		= replica_flags;
		r->in.req->req8.max_object_count	= 133;
		r->in.req->req8.max_ndr_size		= 1336811;
		r->in.req->req8.extended_op		= state->op->extended_op;
		r->in.req->req8.fsmo_info		= state->op->fsmo_info;
		r->in.req->req8.partial_attribute_set	= pas;
		r->in.req->req8.partial_attribute_set_ex= NULL;
		r->in.req->req8.mapping_ctr.num_mappings= mappings == NULL ? 0 : mappings->num_mappings;
		r->in.req->req8.mapping_ctr.mappings	= mappings == NULL ? NULL : mappings->mappings;
	} else {
		r->in.level				= 5;
		r->in.req->req5.destination_dsa_guid	= service->ntds_guid;
		r->in.req->req5.source_dsa_invocation_id= rf1->source_dsa_invocation_id;
		r->in.req->req5.naming_context		= &partition->nc;
		r->in.req->req5.highwatermark		= highwatermark;
		r->in.req->req5.uptodateness_vector	= uptodateness_vector;
		r->in.req->req5.replica_flags		= replica_flags;
		r->in.req->req5.max_object_count	= 133;
		r->in.req->req5.max_ndr_size		= 1336770;
		r->in.req->req5.extended_op		= state->op->extended_op;
		r->in.req->req5.fsmo_info		= state->op->fsmo_info;
	}

#if 0
	NDR_PRINT_IN_DEBUG(drsuapi_DsGetNCChanges, r);
#endif

	state->ndr_struct_ptr = r;
	subreq = dcerpc_drsuapi_DsGetNCChanges_r_send(state,
						      state->ev,
						      drsuapi->drsuapi_handle,
						      r);
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, dreplsrv_op_pull_source_get_changes_done, req);
}

static void dreplsrv_op_pull_source_apply_changes_trigger(struct tevent_req *req,
						          struct drsuapi_DsGetNCChanges *r,
						          uint32_t ctr_level,
						          struct drsuapi_DsGetNCChangesCtr1 *ctr1,
						          struct drsuapi_DsGetNCChangesCtr6 *ctr6);

static void dreplsrv_op_pull_source_get_changes_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(subreq,
				 struct tevent_req);
	struct dreplsrv_op_pull_source_state *state = tevent_req_data(req,
						      struct dreplsrv_op_pull_source_state);
	NTSTATUS status;
	struct drsuapi_DsGetNCChanges *r = talloc_get_type(state->ndr_struct_ptr,
					   struct drsuapi_DsGetNCChanges);
	uint32_t ctr_level = 0;
	struct drsuapi_DsGetNCChangesCtr1 *ctr1 = NULL;
	struct drsuapi_DsGetNCChangesCtr6 *ctr6 = NULL;
	enum drsuapi_DsExtendedError extended_ret = DRSUAPI_EXOP_ERR_NONE;
	state->ndr_struct_ptr = NULL;

	status = dcerpc_drsuapi_DsGetNCChanges_r_recv(subreq, r);
	TALLOC_FREE(subreq);
	if (tevent_req_nterror(req, status)) {
		return;
	}

	if (!W_ERROR_IS_OK(r->out.result)) {
		status = werror_to_ntstatus(r->out.result);
		tevent_req_nterror(req, status);
		return;
	}

	if (*r->out.level_out == 1) {
		ctr_level = 1;
		ctr1 = &r->out.ctr->ctr1;
	} else if (*r->out.level_out == 2 &&
		   r->out.ctr->ctr2.mszip1.ts) {
		ctr_level = 1;
		ctr1 = &r->out.ctr->ctr2.mszip1.ts->ctr1;
	} else if (*r->out.level_out == 6) {
		ctr_level = 6;
		ctr6 = &r->out.ctr->ctr6;
	} else if (*r->out.level_out == 7 &&
		   r->out.ctr->ctr7.level == 6 &&
		   r->out.ctr->ctr7.type == DRSUAPI_COMPRESSION_TYPE_MSZIP &&
		   r->out.ctr->ctr7.ctr.mszip6.ts) {
		ctr_level = 6;
		ctr6 = &r->out.ctr->ctr7.ctr.mszip6.ts->ctr6;
	} else if (*r->out.level_out == 7 &&
		   r->out.ctr->ctr7.level == 6 &&
		   r->out.ctr->ctr7.type == DRSUAPI_COMPRESSION_TYPE_XPRESS &&
		   r->out.ctr->ctr7.ctr.xpress6.ts) {
		ctr_level = 6;
		ctr6 = &r->out.ctr->ctr7.ctr.xpress6.ts->ctr6;
	} else {
		status = werror_to_ntstatus(WERR_BAD_NET_RESP);
		tevent_req_nterror(req, status);
		return;
	}

	if (!ctr1 && !ctr6) {
		status = werror_to_ntstatus(WERR_BAD_NET_RESP);
		tevent_req_nterror(req, status);
		return;
	}

	if (ctr_level == 6) {
		if (!W_ERROR_IS_OK(ctr6->drs_error)) {
			status = werror_to_ntstatus(ctr6->drs_error);
			tevent_req_nterror(req, status);
			return;
		}
		extended_ret = ctr6->extended_ret;
	}

	if (ctr_level == 1) {
		extended_ret = ctr1->extended_ret;
	}

	if (state->op->extended_op != DRSUAPI_EXOP_NONE) {
		state->op->extended_ret = extended_ret;

		if (extended_ret != DRSUAPI_EXOP_ERR_SUCCESS) {
			status = NT_STATUS_UNSUCCESSFUL;
			tevent_req_nterror(req, status);
			return;
		}
	}

	dreplsrv_op_pull_source_apply_changes_trigger(req, r, ctr_level, ctr1, ctr6);
}

static void dreplsrv_update_refs_trigger(struct tevent_req *req);

static void dreplsrv_op_pull_source_apply_changes_trigger(struct tevent_req *req,
							  struct drsuapi_DsGetNCChanges *r,
							  uint32_t ctr_level,
							  struct drsuapi_DsGetNCChangesCtr1 *ctr1,
							   struct drsuapi_DsGetNCChangesCtr6 *ctr6)
{
	struct dreplsrv_op_pull_source_state *state = tevent_req_data(req,
						      struct dreplsrv_op_pull_source_state);
	struct repsFromTo1 rf1 = *state->op->source_dsa->repsFrom1;
	struct dreplsrv_service *service = state->op->service;
	struct dreplsrv_partition *partition = state->op->source_dsa->partition;
	struct dreplsrv_drsuapi_connection *drsuapi = state->op->source_dsa->conn->drsuapi;
	struct ldb_dn *schema_dn = ldb_get_schema_basedn(service->samdb);
	struct dsdb_schema *schema;
	struct dsdb_schema *working_schema = NULL;
	const struct drsuapi_DsReplicaOIDMapping_Ctr *mapping_ctr;
	uint32_t object_count;
	struct drsuapi_DsReplicaObjectListItemEx *first_object;
	uint32_t linked_attributes_count;
	struct drsuapi_DsReplicaLinkedAttribute *linked_attributes;
	const struct drsuapi_DsReplicaCursor2CtrEx *uptodateness_vector;
	struct dsdb_extended_replicated_objects *objects;
	bool more_data = false;
	WERROR status;
	NTSTATUS nt_status;
	uint32_t dsdb_repl_flags = 0;
	struct ldb_dn *nc_root = NULL;
	int ret;

	switch (ctr_level) {
	case 1:
		mapping_ctr			= &ctr1->mapping_ctr;
		object_count			= ctr1->object_count;
		first_object			= ctr1->first_object;
		linked_attributes_count		= 0;
		linked_attributes		= NULL;
		rf1.source_dsa_obj_guid 	= ctr1->source_dsa_guid;
		rf1.source_dsa_invocation_id	= ctr1->source_dsa_invocation_id;
		rf1.highwatermark		= ctr1->new_highwatermark;
		uptodateness_vector		= NULL; /* TODO: map it */
		more_data			= ctr1->more_data;
		break;
	case 6:
		mapping_ctr			= &ctr6->mapping_ctr;
		object_count			= ctr6->object_count;
		first_object			= ctr6->first_object;
		linked_attributes_count		= ctr6->linked_attributes_count;
		linked_attributes		= ctr6->linked_attributes;
		rf1.source_dsa_obj_guid 	= ctr6->source_dsa_guid;
		rf1.source_dsa_invocation_id	= ctr6->source_dsa_invocation_id;
		rf1.highwatermark		= ctr6->new_highwatermark;
		uptodateness_vector		= ctr6->uptodateness_vector;
		more_data			= ctr6->more_data;
		break;
	default:
		nt_status = werror_to_ntstatus(WERR_BAD_NET_RESP);
		tevent_req_nterror(req, nt_status);
		return;
	}

	schema = dsdb_get_schema(service->samdb, state);
	if (!schema) {
		DEBUG(0,(__location__ ": Schema is not loaded yet!\n"));
		tevent_req_nterror(req, NT_STATUS_INTERNAL_ERROR);
		return;
	}

	/*
	 * Decide what working schema to use for object conversion.
	 * We won't need a working schema for empty replicas sent.
	 */
	if (first_object) {
		bool is_schema = ldb_dn_compare(partition->dn, schema_dn) == 0;
		if (is_schema) {
			/* create working schema to convert objects with */
			status = dsdb_repl_make_working_schema(service->samdb,
							       schema,
							       mapping_ctr,
							       object_count,
							       first_object,
							       &drsuapi->gensec_skey,
							       state, &working_schema);
			if (!W_ERROR_IS_OK(status)) {
				DEBUG(0,("Failed to create working schema: %s\n",
					 win_errstr(status)));
				tevent_req_nterror(req, NT_STATUS_INTERNAL_ERROR);
				return;
			}
		}
	}

	if (partition->partial_replica || partition->rodc_replica) {
		dsdb_repl_flags |= DSDB_REPL_FLAG_PARTIAL_REPLICA;
	}
	if (state->op->options & DRSUAPI_DRS_FULL_SYNC_IN_PROGRESS) {
		dsdb_repl_flags |= DSDB_REPL_FLAG_PRIORITISE_INCOMING;
	}
	if (state->op->options & DRSUAPI_DRS_SPECIAL_SECRET_PROCESSING) {
		dsdb_repl_flags |= DSDB_REPL_FLAG_EXPECT_NO_SECRETS;
	}

	if (state->op->extended_op != DRSUAPI_EXOP_NONE) {
		ret = dsdb_find_nc_root(service->samdb, partition,
					partition->dn, &nc_root);
		if (ret != LDB_SUCCESS) {
			DEBUG(0,(__location__ ": Failed to find nc_root for %s\n",
				 ldb_dn_get_linearized(partition->dn)));
			tevent_req_nterror(req, NT_STATUS_INTERNAL_ERROR);
			return;
		}
	} else {
		nc_root = partition->dn;
	}

	status = dsdb_replicated_objects_convert(service->samdb,
						 working_schema ? working_schema : schema,
						 nc_root,
						 mapping_ctr,
						 object_count,
						 first_object,
						 linked_attributes_count,
						 linked_attributes,
						 &rf1,
						 uptodateness_vector,
						 &drsuapi->gensec_skey,
						 dsdb_repl_flags,
						 state, &objects);

	if (W_ERROR_EQUAL(status, WERR_DS_DRA_SCHEMA_MISMATCH)) {
		struct dreplsrv_partition *p;

		if (state->retry_started) {
			nt_status = werror_to_ntstatus(WERR_BAD_NET_RESP);
			DEBUG(0,("Failed to convert objects after retry: %s/%s\n",
				  win_errstr(status), nt_errstr(nt_status)));
			tevent_req_nterror(req, nt_status);
			return;
		}

		/*
		 * Change info sync or extended operation into a fetch
		 * of the schema partition, so we get all the schema
		 * objects we need.
		 *
		 * We don't want to re-do the remote exop,
		 * unless it was REPL_SECRET so we set the
		 * fallback operation to just be a fetch of
		 * the relevent partition.
		 */


		if (state->op->extended_op == DRSUAPI_EXOP_REPL_SECRET) {
			state->extended_op_retry = state->op->extended_op;
		} else {
			state->extended_op_retry = DRSUAPI_EXOP_NONE;
		}
		state->op->extended_op = DRSUAPI_EXOP_NONE;

		if (ldb_dn_compare(nc_root, partition->dn) == 0) {
			state->source_dsa_retry = state->op->source_dsa;
		} else {
			status = dreplsrv_partition_find_for_nc(service,
								NULL, NULL,
								ldb_dn_get_linearized(nc_root),
								&p);
			if (!W_ERROR_IS_OK(status)) {
				DEBUG(2, ("Failed to find requested Naming Context for %s: %s",
					  ldb_dn_get_linearized(nc_root),
					  win_errstr(status)));
				nt_status = werror_to_ntstatus(status);
				tevent_req_nterror(req, nt_status);
				return;
			}
			status = dreplsrv_partition_source_dsa_by_guid(p,
								       &state->op->source_dsa->repsFrom1->source_dsa_obj_guid,
								       &state->source_dsa_retry);

			if (!W_ERROR_IS_OK(status)) {
				struct GUID_txt_buf str;
				DEBUG(2, ("Failed to find requested source DSA for %s and %s: %s",
					  ldb_dn_get_linearized(nc_root),
					  GUID_buf_string(&state->op->source_dsa->repsFrom1->source_dsa_obj_guid, &str),
					  win_errstr(status)));
				nt_status = werror_to_ntstatus(status);
				tevent_req_nterror(req, nt_status);
				return;
			}
		}

		/* Find schema naming context to be synchronized first */
		status = dreplsrv_partition_find_for_nc(service,
							NULL, NULL,
							ldb_dn_get_linearized(schema_dn),
							&p);
		if (!W_ERROR_IS_OK(status)) {
			DEBUG(2, ("Failed to find requested Naming Context for schema: %s",
				  win_errstr(status)));
			nt_status = werror_to_ntstatus(status);
			tevent_req_nterror(req, nt_status);
			return;
		}

		status = dreplsrv_partition_source_dsa_by_guid(p,
							       &state->op->source_dsa->repsFrom1->source_dsa_obj_guid,
							       &state->op->source_dsa);
		if (!W_ERROR_IS_OK(status)) {
			struct GUID_txt_buf str;
			DEBUG(2, ("Failed to find requested source DSA for %s and %s: %s",
				  ldb_dn_get_linearized(schema_dn),
				  GUID_buf_string(&state->op->source_dsa->repsFrom1->source_dsa_obj_guid, &str),
				  win_errstr(status)));
			nt_status = werror_to_ntstatus(status);
			tevent_req_nterror(req, nt_status);
			return;
		}
		DEBUG(4,("Wrong schema when applying reply GetNCChanges, retrying\n"));

		state->retry_started = true;
		dreplsrv_op_pull_source_get_changes_trigger(req);
		return;

	} else if (!W_ERROR_IS_OK(status)) {
		nt_status = werror_to_ntstatus(WERR_BAD_NET_RESP);
		DEBUG(0,("Failed to convert objects: %s/%s\n",
			  win_errstr(status), nt_errstr(nt_status)));
		tevent_req_nterror(req, nt_status);
		return;
	}

	status = dsdb_replicated_objects_commit(service->samdb,
						working_schema,
						objects,
						&state->op->source_dsa->notify_uSN);
	talloc_free(objects);

	if (!W_ERROR_IS_OK(status)) {

		/*
		 * If we failed to apply the records due to a missing
		 * parent, try again after asking for the parent
		 * records first.  Because we don't update the
		 * highwatermark, we start this part of the cycle
		 * again.
		 */
		if (((state->op->options & DRSUAPI_DRS_GET_ANC) == 0)
		    && W_ERROR_EQUAL(status, WERR_DS_DRA_MISSING_PARENT)) {
			state->op->options |= DRSUAPI_DRS_GET_ANC;
			DEBUG(4,("Missing parent object when we didn't set the DRSUAPI_DRS_GET_ANC flag, retrying\n"));
			dreplsrv_op_pull_source_get_changes_trigger(req);
			return;
		} else if (((state->op->options & DRSUAPI_DRS_GET_ANC))
			   && W_ERROR_EQUAL(status, WERR_DS_DRA_MISSING_PARENT)) {
			DEBUG(1,("Missing parent object despite setting DRSUAPI_DRS_GET_ANC flag\n"));
			nt_status = NT_STATUS_INVALID_NETWORK_RESPONSE;
		} else {
			nt_status = werror_to_ntstatus(WERR_BAD_NET_RESP);
		}
		DEBUG(0,("Failed to commit objects: %s/%s\n",
			  win_errstr(status), nt_errstr(nt_status)));
		tevent_req_nterror(req, nt_status);
		return;
	}

	if (state->op->extended_op == DRSUAPI_EXOP_NONE) {
		/* if it applied fine, we need to update the highwatermark */
		*state->op->source_dsa->repsFrom1 = rf1;
	}

	/* we don't need this maybe very large structure anymore */
	TALLOC_FREE(r);

	if (more_data) {
		dreplsrv_op_pull_source_get_changes_trigger(req);
		return;
	}

	/*
	 * If we had to divert via doing some other thing, such as
	 * pulling the schema, then go back and do the original
	 * operation once we are done.
	 */
	if (state->source_dsa_retry != NULL) {
		state->op->source_dsa = state->source_dsa_retry;
		state->op->extended_op = state->extended_op_retry;
		state->source_dsa_retry = NULL;
		dreplsrv_op_pull_source_get_changes_trigger(req);
		return;
	}

	if (state->op->extended_op != DRSUAPI_EXOP_NONE ||
	    state->op->service->am_rodc) {
		/*
		  we don't do the UpdateRefs for extended ops or if we
		  are a RODC
		 */
		tevent_req_done(req);
		return;
	}

	/* now we need to update the repsTo record for this partition
	   on the server. These records are initially established when
	   we join the domain, but they quickly expire.  We do it here
	   so we can use the already established DRSUAPI pipe
	*/
	dreplsrv_update_refs_trigger(req);
}

static void dreplsrv_update_refs_done(struct tevent_req *subreq);

/*
  send a UpdateRefs request to refresh our repsTo record on the server
 */
static void dreplsrv_update_refs_trigger(struct tevent_req *req)
{
	struct dreplsrv_op_pull_source_state *state = tevent_req_data(req,
						      struct dreplsrv_op_pull_source_state);
	struct dreplsrv_service *service = state->op->service;
	struct dreplsrv_partition *partition = state->op->source_dsa->partition;
	struct dreplsrv_drsuapi_connection *drsuapi = state->op->source_dsa->conn->drsuapi;
	struct drsuapi_DsReplicaUpdateRefs *r;
	char *ntds_dns_name;
	struct tevent_req *subreq;

	r = talloc(state, struct drsuapi_DsReplicaUpdateRefs);
	if (tevent_req_nomem(r, req)) {
		return;
	}

	ntds_dns_name = samdb_ntds_msdcs_dns_name(service->samdb, r, &service->ntds_guid);
	if (tevent_req_nomem(ntds_dns_name, req)) {
		talloc_free(r);
		return;
	}

	r->in.bind_handle	= &drsuapi->bind_handle;
	r->in.level             = 1;
	r->in.req.req1.naming_context	  = &partition->nc;
	r->in.req.req1.dest_dsa_dns_name  = ntds_dns_name;
	r->in.req.req1.dest_dsa_guid	  = service->ntds_guid;
	r->in.req.req1.options	          = DRSUAPI_DRS_ADD_REF | DRSUAPI_DRS_DEL_REF;
	if (!service->am_rodc) {
		r->in.req.req1.options |= DRSUAPI_DRS_WRIT_REP;
	}

	state->ndr_struct_ptr = r;
	subreq = dcerpc_drsuapi_DsReplicaUpdateRefs_r_send(state,
							   state->ev,
							   drsuapi->drsuapi_handle,
							   r);
	if (tevent_req_nomem(subreq, req)) {
		talloc_free(r);
		return;
	}
	tevent_req_set_callback(subreq, dreplsrv_update_refs_done, req);
}

/*
  receive a UpdateRefs reply
 */
static void dreplsrv_update_refs_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(subreq,
				 struct tevent_req);
	struct dreplsrv_op_pull_source_state *state = tevent_req_data(req,
						      struct dreplsrv_op_pull_source_state);
	struct drsuapi_DsReplicaUpdateRefs *r = talloc_get_type(state->ndr_struct_ptr,
								struct drsuapi_DsReplicaUpdateRefs);
	NTSTATUS status;

	state->ndr_struct_ptr = NULL;

	status = dcerpc_drsuapi_DsReplicaUpdateRefs_r_recv(subreq, r);
	TALLOC_FREE(subreq);
	if (!NT_STATUS_IS_OK(status)) {
		DEBUG(0,("UpdateRefs failed with %s\n", 
			 nt_errstr(status)));
		tevent_req_nterror(req, status);
		return;
	}

	if (!W_ERROR_IS_OK(r->out.result)) {
		status = werror_to_ntstatus(r->out.result);
		DEBUG(0,("UpdateRefs failed with %s/%s for %s %s\n",
			 win_errstr(r->out.result),
			 nt_errstr(status),
			 r->in.req.req1.dest_dsa_dns_name,
			 r->in.req.req1.naming_context->dn));
		/*
		 * TODO we are currently not sending the
		 * DsReplicaUpdateRefs at the correct moment,
		 * we do it just after a GetNcChanges which is
		 * not always correct.
		 * Especially when another DC is trying to demote
		 * it will sends us a DsReplicaSync that will trigger a getNcChanges
		 * this call will succeed but the DsRecplicaUpdateRefs that we send
		 * just after will not because the DC is in a demote state and
		 * will reply us a WERR_DS_DRA_BUSY, this error will cause us to
		 * answer to the DsReplicaSync with a non OK status, the other DC
		 * will stop the demote due to this error.
		 * In order to cope with this we will for the moment concider
		 * a DS_DRA_BUSY not as an error.
		 * It's not ideal but it should not have a too huge impact for
		 * running production as this error otherwise never happen and
		 * due to the fact the send a DsReplicaUpdateRefs after each getNcChanges
		 */
		if (!W_ERROR_EQUAL(r->out.result, WERR_DS_DRA_BUSY)) {
			tevent_req_nterror(req, status);
			return;
		}
	}

	DEBUG(4,("UpdateRefs OK for %s %s\n", 
		 r->in.req.req1.dest_dsa_dns_name,
		 r->in.req.req1.naming_context->dn));

	tevent_req_done(req);
}

WERROR dreplsrv_op_pull_source_recv(struct tevent_req *req)
{
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		tevent_req_received(req);
		return ntstatus_to_werror(status);
	}

	tevent_req_received(req);
	return WERR_OK;
}

