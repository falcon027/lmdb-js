/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 1999-2005 The OpenLDAP Foundation.
 * Portions Copyright 2001-2003 Pierangelo Masarati.
 * Portions Copyright 1999-2003 Howard Chu.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */
/* ACKNOWLEDGEMENTS:
 * This work was initially developed by the Howard Chu for inclusion
 * in OpenLDAP Software and subsequently enhanced by Pierangelo
 * Masarati.
 */

#include "portable.h"

#include <stdio.h>

#include <ac/socket.h>
#include <ac/string.h>


#define AVL_INTERNAL
#include "slap.h"
#include "../back-ldap/back-ldap.h"
#include "back-meta.h"

/*
 * Set PRINT_CONNTREE larger than 0 to dump the connection tree (debug only)
 */
#define PRINT_CONNTREE 0

/*
 * meta_back_conn_cmp
 *
 * compares two struct metaconn based on the value of the conn pointer;
 * used by avl stuff
 */
int
meta_back_conn_cmp(
	const void *c1,
	const void *c2
	)
{
	struct metaconn *lc1 = ( struct metaconn * )c1;
        struct metaconn *lc2 = ( struct metaconn * )c2;
	
	return SLAP_PTRCMP( lc1->mc_conn, lc2->mc_conn );
}

/*
 * meta_back_conn_dup
 *
 * returns -1 in case a duplicate struct metaconn has been inserted;
 * used by avl stuff
 */
int
meta_back_conn_dup(
	void *c1,
	void *c2
	)
{
	struct metaconn *lc1 = ( struct metaconn * )c1;
	struct metaconn *lc2 = ( struct metaconn * )c2;

	return( ( lc1->mc_conn == lc2->mc_conn ) ? -1 : 0 );
}

/*
 * Debug stuff (got it from libavl)
 */
#if PRINT_CONNTREE > 0
static void
ravl_print( Avlnode *root, int depth )
{
	int     i;
	
	if ( root == 0 ) {
		return;
	}
	
	ravl_print( root->avl_right, depth + 1 );
	
	for ( i = 0; i < depth; i++ ) {
		printf( "    " );
	}

	printf( "c(%d) %d\n", ( ( struct metaconn * )root->avl_data )->mc_conn->c_connid, root->avl_bf );
	
	ravl_print( root->avl_left, depth + 1 );
}

static void
myprint( Avlnode *root )
{
	printf( "********\n" );
	
	if ( root == 0 ) {
		printf( "\tNULL\n" );
	} else {
		ravl_print( root, 0 );
	}
	
	printf( "********\n" );
}
#endif /* PRINT_CONNTREE */
/*
 * End of debug stuff
 */

/*
 * metaconn_alloc
 * 
 * Allocates a connection structure, making room for all the referenced targets
 */
static struct metaconn *
metaconn_alloc( int ntargets )
{
	struct metaconn *lc;

	assert( ntargets > 0 );

	lc = ch_calloc( sizeof( struct metaconn ), 1 );
	if ( lc == NULL ) {
		return NULL;
	}
	
	/*
	 * make it a null-terminated array ...
	 */
	lc->mc_conns = ch_calloc( sizeof( struct metasingleconn ), ntargets + 1 );
	if ( lc->mc_conns == NULL ) {
		free( lc );
		return NULL;
	}

	/* FIXME: needed by META_LAST() */
	lc->mc_conns[ ntargets ].msc_candidate = META_LAST_CONN;

	for ( ; ntargets-- > 0; ) {
		lc->mc_conns[ ntargets ].msc_ld = NULL;
		BER_BVZERO( &lc->mc_conns[ ntargets ].msc_bound_ndn );
		BER_BVZERO( &lc->mc_conns[ ntargets ].msc_cred );
		lc->mc_conns[ ntargets ].msc_bound = META_UNBOUND;
	}

	lc->mc_bound_target = META_BOUND_NONE;

	return lc;
}

/*
 * metaconn_free
 *
 * clears a metaconn
 */
static void
metaconn_free(
		struct metaconn *lc
)
{
	if ( !lc ) {
		return;
	}
	
	if ( lc->mc_conns ) {
		ch_free( lc->mc_conns );
	}

	free( lc );
}

/*
 * init_one_conn
 * 
 * Initializes one connection
 */
