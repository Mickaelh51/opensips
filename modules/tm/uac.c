/*
 * Copyright (C) 2010-2014 OpenSIPS Solutions
 * Copyright (C) 2001-2003 FhG Fokus
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 *
 * History:
 * --------
 *  2003-01-23  t_uac_dlg now uses get_out_socket (jiri)
 *  2003-01-27  fifo:t_uac_dlg completed (jiri)
 *  2003-01-29  scratchpad removed (jiri)
 *  2003-02-13  t_uac, t _uac_dlg, gethfblock, uri2proxy changed to use
 *               proto & rb->dst (andrei)
 *  2003-02-27  FIFO/UAC now dumps reply -- good for CTD (jiri)
 *  2003-02-28  scratchpad compatibility abandoned (jiri)
 *  2003-03-01  kr set through a function now (jiri)
 *  2003-03-19  replaced all mallocs/frees w/ pkg_malloc/pkg_free (andrei)
 *  2003-04-02  port_no_str does not contain a leading ':' anymore (andrei)
 *  2003-07-08  appropriate log messages in check_params(...),
 *               call calculate_hooks if next_hop==NULL in t_uac (dcm)
 *  2003-10-24  updated to the new socket_info lists (andrei)
 *  2003-12-03  completion filed removed from transaction and uac callbacks
 *              merged in transaction callbacks as LOCAL_COMPLETED (bogdan)
 *  2004-02-11  FIFO/CANCEL + alignments (hash=f(callid,cseq)) (uli+jiri)
 *  2004-02-13  t->is_invite, t->local, t->noisy_ctimer replaced (bogdan)
 *  2004-08-23  avp support in t_uac (bogdan)
 *
 *
 * simple UAC for things such as SUBSCRIBE or SMS gateway;
 * no authentication and other UAC features -- just send
 * a message, retransmit and await a reply; forking is not
 * supported during client generation, in all other places
 * it is -- adding it should be simple
 */

#include <string.h>
#include "../../mem/shm_mem.h"
#include "../../dprint.h"
#include "../../md5.h"
#include "../../socket_info.h"
#include "../../route.h"
#include "../../action.h"
#include "../../dset.h"
#include "../../data_lump.h"
#include "../../parser/parse_methods.h"

#include "ut.h"
#include "h_table.h"
#include "t_hooks.h"
#include "t_funcs.h"
#include "t_msgbuilder.h"
#include "callid.h"
#include "uac.h"


#define FROM_TAG_LEN (MD5_LEN + 1 /* - */ + CRC16_LEN) /* length of FROM tags */

static char from_tag[FROM_TAG_LEN + 1];

 /* Enable/disable passing of provisional replies to FIFO applications */
int pass_provisional_replies = 0;

/* T holder for the last local transaction */
struct cell** last_localT;


/*
 * Initialize UAC
 */
int uac_init(void)
{
	str src[3];
	struct socket_info *si;

	if (RAND_MAX < TM_TABLE_ENTRIES) {
		LM_WARN("uac does not spread across the whole hash table\n");
	}
	/* on tcp/tls bind_address is 0 so try to get the first address we listen
	 * on no matter the protocol */
	si=bind_address?bind_address:get_first_socket();
	if (si==0){
		LM_CRIT("null socket list\n");
		return -1;
	}

	/* calculate the initial From tag */
	src[0].s = "Long live SER server";
	src[0].len = strlen(src[0].s);
	src[1].s = si->address_str.s;
	src[1].len = strlen(src[1].s);
	src[2].s = si->port_no_str.s;
	src[2].len = strlen(src[2].s);

	MD5StringArray(from_tag, src, 3);
	from_tag[MD5_LEN] = '-';
	return 1;
}


/*
 * Generate a From tag
 */
void generate_fromtag(str* tag, str* callid)
{
	     /* calculate from tag from callid */
	crcitt_string_array(&from_tag[MD5_LEN + 1], callid, 1);
	tag->s = from_tag;
	tag->len = FROM_TAG_LEN;
}


/*
 * Check value of parameters
 */
