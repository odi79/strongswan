/*
 * Copyright (C) 2008 Martin Willi
 * Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * $Id$
 */

#include "stroke_list.h"

#include <daemon.h>
#include <credentials/certificates/x509.h>
#include <credentials/certificates/crl.h>

/* warning intervals for list functions */
#define CERT_WARNING_INTERVAL  30	/* days */
#define CRL_WARNING_INTERVAL	7	/* days */

typedef struct private_stroke_list_t private_stroke_list_t;

/**
 * private data of stroke_list
 */
struct private_stroke_list_t {

	/**
	 * public functions
	 */
	stroke_list_t public;
	
	/**
	 * timestamp of daemon start
	 */
	time_t uptime;
};

/**
 * log an IKE_SA to out
 */
static void log_ike_sa(FILE *out, ike_sa_t *ike_sa, bool all)
{
	ike_sa_id_t *id = ike_sa->get_id(ike_sa);
	u_int32_t rekey, reauth;

	fprintf(out, "%12s[%d]: %N, %H[%D]...%H[%D]\n",
			ike_sa->get_name(ike_sa), ike_sa->get_unique_id(ike_sa),
			ike_sa_state_names, ike_sa->get_state(ike_sa),
			ike_sa->get_my_host(ike_sa), ike_sa->get_my_id(ike_sa),
			ike_sa->get_other_host(ike_sa), ike_sa->get_other_id(ike_sa));
	
	if (all)
	{
		fprintf(out, "%12s[%d]: IKE SPIs: %.16llx_i%s %.16llx_r%s",
				ike_sa->get_name(ike_sa), ike_sa->get_unique_id(ike_sa),
				id->get_initiator_spi(id), id->is_initiator(id) ? "*" : "",
				id->get_responder_spi(id), id->is_initiator(id) ? "" : "*");
	
		rekey = ike_sa->get_statistic(ike_sa, STAT_REKEY_TIME);
		reauth = ike_sa->get_statistic(ike_sa, STAT_REAUTH_TIME);
		if (rekey)
		{
			fprintf(out, ", rekeying in %V", &rekey);
		}
		if (reauth)
		{
			fprintf(out, ", reauthentication in %V", &reauth);
		}
		if (!rekey && !reauth)
		{
			fprintf(out, ", rekeying disabled");
		}
		fprintf(out, "\n");
	}
}

/**
 * log an CHILD_SA to out
 */
static void log_child_sa(FILE *out, child_sa_t *child_sa, bool all)
{
	u_int32_t rekey, now = time(NULL);
	u_int32_t use_in, use_out, use_fwd;
	encryption_algorithm_t encr_alg;
	integrity_algorithm_t int_alg;
	size_t encr_len, int_len;
	mode_t mode;
	
	child_sa->get_stats(child_sa, &mode, &encr_alg, &encr_len,
						&int_alg, &int_len, &rekey, &use_in, &use_out,
						&use_fwd);
	
	fprintf(out, "%12s{%d}:  %N, %N", 
			child_sa->get_name(child_sa), child_sa->get_reqid(child_sa),
			child_sa_state_names, child_sa->get_state(child_sa),
			mode_names, mode);
	
	if (child_sa->get_state(child_sa) == CHILD_INSTALLED)
	{
		fprintf(out, ", %N SPIs: %.8x_i %.8x_o",
				protocol_id_names, child_sa->get_protocol(child_sa),
				htonl(child_sa->get_spi(child_sa, TRUE)),
				htonl(child_sa->get_spi(child_sa, FALSE)));
		
		if (all)
		{
			fprintf(out, "\n%12s{%d}:  ", child_sa->get_name(child_sa), 
					child_sa->get_reqid(child_sa));
			
			
			if (child_sa->get_protocol(child_sa) == PROTO_ESP)
			{
				fprintf(out, "%N", encryption_algorithm_names, encr_alg);
				
				if (encr_len)
				{
					fprintf(out, "-%d", encr_len);
				}
				fprintf(out, "/");
			}
			
			fprintf(out, "%N", integrity_algorithm_names, int_alg);
			if (int_len)
			{
				fprintf(out, "-%d", int_len);
			}
			fprintf(out, ", rekeying ");
			
			if (rekey)
			{
				fprintf(out, "in %#V", &now, &rekey);
			}
			else
			{
				fprintf(out, "disabled");
			}
			
			fprintf(out, ", last use: ");
			use_in = max(use_in, use_fwd);
			if (use_in)
			{
				fprintf(out, "%ds_i ", now - use_in);
			}
			else
			{
				fprintf(out, "no_i ");
			}
			if (use_out)
			{
				fprintf(out, "%ds_o ", now - use_out);
			}
			else
			{
				fprintf(out, "no_o ");
			}
		}
	}
	
	fprintf(out, "\n%12s{%d}:   %#R=== %#R\n",
			child_sa->get_name(child_sa), child_sa->get_reqid(child_sa),
			child_sa->get_traffic_selectors(child_sa, TRUE),
			child_sa->get_traffic_selectors(child_sa, FALSE));
}