static int
init_one_conn(
		Operation		*op,
		SlapReply		*rs,
		struct metatarget	*lt, 
		struct metasingleconn	*lsc,
		char			*candidate,
		ldap_back_send_t	sendok )
{
	struct metainfo	*li = ( struct metainfo * )op->o_bd->be_private;
	int		vers;
	dncookie	dc;

	/*
	 * Already init'ed
	 */
	if ( lsc->msc_ld != NULL ) {
		rs->sr_err = LDAP_SUCCESS;
		goto error_return;
	}
       
	/*
	 * Attempts to initialize the connection to the target ds
	 */
	rs->sr_err = ldap_initialize( &lsc->msc_ld, lt->mt_uri );
	if ( rs->sr_err != LDAP_SUCCESS ) {
		goto error_return;
	}

	/*
	 * Set LDAP version. This will always succeed: If the client
	 * bound with a particular version, then so can we.
	 */
	vers = op->o_conn->c_protocol;
	ldap_set_option( lsc->msc_ld, LDAP_OPT_PROTOCOL_VERSION, &vers );

	/* automatically chase referrals ("chase-referrals"/"dont-chase-referrals" statement) */
	if ( LDAP_BACK_CHASE_REFERRALS( li ) ) {
		ldap_set_option( lsc->msc_ld, LDAP_OPT_REFERRALS, LDAP_OPT_ON );
	}

#ifdef HAVE_TLS
	/* start TLS ("start-tls"/"try-start-tls" statements) */
	if ( ( LDAP_BACK_USE_TLS( li ) || ( op->o_conn->c_is_tls && LDAP_BACK_PROPAGATE_TLS( li ) ) )
			&& !ldap_is_ldaps_url( lt->mt_uri ) )
	{
#ifdef SLAP_STARTTLS_ASYNCHRONOUS
		/*
		 * use asynchronous StartTLS
		 * in case, chase referral (not implemented yet)
		 */
		int		msgid;

		rs->sr_err = ldap_start_tls( lsc->msc_ld, NULL, NULL, &msgid );
		if ( rs->sr_err == LDAP_SUCCESS ) {
			LDAPMessage	*res = NULL;
			int		rc, retries = 1;
			struct timeval	tv = { 0, 0 };

retry:;
			rc = ldap_result( lsc->msc_ld, msgid, LDAP_MSG_ALL, &tv, &res );
			if ( rc < 0 ) {
				rs->sr_err = LDAP_OTHER;

			} else if ( rc == 0 ) {
				if ( retries ) {
					retries--;
					tv.tv_sec = 0;
					tv.tv_usec = 100000;
					goto retry;
				}
				rs->sr_err = LDAP_OTHER;

			} else if ( rc == LDAP_RES_EXTENDED ) {
				struct berval	*data = NULL;

				rs->sr_err = ldap_parse_extended_result( lsc->msc_ld, res,
						NULL, &data, 0 );
				if ( rs->sr_err == LDAP_SUCCESS ) {
					rs->sr_err = ldap_result2error( lsc->msc_ld, res, 1 );
					res = NULL;
					
					/* FIXME: in case a referral 
					 * is returned, should we try
					 * using it instead of the 
					 * configured URI? */
					if ( rs->sr_err == LDAP_SUCCESS ) {
						ldap_install_tls( lsc->msc_ld );

					} else if ( rs->sr_err == LDAP_REFERRAL ) {
						rs->sr_err = LDAP_OTHER;
						rs->sr_text = "unwilling to chase referral returned by Start TLS exop";
					}

					if ( data ) {
						if ( data->bv_val ) {
							ber_memfree( data->bv_val );
						}
						ber_memfree( data );
					}
				}

			} else {
				rs->sr_err = LDAP_OTHER;
			}

			if ( res != NULL ) {
				ldap_msgfree( res );
			}
		}
#else /* ! SLAP_STARTTLS_ASYNCHRONOUS */
		/*
		 * use synchronous StartTLS
		 */
		rs->sr_err = ldap_start_tls_s( lsc->msc_ld, NULL, NULL );
#endif /* ! SLAP_STARTTLS_ASYNCHRONOUS */

		/* if StartTLS is requested, only attempt it if the URL
		 * is not "ldaps://"; this may occur not only in case
		 * of misconfiguration, but also when used in the chain 
		 * overlay, where the "uri" can be parsed out of a referral */
		if ( rs->sr_err == LDAP_SERVER_DOWN
				|| ( rs->sr_err != LDAP_SUCCESS && LDAP_BACK_TLS_CRITICAL( li ) ) )
		{
			ldap_unbind_ext_s( lsc->msc_ld, NULL, NULL );
			goto error_return;
		}
	}
#endif /* HAVE_TLS */

	/*
	 * Set the network timeout if set
	 */
	if ( li->mi_network_timeout != 0 ) {
		struct timeval	network_timeout;

		network_timeout.tv_usec = 0;
		network_timeout.tv_sec = li->mi_network_timeout;

		ldap_set_option( lsc->msc_ld, LDAP_OPT_NETWORK_TIMEOUT,
				(void *)&network_timeout );
	}

	/*
	 * Sets a cookie for the rewrite session
	 */
	( void )rewrite_session_init( lt->mt_rwmap.rwm_rw, op->o_conn );

	/*
	 * If the connection DN is not null, an attempt to rewrite it is made
	 */
	if ( !BER_BVISEMPTY( &op->o_conn->c_dn ) ) {
		dc.rwmap = &lt->mt_rwmap;
		dc.conn = op->o_conn;
		dc.rs = rs;
		dc.ctx = "bindDN";
		
		/*
		 * Rewrite the bind dn if needed
		 */
		if ( ldap_back_dn_massage( &dc, &op->o_conn->c_dn,
					&lsc->msc_bound_ndn ) )
		{
			goto error_return;
		}

		/* copy the DN idf needed */
		if ( lsc->msc_bound_ndn.bv_val == op->o_conn->c_dn.bv_val ) {
			ber_dupbv( &lsc->msc_bound_ndn, &op->o_conn->c_dn );
		}

		assert( !BER_BVISNULL( &lsc->msc_bound_ndn ) );

	} else {
		ber_str2bv( "", 0, 1, &lsc->msc_bound_ndn );
	}

	lsc->msc_bound = META_UNBOUND;

error_return:;
	if ( rs->sr_err != LDAP_SUCCESS ) {
		rs->sr_err = slap_map_api2result( rs );
		if ( sendok & LDAP_BACK_SENDERR ) {
			send_ldap_result( op, rs );
			rs->sr_text = NULL;
		}

	} else {

		/*
		 * The candidate is activated
		 */
		*candidate = META_CANDIDATE;
	}

	return rs->sr_err;
}