static inline int check_params(str* method, str* to, str* from, dlg_t** dialog)
{
	if (!method || !to || !from || !dialog) {
		LM_ERR("invalid parameter value\n");
		return -1;
	}

	if (!method->s || !method->len) {
		LM_ERR("invalid request method\n");
		return -2;
	}

	if (!to->s || !to->len) {
		LM_ERR("invalid To URI\n");
		return -4;
	}

	if (!from->s || !from->len) {
		LM_ERR("invalid From URI\n");
		return -5;
	}
	return 0;
}


static inline unsigned int dlg2hash( dlg_t* dlg )
{
	str cseq_nr;
	unsigned int hashid;

	cseq_nr.s=int2str(dlg->loc_seq.value, &cseq_nr.len);
	hashid = tm_hash(dlg->id.call_id, cseq_nr);
	return hashid;
}


static int run_local_route( struct cell *new_cell, char **buf, int *buf_len,
				dlg_t *dialog, struct sip_msg **ret_req, char **ret_req_buf)
{
	struct sip_msg *req = NULL;
	struct cell *backup_cell;
	int backup_route_type;
	struct retr_buf *request;
	struct proxy_l *new_proxy = NULL;
	union sockaddr_union new_to_su;
	struct socket_info *new_send_sock;
	unsigned short dst_changed;
	char *buf1=NULL, *sipmsg_buf;
	int buf_len1, sip_msg_len;
	str h_to, h_from, h_cseq, h_callid;

	/* do not build buffer if callbacks are not needed and
	 * there are no local routes */
	if (!has_tran_tmcbs(new_cell, TMCB_LOCAL_REQUEST_OUT) &&
			!sroutes->local.a)
		return 0;

	LM_DBG("building sip_msg from buffer\n");
	sipmsg_buf = *buf; /* remember the buffer used to get the sip_msg */
	req = buf_to_sip_msg( *buf, *buf_len, dialog);
	if (req==NULL) {
		LM_ERR("failed to build sip_msg from buffer\n");
		return -1;
	}

	/* set this transaction as active one */
	backup_cell = get_t();
	set_t( new_cell );
	/* disable parallel forking */
	set_dset_state( 0 /*disable*/);

	/* run the route */
	swap_route_type( backup_route_type, LOCAL_ROUTE);
	if (sroutes && sroutes->local.a)
		run_top_route( sroutes->local, req);
	if (has_tran_tmcbs(new_cell, TMCB_LOCAL_REQUEST_OUT) ) {
		run_trans_callbacks(TMCB_LOCAL_REQUEST_OUT, new_cell,
			req, 0, 0);
	}
	set_route_type( backup_route_type );

	/* transfer current message context back to t */
	new_cell->uac[0].br_flags = getb0flags(req);
	/* restore the prevoius active transaction */
	set_t( backup_cell );

	set_dset_state( 1 /*enable*/);

	/* check for changes - if none, do not regenerate the buffer */
	dst_changed = 1;
	if (req->new_uri.s || req->force_send_socket!=dialog->send_sock ||
	req->dst_uri.len != dialog->hooks.next_hop->len ||
	memcmp(req->dst_uri.s,dialog->hooks.next_hop->s,req->dst_uri.len) ||
	(dst_changed=0)!=0 || req->add_rm || should_update_sip_body(req)
	|| req->msg_flags&FL_FORCE_LOCAL_RPORT) {

		/* stuff changed in the request, we may need to rebuild, so let's
		 * evaluate the changes first, mainly if the destination changed */
		request = &new_cell->uac[0].request;
		new_send_sock = NULL;
		/* do we also need to change the destination? */
		if (dst_changed) {
			/* calculate the socket corresponding to next hop */
			new_proxy = uri2proxy(
				req->dst_uri.s ? &(req->dst_uri) : &req->new_uri,
				PROTO_NONE );
			if (new_proxy==0)
				goto skip_update;
			/* use the first address */
			hostent2su( &new_to_su,
				&new_proxy->host, new_proxy->addr_idx,
				new_proxy->port ? new_proxy->port:SIP_PORT);
			/* get the send socket */
			new_send_sock = get_send_socket( req, &new_to_su,
				new_proxy->proto);
			if (new_send_sock==NULL) {
				free_proxy( new_proxy );
				pkg_free( new_proxy );
				LM_ERR("no socket found for the new destination\n");
					goto skip_update;
			}
		}

		/* if interface change or new VIA related flags were added,
		 * we need to re-build the via */
		if ( (new_send_sock && new_send_sock != dialog->send_sock)
		|| (req->msg_flags&FL_FORCE_LOCAL_RPORT &&
		/* coding hack to get the new_send_sock set if FL_FORCE_LOCAL_RPORT
		 * was set (note that new_send_sock gets set only if destination was
		 * changed, which is not the case here) */
		(new_send_sock=dialog->send_sock)!=NULL) ) {

			LM_DBG("Interface change in local route -> "
				"rebuilding via\n");
			if (!del_lump( req, req->h_via1->name.s - req->buf,
			req->h_via1->len,0)) {
				LM_ERR("Failed to remove initial via \n");
				goto skip_update;
			}

			memcpy(req->add_to_branch_s,req->via1->branch->value.s,
				req->via1->branch->value.len);
			req->add_to_branch_len = req->via1->branch->value.len;

			/* build the shm buffer now */
			set_init_lump_flags(LUMPFLAG_BRANCH);
			buf1 = build_req_buf_from_sip_req( req, (unsigned int*)&buf_len1,
				new_send_sock, new_send_sock->proto, NULL, MSG_TRANS_SHM_FLAG);
			reset_init_lump_flags();
			del_flaged_lumps( &req->add_rm, LUMPFLAG_BRANCH);

		} else {

			LM_DBG("Change in local route -> rebuilding buffer\n");
			/* build the shm buffer now */
			buf1 = build_req_buf_from_sip_req( req, (unsigned int*)&buf_len1,
				dialog->send_sock, dialog->send_sock->proto,
				NULL, MSG_TRANS_SHM_FLAG|MSG_TRANS_NOVIA_FLAG);
			/* now as it used, hide the original VIA header */
			del_lump( req, req->h_via1->name.s-req->buf, req->h_via1->len, 0);
			new_send_sock = NULL;

		}

		/* from this point further, the only updated send_sock is the one
		 * from transaction, request->dst.send_sock */

		if (!buf1) {
			LM_ERR("no more shm mem\n");
			/* keep original buffer */
			goto skip_update;
		}
		/* update shortcuts */
		if(!req->add_rm && !req->new_uri.s) {
			/* headers are not affected, simply tranlate */
			new_cell->from.s = new_cell->from.s - *buf + buf1;
			new_cell->to.s = new_cell->to.s - *buf + buf1;
			new_cell->callid.s = new_cell->callid.s - *buf + buf1;
			new_cell->cseq_n.s = new_cell->cseq_n.s - *buf + buf1;
		} else {
			/* use heavy artilery :D */
			if (extract_ftc_hdrs( buf1, buf_len1, &h_from, &h_to,
			&h_cseq, &h_callid)!=0 ) {
				LM_ERR("failed to update shortcut pointers\n");
				shm_free(buf1);
				goto skip_update;
			}
			new_cell->from = h_from;
			new_cell->to = h_to;
			new_cell->callid = h_callid;
			new_cell->cseq_n = h_cseq;
		}

		/* here we rely on how build_uac_req() builds the first line */
		new_cell->uac[0].uri.s = buf1 + req->first_line.u.request.method.len+1;
		new_cell->uac[0].uri.len = GET_RURI(req)->len;

		/* update also info about new destination and send sock */
		if (new_send_sock) {
			request->dst.send_sock = new_send_sock;
			request->dst.proto = new_send_sock->proto;
			request->dst.proto_reserved1 = 0;
		}
		if (new_proxy) {
			request->dst.to = new_to_su;
			/* for DNS based failover, copy the DNS proxy into transaction */
			if (!disable_dns_failover) {
				new_cell->uac[0].proxy = shm_clone_proxy( new_proxy,
					1/*do_free*/);
				if (new_cell->uac[0].proxy==NULL)
					LM_ERR("failed to store DNS info -> no DNS "
						"based failover\n");
			}
		}

		/* the `buf` buffer is the same as `sipmsg_buf`, so we
		 * do not loose the original msg buffer; later, if we 
		 * see a non zero `buf1` (as a marker that we visited this
		 * part of the code), we know that we have to release the
		 * `sipmsg_buf` also; if we did not visit this part of the
		 * code, the `buf` == `sipmsg_buf` and  `buf1` is NULL, so
		 * nothing to free later */
		*buf = buf1;
		*buf_len = buf_len1;
		/* use new buffer */

	} else {

		/* no changes over the message, buffer is already generated,
		   just hide the original VIA for potential further branches */
		del_lump(req,req->h_via1->name.s-req->buf,req->h_via1->len,0);
	}


skip_update:

	/* save the SIP message into transaction */
	new_cell->uas.request = sip_msg_cloner( req, &sip_msg_len, 1);
	if (new_cell->uas.request==NULL) {
		/* reset any T triggering */
		new_cell->on_negative = 0;
		new_cell->on_reply = 0;
	} else {
		new_cell->uas.end_request=
			((char*)new_cell->uas.request)+sip_msg_len;
	}
	/* no parallel support in UAC transactions */
	new_cell->on_branch = 0;

	/* cleanup */
	if (new_proxy) {
		free_proxy( new_proxy );
		pkg_free( new_proxy );
	}

	if (ret_req) {
		*ret_req = req;
		/* if the buffer was rebuilt, return also the original buffer */
		*ret_req_buf = buf1 ? sipmsg_buf : NULL;
	} else {
		free_sip_msg(req);
		pkg_free(req);
		/* if the buffer was rebuilt, free the original one */
		if (buf1)
			shm_free(sipmsg_buf);
	}

	return 0;
}