/**
 * Implementation of stroke_list_t.status.
 */
static void status(private_stroke_list_t *this, stroke_msg_t *msg, FILE *out, bool all)
{
	enumerator_t *enumerator, *children;
	iterator_t *iterator;
	host_t *host;
	peer_cfg_t *peer_cfg;
	ike_cfg_t *ike_cfg;
	child_cfg_t *child_cfg;
	ike_sa_t *ike_sa;
	char *name = NULL;
	time_t uptime;
	
	name = msg->status.name;
	
	if (all)
	{
		uptime = time(NULL) - this->uptime;
		fprintf(out, "Performance:\n");
		fprintf(out, "  uptime: %V, since %T\n", &uptime, &this->uptime);
		fprintf(out, "  worker threads: %d idle of %d,",
				charon->processor->get_idle_threads(charon->processor),
				charon->processor->get_total_threads(charon->processor));
		fprintf(out, " job queue load: %d,",
				charon->processor->get_job_load(charon->processor));
		fprintf(out, " scheduled events: %d\n",
				charon->scheduler->get_job_load(charon->scheduler));
		iterator = charon->kernel_interface->create_address_iterator(
													charon->kernel_interface);
		fprintf(out, "Listening IP addresses:\n");
		while (iterator->iterate(iterator, (void**)&host))
		{
			fprintf(out, "  %H\n", host);
		}
		iterator->destroy(iterator);
	
		fprintf(out, "Connections:\n");
		enumerator = charon->backends->create_peer_cfg_enumerator(charon->backends);
		while (enumerator->enumerate(enumerator, (void**)&peer_cfg))
		{
			if (peer_cfg->get_ike_version(peer_cfg) != 2 ||
				(name && !streq(name, peer_cfg->get_name(peer_cfg))))
			{
				continue;
			}
			
			ike_cfg = peer_cfg->get_ike_cfg(peer_cfg);
			fprintf(out, "%12s:  %H[%D]...%H[%D]\n", peer_cfg->get_name(peer_cfg),
					ike_cfg->get_my_host(ike_cfg), peer_cfg->get_my_id(peer_cfg),
					ike_cfg->get_other_host(ike_cfg), peer_cfg->get_other_id(peer_cfg));
			/* TODO: list CAs and groups */
			children = peer_cfg->create_child_cfg_enumerator(peer_cfg);
			while (children->enumerate(children, &child_cfg))
			{
				linked_list_t *my_ts, *other_ts;
				my_ts = child_cfg->get_traffic_selectors(child_cfg, TRUE, NULL, NULL);
				other_ts = child_cfg->get_traffic_selectors(child_cfg, FALSE, NULL, NULL);
				fprintf(out, "%12s:    %#R=== %#R\n", child_cfg->get_name(child_cfg),
						my_ts, other_ts);
				my_ts->destroy_offset(my_ts, offsetof(traffic_selector_t, destroy));
				other_ts->destroy_offset(other_ts, offsetof(traffic_selector_t, destroy));
			}
			children->destroy(children);
		}
		enumerator->destroy(enumerator);
	}
	
	iterator = charon->ike_sa_manager->create_iterator(charon->ike_sa_manager);
	if (all && iterator->get_count(iterator) > 0)
	{
		fprintf(out, "Security Associations:\n");
	}
	while (iterator->iterate(iterator, (void**)&ike_sa))
	{
		bool ike_printed = FALSE;
		child_sa_t *child_sa;
		iterator_t *children = ike_sa->create_child_sa_iterator(ike_sa);

		if (name == NULL || streq(name, ike_sa->get_name(ike_sa)))
		{
			log_ike_sa(out, ike_sa, all);
			ike_printed = TRUE;
		}

		while (children->iterate(children, (void**)&child_sa))
		{
			if (name == NULL || streq(name, child_sa->get_name(child_sa)))
			{
				if (!ike_printed)
				{
					log_ike_sa(out, ike_sa, all);
					ike_printed = TRUE;
				}
				log_child_sa(out, child_sa, all);
			}	
		}
		children->destroy(children);
	}
	iterator->destroy(iterator);
}