/*
 * callback for unique candidate selection
 */
static int
meta_back_conn_cb( Operation *op, SlapReply *rs )
{
	assert( op->o_tag == LDAP_REQ_SEARCH );

	switch ( rs->sr_type ) {
	case REP_SEARCH:
		((int *)op->o_callback->sc_private)[0] = (int)op->o_private;
		break;

	case REP_SEARCHREF:
	case REP_RESULT:
		break;

	default:
		return rs->sr_err;
	}

	return 0;
}


static int
meta_back_get_candidate(
	Operation	*op,
	SlapReply	*rs,
	struct berval	*ndn )
{
	struct metainfo	*li = ( struct metainfo * )op->o_bd->be_private;
	int		candidate;

	/*
	 * tries to get a unique candidate
	 * (takes care of default target)
	 */
	candidate = meta_back_select_unique_candidate( li, ndn );

	/*
	 * if any is found, inits the connection
	 */
	if ( candidate == META_TARGET_NONE ) {
		rs->sr_err = LDAP_NO_SUCH_OBJECT;
		rs->sr_text = "no suitable candidate target found";

	} else if ( candidate == META_TARGET_MULTIPLE ) {
		Filter		f = { 0 };
		Operation	op2 = *op;
		SlapReply	rs2 = { 0 };
		slap_callback	cb2 = { 0 };
		int		rc;

		/* try to get a unique match for the request ndn
		 * among the multiple candidates available */
		op2.o_tag = LDAP_REQ_SEARCH;
		op2.o_req_dn = *ndn;
		op2.o_req_ndn = *ndn;
		op2.ors_scope = LDAP_SCOPE_BASE;
		op2.ors_deref = LDAP_DEREF_NEVER;
		op2.ors_attrs = slap_anlist_no_attrs;
		op2.ors_attrsonly = 0;
		op2.ors_limit = NULL;
		op2.ors_slimit = 1;
		op2.ors_tlimit = SLAP_NO_LIMIT;

		f.f_choice = LDAP_FILTER_PRESENT;
		f.f_desc = slap_schema.si_ad_objectClass;
		op2.ors_filter = &f;
		BER_BVSTR( &op2.ors_filterstr, "(objectClass=*)" );

		op2.o_callback = &cb2;
		cb2.sc_response = meta_back_conn_cb;
		cb2.sc_private = (void *)&candidate;

		rc = op->o_bd->be_search( &op2, &rs2 );

		switch ( rs2.sr_err ) {
		case LDAP_SUCCESS:
		default:
			rs->sr_err = rs2.sr_err;
			break;

		case LDAP_SIZELIMIT_EXCEEDED:
			/* if multiple candidates can serve the operation,
			 * and a default target is defined, and it is
			 * a candidate, try using it (FIXME: YMMV) */
			if ( li->mi_defaulttarget != META_DEFAULT_TARGET_NONE
				&& meta_back_is_candidate( &li->mi_targets[ li->mi_defaulttarget ]->mt_nsuffix, ndn ) )
			{
				candidate = li->mi_defaulttarget;
				rs->sr_err = LDAP_SUCCESS;
				rs->sr_text = NULL;

			} else {
				rs->sr_err = LDAP_UNWILLING_TO_PERFORM;
				rs->sr_text = "cannot select unique candidate target";
			}
			break;
		}
	}