/*
 * Send a request using data from the dialog structure
 */
int t_uac(str* method, str* headers, str* body, dlg_t* dialog,
				transaction_cb cb, void* cbp,release_tmcb_param release_func)
{
	union sockaddr_union to_su;
	struct cell *new_cell;
	struct retr_buf *request;
	struct sip_msg *req = NULL;
	char *buf_req = NULL;
	struct usr_avp **backup;
	char *buf;
	int buf_len;
	int ret, flags;
	unsigned int hi, method_id;
	struct proxy_l *proxy;
	struct tm_callback *it;

	ret=-1;

	/*** added by dcm
	 * - needed by external ua to send a request within a dlg
	 */
	if(!dialog->hooks.next_hop && w_calculate_hooks(dialog)<0)
		goto error_out;

	if(dialog->obp.s)
		dialog->hooks.next_hop = &dialog->obp;

	LM_DBG("next_hop=<%.*s>\n",dialog->hooks.next_hop->len,
			dialog->hooks.next_hop->s);

	/* calculate the socket corresponding to next hop */
	proxy = uri2proxy( dialog->hooks.next_hop,
		dialog->send_sock ? dialog->send_sock->proto : PROTO_NONE );
	if (proxy==0)  {
		ret=E_BAD_ADDRESS;
		goto error_out;
	}
	/* use the first address */
	hostent2su( &to_su,
		&proxy->host, proxy->addr_idx, proxy->port ? proxy->port:SIP_PORT);

	/* check/discover the send socket */
	if (dialog->send_sock) {
		/* if already set, the protocol of send sock must have the 
		   the same type as the proto required by destination URI */
		if (proxy->proto != dialog->send_sock->proto)
			dialog->send_sock = NULL;
	}
	if (dialog->send_sock==NULL) {
		/* get the send socket */
		dialog->send_sock = get_send_socket(NULL/*msg*/, &to_su, proxy->proto);
		if (!dialog->send_sock) {
			LM_ERR("no corresponding socket for af %d\n", to_su.s.sa_family);
			ser_error = E_NO_SOCKET;
			goto error3;
		}
	}
	LM_DBG("sending socket is %.*s \n",
		dialog->send_sock->name.len,dialog->send_sock->name.s);

	/* ***** Create TRANSACTION and all related  ***** */
	new_cell = build_cell( NULL/*msg*/, 1/*full UAS clone*/);
	if (!new_cell) {
		ret=E_OUT_OF_MEM;
		LM_ERR("short of cell shmem\n");
		goto error3;
	}

	/* if we have a dialog ctx, link it to the newly created transaction
	 * we might need it later for accessing dialog specific info */
	if (dialog->dialog_ctx)
		new_cell->dialog_ctx = dialog->dialog_ctx;
	if (has_new_local_tmcbs()) {
		if (parse_method(method->s, method->s + method->len, &method_id) == NULL)
			method_id = METHOD_UNDEF;
		run_new_local_callbacks(new_cell, NULL, method_id);
	}

	/* pass the transaction flags from dialog to transaction */
	new_cell->flags |= dialog->T_flags;

	/* add the callback the transaction for LOCAL_COMPLETED event */
	flags = TMCB_LOCAL_COMPLETED;
	/* Add also TMCB_LOCAL_RESPONSE_OUT if provisional replies are desired */
	if (pass_provisional_replies || pass_provisional(new_cell))
		flags |= TMCB_LOCAL_RESPONSE_OUT;
	if(cb && insert_tmcb(&(new_cell->tmcb_hl),flags,cb,cbp,release_func)!=1){
		ret=E_OUT_OF_MEM;
		LM_ERR("short of tmcb shmem\n");
		goto error2;
	}

	if (method->len==INVITE_LEN && memcmp(method->s, INVITE, INVITE_LEN)==0)
		new_cell->flags |= T_IS_INVITE_FLAG;
	new_cell->flags |= T_IS_LOCAL_FLAG;

	request = &new_cell->uac[0].request;
	if (dialog->forced_to_su.s.sa_family == AF_UNSPEC)
		request->dst.to = to_su;
	else
		request->dst.to = dialog->forced_to_su;
	request->dst.send_sock = dialog->send_sock;
	request->dst.proto = dialog->send_sock->proto;
	request->dst.proto_reserved1 = 0;

	hi=dlg2hash(dialog);
	LOCK_HASH(hi);
	insert_into_hash_table_unsafe(new_cell, hi);
	UNLOCK_HASH(hi);

	/* copy AVPs into transaction */
	new_cell->user_avps = dialog->avps;
	dialog->avps = NULL;

	/* set transaction AVP list */
	backup = set_avp_list( &new_cell->user_avps );

	/* run the "transaction created" callback if set */
	if (dialog->t_created_cb)
		dialog->t_created_cb( new_cell, dialog->t_created_cb_param);

	/* ***** Create the message buffer ***** */
	buf = build_uac_req(method, headers, body, dialog, 0, new_cell, &buf_len);
	if (!buf) {
		LM_ERR("failed to build message\n");
		ret=E_OUT_OF_MEM;
		goto error1;
	}

	/* run the local route */
	if (sroutes==NULL)
		LM_BUG("running local route/t_uac, but no routes in the process\n");
	run_local_route( new_cell, &buf, &buf_len, dialog, &req, &buf_req);

	if (request->buffer.s==NULL) {
		request->buffer.s = buf;
		request->buffer.len = buf_len;
	}

	/* for DNS based failover, copy the DNS proxy into transaction
	 * NOTE: while running the local route, the DNS proxy may be set
	 *   if a different one is needed (due destination change), so 
	 *   we clone it here ONLY if not set */
	if (!disable_dns_failover && new_cell->uac[0].proxy==NULL) {
		new_cell->uac[0].proxy = shm_clone_proxy( proxy, 1/*do_free*/);
		if (new_cell->uac[0].proxy==NULL)
			LM_ERR("failed to store DNS info -> no DNS based failover\n");
	}

	/* the method is the the very beginning of the buffer */
	new_cell->method.s = request->buffer.s;
	new_cell->method.len = method->len;

	new_cell->nr_of_outgoings++;

	if(last_localT) {
		*last_localT = new_cell;
		REF_UNSAFE(new_cell);
	}

	if (new_cell->uac[0].br_flags & tcp_no_new_conn_bflag)
		tcp_no_new_conn = 1;

	set_bavp_list(&new_cell->uac[0].user_avps);
	if (SEND_BUFFER(request) == -1) {
		LM_ERR("attempt to send to '%.*s' failed\n",
			dialog->hooks.next_hop->len,
			dialog->hooks.next_hop->s);
	}
	reset_bavp_list();

	tcp_no_new_conn = 0;

	if (method->len==ACK_LEN && memcmp(method->s, ACK, ACK_LEN)==0 ) {
		t_release_transaction(new_cell);
	} else {
		start_retr(request);
	}

	/* successfully sent out */

	if ( req ) {
		/* run callbacks 
		 * NOTE: this callback will be executed ONLY if the local route
		 * was executed (so we have the msg) */
		if ( has_tran_tmcbs( new_cell, TMCB_MSG_SENT_OUT) ) {
			set_extra_tmcb_params( &request->buffer,
				&request->dst);
			run_trans_callbacks( TMCB_MSG_SENT_OUT, new_cell,
				req, 0, 0);
		}
		free_sip_msg(req);
		pkg_free(req);
		if (buf_req) shm_free(buf_req);
	}

	set_avp_list( backup );
	free_proxy( proxy );
	pkg_free( proxy );

	return 1;

error1:
	LOCK_HASH(hi);
	remove_from_hash_table_unsafe(new_cell);
	UNLOCK_HASH(hi);
error2:
	/* if we set a callback, prevent any eventual release function to run
	 * and let the upper layer to take care of distoying the callback params
	 * or any resources it passed here. */
	if (cb && release_func)
		for(it=new_cell->tmcb_hl.first ; it ; it=it->next)
			if (it->callback==cb) {
				it->release=NULL;
				break;
			}
	free_cell(new_cell);
error3:
	free_proxy( proxy );
	pkg_free( proxy );
error_out:
	return ret;
}