/**
 * list all X.509 certificates matching the flags
 */
static void stroke_list_certs(char *label, x509_flag_t flags, bool utc, FILE *out)
{
	bool first = TRUE;
	time_t now = time(NULL);
	certificate_t *cert;
	enumerator_t *enumerator;
	
	enumerator = charon->credentials->create_cert_enumerator(
						charon->credentials, CERT_X509, KEY_ANY, NULL, FALSE);
	while (enumerator->enumerate(enumerator, (void**)&cert))
	{
		x509_t *x509 = (x509_t*)cert;
		x509_flag_t x509_flags = x509->get_flags(x509);

		/* list only if flag is set, or flags == 0 (ignoring self-signed) */
		if ((x509_flags & flags) || (flags == (x509_flags & ~X509_SELF_SIGNED)))
		{
			enumerator_t *enumerator;
			identification_t *altName;
			bool first_altName = TRUE;
			chunk_t serial = x509->get_serial(x509);
			identification_t *authkey = x509->get_authKeyIdentifier(x509);
			time_t notBefore, notAfter;
			public_key_t *public = cert->get_public_key(cert);

			if (first)
			{
				fprintf(out, "\n");
				fprintf(out, "List of %s:\n", label);
				first = FALSE;
			}
			fprintf(out, "\n");

			/* list subjectAltNames */
			enumerator = x509->create_subjectAltName_enumerator(x509);
			while (enumerator->enumerate(enumerator, (void**)&altName))
			{
				if (first_altName)
				{
					fprintf(out, "  altNames:  ");
					first_altName = FALSE;
				}
				else
				{
					fprintf(out, ", ");
				}
				fprintf(out, "%D", altName);
			}
			if (!first_altName)
			{
				fprintf(out, "\n");
			}
			enumerator->destroy(enumerator);

			fprintf(out, "  subject:  %D\n", cert->get_subject(cert));
			fprintf(out, "  issuer:   %D\n", cert->get_issuer(cert));
			fprintf(out, "  serial:    %#B\n", &serial);

			/* list validity */
			cert->get_validity(cert, &now, &notBefore, &notAfter);
			fprintf(out, "  validity:  not before %#T, ", &notBefore, utc);
			if (now < notBefore)
			{
				fprintf(out, "not valid yet (valid in %#V)\n", &now, &notBefore);
			}
			else
			{
				fprintf(out, "ok\n");
			}
			fprintf(out, "             not after  %#T, ", &notAfter, utc);
			if (now > notAfter)
			{
				fprintf(out, "expired (%#V ago)\n", &now, &notAfter);
			}
			else
			{
				fprintf(out, "ok");
				if (now > notAfter - CERT_WARNING_INTERVAL * 60 * 60 * 24)
				{
					fprintf(out, " (expires in %#V)", &now, &notAfter);
				}
				fprintf(out, " \n");
			}
	
			/* list public key information */
			if (public)
			{
				private_key_t *private = NULL;
				identification_t *id, *keyid;
			
				id    = public->get_id(public, ID_PUBKEY_SHA1);
				keyid = public->get_id(public, ID_PUBKEY_INFO_SHA1);

				private = charon->credentials->get_private(
									charon->credentials, 
									public->get_type(public), keyid, NULL);
				fprintf(out, "  pubkey:    %N %d bits%s\n",
						key_type_names, public->get_type(public),
						public->get_keysize(public) * 8,
						private ? ", has private key" : "");
				fprintf(out, "  keyid:     %D\n", keyid);
				fprintf(out, "  subjkey:   %D\n", id);
				DESTROY_IF(private);
				public->destroy(public);
			}
	
			/* list optional authorityKeyIdentifier */
			if (authkey)
			{
				fprintf(out, "  authkey:   %D\n", authkey);
			}
		}
	}
	enumerator->destroy(enumerator);
}