	return candidate;
}

static void
meta_back_candidate_keyfree( void *key, void *data )
{
	ber_memfree_x( data, NULL );
}

char *
meta_back_candidates_get( Operation *op )
{
	struct metainfo	*li = ( struct metainfo * )op->o_bd->be_private;
	void		*data = NULL;

	if ( op->o_threadctx ) {
		ldap_pvt_thread_pool_getkey( op->o_threadctx,
				meta_back_candidate_keyfree, &data, NULL );
	} else {
		data = (void *)li->mi_candidates;
	}

	if ( data == NULL ) {
		data = ber_memalloc_x( sizeof( char ) * li->mi_ntargets, NULL );
		if ( op->o_threadctx ) {
			ldap_pvt_thread_pool_setkey( op->o_threadctx,
					meta_back_candidate_keyfree, data,
					meta_back_candidate_keyfree );

		} else {
			li->mi_candidates = (char *)data;
		}
	}

	return (char *)data;
}

/*
 * meta_back_getconn
 * 
 * Prepares the connection structure
 * 
 * RATIONALE:
 *
 * - determine what DN is being requested:
 *
 *	op	requires candidate	checks
 *
 *	add	unique			parent of o_req_ndn
 *	bind	unique^*[/all]		o_req_ndn [no check]
 *	compare	unique^+		o_req_ndn
 *	delete	unique			o_req_ndn
 *	modify	unique			o_req_ndn
 *	search	any			o_req_ndn
 *	modrdn	unique[, unique]	o_req_ndn[, orr_nnewSup]
 *
 * - for ops that require the candidate to be unique, in case of multiple
 *   occurrences an internal search with sizeLimit=1 is performed
 *   if a unique candidate can actually be determined.  If none is found,
 *   the operation aborts; if multiple are found, the default target
 *   is used if defined and candidate; otherwise the operation aborts.
 *
 * *^note: actually, the bind operation is handled much like a search;
 *   i.e. the bind is broadcast to all candidate targets.
 *
 * +^note: actually, the compare operation is handled much like a search;
 *   i.e. the compare is broadcast to all candidate targets, while checking
 *   that exactly none (noSuchObject) or one (TRUE/FALSE/UNDEFINED) is
 *   returned.
 */