/*
 * Send a message within a dialog
 */
int req_within(str* method, str* headers, str* body, dlg_t* dialog,
		transaction_cb completion_cb,void* cbp,release_tmcb_param release_func)
{
	if (!method || !dialog) {
		LM_ERR("invalid parameter value\n");
		goto err;
	}

	if (dialog->state != DLG_CONFIRMED) {
		LM_ERR("dialog is not confirmed yet\n");
		goto err;
	}

	if ( !( ((method->len == 3) && !memcmp("ACK", method->s, 3)) ||
	        ((method->len == 6) && !memcmp("CANCEL", method->s, 6)) ) )  {
		dialog->loc_seq.value++; /* Increment CSeq */
	}

	return t_uac(method, headers, body, dialog,
		completion_cb, cbp, release_func);
err:
	return -1;
}


/*
 * Send an initial request that will start a dialog
 */
int req_outside(str* method, str* to, str* from,
	str* headers, str* body, dlg_t** dialog,
	transaction_cb cb, void* cbp,release_tmcb_param release_func)
{
	str callid, fromtag;

	if (check_params(method, to, from, dialog) < 0) goto err;

	generate_callid(&callid);
	generate_fromtag(&fromtag, &callid);

	if (new_dlg_uac(&callid, &fromtag, DEFAULT_CSEQ, from, to, NULL, dialog) < 0) {
		LM_ERR("failed to create new dialog\n");
		goto err;
	}

	return t_uac(method, headers, body, *dialog, cb, cbp, release_func);
err:
	return -1;
}


/*
 * Send a transactional request, no dialogs involved
 */
int request(str* m, str* ruri, str* to, str* from, str* h, str* b, str *oburi,
				transaction_cb cb, void* cbp,release_tmcb_param release_func)
{
	str callid, fromtag;
	dlg_t* dialog;
	int res;

	if (check_params(m, to, from, &dialog) < 0) goto err;

	generate_callid(&callid);
	generate_fromtag(&fromtag, &callid);

	if (new_dlg_uac(&callid, &fromtag, DEFAULT_CSEQ, from, to, NULL, &dialog) < 0) {
		LM_ERR("failed to create temporary dialog\n");
		goto err;
	}

	if (ruri) {
		dialog->rem_target.s = ruri->s;
		dialog->rem_target.len = ruri->len;
		dialog->hooks.request_uri = &dialog->rem_target;
	}

	if (oburi && oburi->s) dialog->hooks.next_hop = oburi;

	w_calculate_hooks(dialog);

	res = t_uac(m, h, b, dialog, cb, cbp, release_func);
	dialog->rem_target.s = 0;
	free_dlg(dialog);
	return res;

err:
	return -1;
}


void setlocalTholder(struct cell** holder)
{
	last_localT = holder;
}