/**
 * list all X.509 CRLs
 */
static void stroke_list_crls(bool utc, FILE *out)
{
	bool first = TRUE;
	time_t thisUpdate, nextUpdate, now = time(NULL);
	certificate_t *cert;
	enumerator_t *enumerator;
	
	enumerator = charon->credentials->create_cert_enumerator(
						charon->credentials, CERT_X509_CRL, KEY_ANY, NULL, FALSE);
	while (enumerator->enumerate(enumerator, (void**)&cert))
	{
		crl_t *crl = (crl_t*)cert;
		chunk_t serial  = crl->get_serial(crl);
		identification_t *authkey = crl->get_authKeyIdentifier(crl);

		if (first)
		{
			fprintf(out, "\n");
			fprintf(out, "List of X.509 CRLs:\n");
			first = FALSE;
		}
		fprintf(out, "\n");

		fprintf(out, "  issuer:   %D\n", cert->get_issuer(cert));

		/* list optional crlNumber */
		if (serial.ptr)
		{
			fprintf(out, "  serial:    %#B\n", &serial);
		}

		/* count the number of revoked certificates */
		{
			int count = 0;
			enumerator_t *enumerator = crl->create_enumerator(crl);

			while (enumerator->enumerate(enumerator, NULL, NULL, NULL))
			{
				count++;
			}
			fprintf(out, "  revoked:   %d certificate%s\n", count,
							(count == 1)? "" : "s");
			enumerator->destroy(enumerator);
		}

		/* list validity */
		cert->get_validity(cert, &now, &thisUpdate, &nextUpdate);
		fprintf(out, "  updates:   this %#T\n",  &thisUpdate, utc);
		fprintf(out, "             next %#T, ", &nextUpdate, utc);
		if (now > nextUpdate)
		{
			fprintf(out, "expired (%#V ago)\n", &now, &nextUpdate);
		}
		else
		{
			fprintf(out, "ok");
			if (now > nextUpdate - CRL_WARNING_INTERVAL * 60 * 60 * 24)
			{
				fprintf(out, " (expires in %#V)", &now, &nextUpdate);
			}
			fprintf(out, " \n");
		}

		/* list optional authorityKeyIdentifier */
		if (authkey)
		{
			fprintf(out, "  authkey:   %D\n", authkey);
		}
	}
	enumerator->destroy(enumerator);
}

/**
 * Implementation of stroke_list_t.list.
 */
static void list(private_stroke_list_t *this, stroke_msg_t *msg, FILE *out)
{
	if (msg->list.flags & LIST_CERTS)
	{
		stroke_list_certs("X.509 End Entity Certificates",
						  0, msg->list.utc, out);
	}
	if (msg->list.flags & LIST_CACERTS)
	{
		stroke_list_certs("X.509 CA Certificates",
						  X509_CA, msg->list.utc, out);
	}
	if (msg->list.flags & LIST_OCSPCERTS)
	{
		stroke_list_certs("X.509 OCSP Signer Certificates",
						  X509_OCSP_SIGNER, msg->list.utc, out);
	}
	if (msg->list.flags & LIST_AACERTS)
	{
		stroke_list_certs("X.509 AA Certificates",
						  X509_AA, msg->list.utc, out);
	}
	if (msg->list.flags & LIST_ACERTS)
	{

	}
	if (msg->list.flags & LIST_CRLS)
	{
		stroke_list_crls(msg->list.utc, out);
	}
	if (msg->list.flags & LIST_OCSP)
	{

	}
}

/**
 * Implementation of stroke_list_t.destroy
 */
static void destroy(private_stroke_list_t *this)
{
	free(this);
}

/*
 * see header file
 */
stroke_list_t *stroke_list_create()
{
	private_stroke_list_t *this = malloc_thing(private_stroke_list_t);
	
	this->public.list = (void(*)(stroke_list_t*, stroke_msg_t *msg, FILE *out))list;
	this->public.status = (void(*)(stroke_list_t*, stroke_msg_t *msg, FILE *out,bool))status;
	this->public.destroy = (void(*)(stroke_list_t*))destroy;
	
	this->uptime = time(NULL);
	
	return &this->public;
}