struct metaconn *
meta_back_getconn(
	       	Operation 		*op,
		SlapReply		*rs,
		int 			*candidate,
		ldap_back_send_t	sendok )
{
	struct metainfo	*li = ( struct metainfo * )op->o_bd->be_private;
	struct metaconn	*lc, lc_curr;
	int		cached = META_TARGET_NONE,
			i = META_TARGET_NONE,
			err = LDAP_SUCCESS,
			new_conn = 0;

	meta_op_type	op_type = META_OP_REQUIRE_SINGLE;
	int		parent = 0,
			newparent = 0;
	struct berval	ndn = op->o_req_ndn;

	char		*candidates = meta_back_candidates_get( op );

	/* Searches for a metaconn in the avl tree */
	lc_curr.mc_conn = op->o_conn;
	ldap_pvt_thread_mutex_lock( &li->mi_conn_mutex );
	lc = (struct metaconn *)avl_find( li->mi_conntree, 
		(caddr_t)&lc_curr, meta_back_conn_cmp );
	ldap_pvt_thread_mutex_unlock( &li->mi_conn_mutex );

	switch ( op->o_tag ) {
	case LDAP_REQ_ADD:
		/* if we go to selection, the entry must not exist,
		 * and we must be able to resolve the parent */
		parent = 1;
		dnParent( &ndn, &ndn );
		break;

	case LDAP_REQ_MODRDN:
		/* if nnewSuperior is not NULL, it must resolve
		 * to the same candidate as the req_ndn */
		if ( op->orr_nnewSup ) {
			newparent = 1;
		}
		break;

	case LDAP_REQ_BIND:
		/* if bound as rootdn, the backend must bind to all targets
		 * with the administrative identity */
		if ( op->orb_method == LDAP_AUTH_SIMPLE && be_isroot_pw( op ) ) {
			op_type = META_OP_REQUIRE_ALL;
		}
		break;

	case LDAP_REQ_DELETE:
	case LDAP_REQ_MODIFY:
		/* just a unique candidate */
		break;

	case LDAP_REQ_COMPARE:
	case LDAP_REQ_SEARCH:
		/* allow multiple candidates for the searchBase */
		op_type = META_OP_ALLOW_MULTIPLE;
		break;

	default:
		/* right now, just break (exop?) */
		break;
	}

	/*
	 * require all connections ...
	 */
	if ( op_type == META_OP_REQUIRE_ALL ) {

		/* Looks like we didn't get a bind. Open a new session... */
		if ( !lc ) {
			lc = metaconn_alloc( li->mi_ntargets );
			lc->mc_conn = op->o_conn;
			new_conn = 1;
		}

		for ( i = 0; i < li->mi_ntargets; i++ ) {

			/*
			 * The target is activated; if needed, it is
			 * also init'd
			 */
			int lerr = init_one_conn( op, rs, li->mi_targets[ i ],
					&lc->mc_conns[ i ], &candidates[ i ],
					sendok );
			if ( lerr != LDAP_SUCCESS ) {
				
				/*
				 * FIXME: in case one target cannot
				 * be init'd, should the other ones
				 * be tried?
				 */
				candidates[ i ] = META_NOT_CANDIDATE;
#if 0
				( void )meta_clear_one_candidate( &lc->mc_conns[ i ] );
#endif
				err = lerr;
				continue;
			}
		}
		goto done;
	}
	
	/*
	 * looks in cache, if any
	 */
	if ( li->mi_cache.ttl != META_DNCACHE_DISABLED ) {
		cached = i = meta_dncache_get_target( &li->mi_cache, &op->o_req_ndn );
	}

	if ( op_type == META_OP_REQUIRE_SINGLE ) {

		memset( candidates, META_NOT_CANDIDATE, sizeof( char ) * li->mi_ntargets );

		/*
		 * tries to get a unique candidate
		 * (takes care of default target)
		 */
		if ( i == META_TARGET_NONE ) {
			i = meta_back_get_candidate( op, rs, &ndn );

			if ( rs->sr_err != LDAP_SUCCESS ) {
				if ( sendok & LDAP_BACK_SENDERR ) {
					send_ldap_result( op, rs );
					rs->sr_text = NULL;
				}
				return NULL;
			}
		}

		if ( newparent && meta_back_get_candidate( op, rs, op->orr_nnewSup ) != i )
		{
			rs->sr_err = LDAP_UNWILLING_TO_PERFORM;
			rs->sr_text = "cross-target rename not supported";
			if ( sendok & LDAP_BACK_SENDERR ) {
				send_ldap_result( op, rs );
				rs->sr_text = NULL;
			}
			return NULL;
		}

		Debug( LDAP_DEBUG_CACHE,
	"==>meta_back_getconn: got target %d for ndn=\"%s\" from cache\n",
				i, op->o_req_ndn.bv_val, 0 );

		/* Retries searching for a metaconn in the avl tree */
		lc_curr.mc_conn = op->o_conn;
		ldap_pvt_thread_mutex_lock( &li->mi_conn_mutex );
		lc = (struct metaconn *)avl_find( li->mi_conntree, 
			(caddr_t)&lc_curr, meta_back_conn_cmp );
		ldap_pvt_thread_mutex_unlock( &li->mi_conn_mutex );

		/* Looks like we didn't get a bind. Open a new session... */
		if ( !lc ) {
			lc = metaconn_alloc( li->mi_ntargets );
			lc->mc_conn = op->o_conn;
			new_conn = 1;
		}

		/*
		 * Clear all other candidates
		 */
		( void )meta_clear_unused_candidates( op, lc, i );

		/*
		 * The target is activated; if needed, it is
		 * also init'd. In case of error, init_one_conn
		 * sends the appropriate result.
		 */
		err = init_one_conn( op, rs, li->mi_targets[ i ],
				&lc->mc_conns[ i ], &candidates[ i ],
				sendok );
		if ( err != LDAP_SUCCESS ) {
		
			/*
			 * FIXME: in case one target cannot
			 * be init'd, should the other ones
			 * be tried?
			 */
			candidates[ i ] = META_NOT_CANDIDATE;
 			if ( new_conn ) {
				( void )meta_clear_one_candidate( &lc->mc_conns[ i ] );
				metaconn_free( lc );
			}
			return NULL;
		}

		if ( candidate ) {
			*candidate = i;
		}

	/*
	 * if no unique candidate ...
	 */
	} else {

		/* Looks like we didn't get a bind. Open a new session... */
		if ( !lc ) {
			lc = metaconn_alloc( li->mi_ntargets );
			lc->mc_conn = op->o_conn;
			new_conn = 1;
		}

		for ( i = 0; i < li->mi_ntargets; i++ ) {
			if ( i == cached 
				|| meta_back_is_candidate( &li->mi_targets[ i ]->mt_nsuffix,
						&op->o_req_ndn ) )
			{

				/*
				 * The target is activated; if needed, it is
				 * also init'd
				 */
				int lerr = init_one_conn( op, rs,
						li->mi_targets[ i ],
						&lc->mc_conns[ i ],
						&candidates[ i ],
						sendok );
				if ( lerr != LDAP_SUCCESS ) {
				
					/*
					 * FIXME: in case one target cannot
					 * be init'd, should the other ones
					 * be tried?
					 */
					candidates[ i ] = META_NOT_CANDIDATE;
#if 0
					( void )meta_clear_one_candidate( &lc->mc_conns[ i ] );
#endif
					err = lerr;
					continue;
				}

			} else {
				candidates[ i ] = META_NOT_CANDIDATE;
			}
		}
	}

done:;
	/* clear out init_one_conn non-fatal errors */
	rs->sr_err = LDAP_SUCCESS;
	rs->sr_text = NULL;

	if ( new_conn ) {
		
		/*
		 * Inserts the newly created metaconn in the avl tree
		 */
		ldap_pvt_thread_mutex_lock( &li->mi_conn_mutex );
		err = avl_insert( &li->mi_conntree, ( caddr_t )lc,
			       	meta_back_conn_cmp, meta_back_conn_dup );

#if PRINT_CONNTREE > 0
		myprint( li->mi_conntree );
#endif /* PRINT_CONNTREE */
		
		ldap_pvt_thread_mutex_unlock( &li->mi_conn_mutex );

		Debug( LDAP_DEBUG_TRACE,
			"=>meta_back_getconn: conn %ld inserted\n",
			lc->mc_conn->c_connid, 0, 0 );
		
		/*
		 * Err could be -1 in case a duplicate metaconn is inserted
		 */
		if ( err != 0 ) {
			rs->sr_err = LDAP_OTHER;
			rs->sr_text = "Internal server error";
			metaconn_free( lc );
			if ( sendok & LDAP_BACK_SENDERR ) {
				send_ldap_result( op, rs );
				rs->sr_text = NULL;
			}
			return NULL;
		}

	} else {
		Debug( LDAP_DEBUG_TRACE,
			"=>meta_back_getconn: conn %ld fetched\n",
			lc->mc_conn->c_connid, 0, 0 );
	}
	
	return lc;
}

