/* -*- c-basic-offset: 8 -*-
   rdesktop: A Remote Desktop Protocol client.
   CredSSP layer and Kerberos support.
   Copyright 2012-2017 Henrik Andersson <hean01@cendio.se> for Cendio AB

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <gssapi/gssapi.h>
#include "rdesktop.h"

extern RD_BOOL g_use_password_as_pin;

extern char *g_sc_csp_name;
extern char *g_sc_reader_name;
extern char *g_sc_card_name;
extern char *g_sc_container_name;

static gss_OID_desc _gss_spnego_krb5_mechanism_oid_desc =
	{ 9, (void *) "\x2a\x86\x48\x86\xf7\x12\x01\x02\x02" };

static STREAM
ber_wrap_hdr_data(int tagval, STREAM in)
{
	STREAM out;
	int size = s_length(in) + 16;

	out = s_alloc(size);
	ber_out_header(out, tagval, s_length(in));
	out_stream(out, in);
	s_mark_end(out);

	return out;
}


static void
cssp_gss_report_error(OM_uint32 code, char *str, OM_uint32 major_status, OM_uint32 minor_status)
{
	OM_uint32 msgctx = 0, ms;
	gss_buffer_desc status_string;

	logger(Core, Debug, "GSS error [%d:%d:%d]: %s", (major_status & 0xff000000) >> 24,	// Calling error
	       (major_status & 0xff0000) >> 16,	// Routine error
	       major_status & 0xffff,	// Supplementary info bits
	       str);

	do
	{
		ms = gss_display_status(&minor_status, major_status,
					code, GSS_C_NULL_OID, &msgctx, &status_string);
		if (ms != GSS_S_COMPLETE)
			continue;

		logger(Core, Debug, " - %s", status_string.value);

	}
	while (ms == GSS_S_COMPLETE && msgctx);

}


static RD_BOOL
cssp_gss_mech_available(gss_OID mech)
{
	int mech_found;
	OM_uint32 major_status, minor_status;
	gss_OID_set mech_set;

	mech_found = 0;

	if (mech == GSS_C_NO_OID)
		return True;

	major_status = gss_indicate_mechs(&minor_status, &mech_set);
	if (!mech_set)
		return False;

	if (GSS_ERROR(major_status))
	{
		cssp_gss_report_error(GSS_C_GSS_CODE, "Failed to get available mechs on system",
				      major_status, minor_status);
		return False;
	}

	gss_test_oid_set_member(&minor_status, mech, mech_set, &mech_found);

	if (GSS_ERROR(major_status))
	{
		cssp_gss_report_error(GSS_C_GSS_CODE, "Failed to match mechanism in set",
				      major_status, minor_status);
		return False;
	}

	if (!mech_found)
		return False;

	return True;
}

static RD_BOOL
cssp_gss_get_service_name(char *server, gss_name_t * name)
{
	gss_buffer_desc output;
	OM_uint32 major_status, minor_status;

	const char service_name[] = "TERMSRV";

	gss_OID type = (gss_OID) GSS_C_NT_HOSTBASED_SERVICE;
	int size = (strlen(service_name) + 1 + strlen(server) + 1);

	output.value = malloc(size);
	snprintf(output.value, size, "%s@%s", service_name, server);
	output.length = strlen(output.value) + 1;

	major_status = gss_import_name(&minor_status, &output, type, name);

	if (GSS_ERROR(major_status))
	{
		cssp_gss_report_error(GSS_C_GSS_CODE, "Failed to create service principal name",
				      major_status, minor_status);
		return False;
	}

	gss_release_buffer(&minor_status, &output);

	return True;

}

static STREAM
cssp_gss_wrap(gss_ctx_id_t ctx, STREAM in)
{
	int conf_state;
	OM_uint32 major_status;
	OM_uint32 minor_status;
	gss_buffer_desc inbuf, outbuf;
	STREAM out;

	s_seek(in, 0);
	inbuf.length = s_length(in);
	in_uint8p(in, inbuf.value, s_length(in));
	s_seek(in, 0);

	major_status = gss_wrap(&minor_status, ctx, True,
				GSS_C_QOP_DEFAULT, &inbuf, &conf_state, &outbuf);

	if (major_status != GSS_S_COMPLETE)
	{
		cssp_gss_report_error(GSS_C_GSS_CODE, "Failed to encrypt and sign message",
				      major_status, minor_status);
		return NULL;
	}

	if (!conf_state)
	{
		logger(Core, Error,
		       "cssp_gss_wrap(), GSS Confidentiality failed, no encryption of message performed.");
		return NULL;
	}

	// write enc data to out stream
	out = s_alloc(outbuf.length);
	out_uint8a(out, outbuf.value, outbuf.length);
	s_mark_end(out);
	s_seek(out, 0);

	gss_release_buffer(&minor_status, &outbuf);

	return out;
}

static STREAM
cssp_gss_unwrap(gss_ctx_id_t ctx, STREAM in)
{
	OM_uint32 major_status;
	OM_uint32 minor_status;
	gss_qop_t qop_state;
	gss_buffer_desc inbuf, outbuf;
	int conf_state;
	STREAM out;

	s_seek(in, 0);
	inbuf.length = s_length(in);
	in_uint8p(in, inbuf.value, s_length(in));
	s_seek(in, 0);

	major_status = gss_unwrap(&minor_status, ctx, &inbuf, &outbuf, &conf_state, &qop_state);

	if (major_status != GSS_S_COMPLETE)
	{
		cssp_gss_report_error(GSS_C_GSS_CODE, "Failed to decrypt message",
				      major_status, minor_status);
		return NULL;
	}

	out = s_alloc(outbuf.length);
	out_uint8a(out, outbuf.value, outbuf.length);
	s_mark_end(out);
	s_seek(out, 0);

	gss_release_buffer(&minor_status, &outbuf);

	return out;
}


static STREAM
cssp_encode_tspasswordcreds(char *username, char *password, char *domain)
{
	STREAM out, h1, h2;
	struct stream tmp = { 0 };
	struct stream message = { 0 };

	memset(&tmp, 0, sizeof(tmp));
	memset(&message, 0, sizeof(message));

	s_realloc(&tmp, 512 * 4);

	// domainName [0]
	s_reset(&tmp);
	out_utf16s(&tmp, domain);
	s_mark_end(&tmp);
	h2 = ber_wrap_hdr_data(BER_TAG_OCTET_STRING, &tmp);
	h1 = ber_wrap_hdr_data(BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 0, h2);
	s_realloc(&message, s_length(&message) + s_length(h1));
	out_stream(&message, h1);
	s_mark_end(&message);
	s_free(h2);
	s_free(h1);

	// userName [1]
	s_reset(&tmp);
	out_utf16s(&tmp, username);
	s_mark_end(&tmp);

	h2 = ber_wrap_hdr_data(BER_TAG_OCTET_STRING, &tmp);
	h1 = ber_wrap_hdr_data(BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 1, h2);
	s_realloc(&message, s_length(&message) + s_length(h1));
	out_stream(&message, h1);
	s_mark_end(&message);
	s_free(h2);
	s_free(h1);

	// password [2]
	s_reset(&tmp);
	out_utf16s(&tmp, password);
	s_mark_end(&tmp);
	h2 = ber_wrap_hdr_data(BER_TAG_OCTET_STRING, &tmp);
	h1 = ber_wrap_hdr_data(BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 2, h2);
	s_realloc(&message, s_length(&message) + s_length(h1));
	out_stream(&message, h1);
	s_mark_end(&message);
	s_free(h2);
	s_free(h1);

	// build message
	out = ber_wrap_hdr_data(BER_TAG_SEQUENCE | BER_TAG_CONSTRUCTED, &message);

	// cleanup
	xfree(tmp.data);
	xfree(message.data);
	return out;
}

/* KeySpecs from wincrypt.h */
#define AT_KEYEXCHANGE 1
#define AT_SIGNATURE   2

static STREAM
cssp_encode_tscspdatadetail(unsigned char keyspec, char *card, char *reader, char *container,
			    char *csp)
{
	STREAM out;
	STREAM h1, h2;
	struct stream tmp = { 0 };
	struct stream message = { 0 };

	s_realloc(&tmp, 512 * 4);

	// keySpec [0]
	s_reset(&tmp);
	out_uint8(&tmp, keyspec);
	s_mark_end(&tmp);
	h2 = ber_wrap_hdr_data(BER_TAG_INTEGER, &tmp);
	h1 = ber_wrap_hdr_data(BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 0, h2);
	s_realloc(&message, s_length(&message) + s_length(h1));
	out_stream(&message, h1);
	s_mark_end(&message);
	s_free(h2);
	s_free(h1);

	// cardName [1]
	if (card)
	{
		s_reset(&tmp);
		out_utf16s(&tmp, card);
		s_mark_end(&tmp);
		h2 = ber_wrap_hdr_data(BER_TAG_OCTET_STRING, &tmp);
		h1 = ber_wrap_hdr_data(BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 1, h2);
		s_realloc(&message, s_length(&message) + s_length(h1));
		out_stream(&message, h1);
		s_mark_end(&message);
		s_free(h2);
		s_free(h1);
	}

	// readerName [2]
	if (reader)
	{
		s_reset(&tmp);
		out_utf16s(&tmp, reader);
		s_mark_end(&tmp);
		h2 = ber_wrap_hdr_data(BER_TAG_OCTET_STRING, &tmp);
		h1 = ber_wrap_hdr_data(BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 2, h2);
		s_realloc(&message, s_length(&message) + s_length(h1));
		out_stream(&message, h1);
		s_mark_end(&message);
		s_free(h2);
		s_free(h1);
	}

	// containerName [3]
	if (container)
	{
		s_reset(&tmp);
		out_utf16s(&tmp, container);
		s_mark_end(&tmp);
		h2 = ber_wrap_hdr_data(BER_TAG_OCTET_STRING, &tmp);
		h1 = ber_wrap_hdr_data(BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 3, h2);
		s_realloc(&message, s_length(&message) + s_length(h1));
		out_stream(&message, h1);
		s_mark_end(&message);
		s_free(h2);
		s_free(h1);
	}

	// cspName [4]
	if (csp)
	{
		s_reset(&tmp);
		out_utf16s(&tmp, csp);
		s_mark_end(&tmp);
		h2 = ber_wrap_hdr_data(BER_TAG_OCTET_STRING, &tmp);
		h1 = ber_wrap_hdr_data(BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 4, h2);
		s_realloc(&message, s_length(&message) + s_length(h1));
		out_stream(&message, h1);
		s_mark_end(&message);
		s_free(h2);
		s_free(h1);
	}

	s_mark_end(&message);

	// build message
	out = ber_wrap_hdr_data(BER_TAG_SEQUENCE | BER_TAG_CONSTRUCTED, &message);

	// cleanup
	free(tmp.data);
	free(message.data);
	return out;
}

static STREAM
cssp_encode_tssmartcardcreds(char *username, char *password, char *domain)
{
	STREAM out, h1, h2;
	struct stream tmp = { 0 };
	struct stream message = { 0 };

	s_realloc(&tmp, 512 * 4);

	// pin [0]
	s_reset(&tmp);
	out_utf16s(&tmp, password);
	s_mark_end(&tmp);
	h2 = ber_wrap_hdr_data(BER_TAG_OCTET_STRING, &tmp);
	h1 = ber_wrap_hdr_data(BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 0, h2);
	s_realloc(&message, s_length(&message) + s_length(h1));
	out_stream(&message, h1);
	s_mark_end(&message);
	s_free(h2);
	s_free(h1);

	// cspData [1]
	h2 = cssp_encode_tscspdatadetail(AT_KEYEXCHANGE, g_sc_card_name, g_sc_reader_name,
					 g_sc_container_name, g_sc_csp_name);
	h1 = ber_wrap_hdr_data(BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 1, h2);
	s_realloc(&message, s_length(&message) + s_length(h1));
	out_stream(&message, h1);
	s_mark_end(&message);
	s_free(h2);
	s_free(h1);

	// userHint [2]
	if (username && strlen(username))
	{
		s_reset(&tmp);
		out_utf16s(&tmp, username);
		s_mark_end(&tmp);
		h2 = ber_wrap_hdr_data(BER_TAG_OCTET_STRING, &tmp);
		h1 = ber_wrap_hdr_data(BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 2, h2);
		s_realloc(&message, s_length(&message) + s_length(h1));
		out_stream(&message, h1);
		s_mark_end(&message);
		s_free(h2);
		s_free(h1);
	}

	// domainHint [3]
	if (domain && strlen(domain))
	{
		s_reset(&tmp);
		out_utf16s(&tmp, domain);
		s_mark_end(&tmp);
		h2 = ber_wrap_hdr_data(BER_TAG_OCTET_STRING, &tmp);
		h1 = ber_wrap_hdr_data(BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 3, h2);
		s_realloc(&message, s_length(&message) + s_length(h1));
		out_stream(&message, h1);
		s_mark_end(&message);
		s_free(h2);
		s_free(h1);
	}

	s_mark_end(&message);

	// build message
	out = ber_wrap_hdr_data(BER_TAG_SEQUENCE | BER_TAG_CONSTRUCTED, &message);

	// cleanup
	free(tmp.data);
	free(message.data);
	return out;
}

STREAM
cssp_encode_tscredentials(char *username, char *password, char *domain)
{
	STREAM out;
	STREAM h1, h2, h3;
	struct stream tmp = { 0 };
	struct stream message = { 0 };

	// credType [0]
	s_realloc(&tmp, sizeof(uint8));
	s_reset(&tmp);
	if (g_use_password_as_pin == False)
	{
		out_uint8(&tmp, 1);	// TSPasswordCreds
	}
	else
	{
		out_uint8(&tmp, 2);	// TSSmartCardCreds
	}

	s_mark_end(&tmp);
	h2 = ber_wrap_hdr_data(BER_TAG_INTEGER, &tmp);
	h1 = ber_wrap_hdr_data(BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 0, h2);
	s_realloc(&message, s_length(&message) + s_length(h1));
	out_stream(&message, h1);
	s_mark_end(&message);
	s_free(h2);
	s_free(h1);

	// credentials [1]
	if (g_use_password_as_pin == False)
	{
		h3 = cssp_encode_tspasswordcreds(username, password, domain);
	}
	else
	{
		h3 = cssp_encode_tssmartcardcreds(username, password, domain);
	}

	h2 = ber_wrap_hdr_data(BER_TAG_OCTET_STRING, h3);
	h1 = ber_wrap_hdr_data(BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 1, h2);
	s_realloc(&message, s_length(&message) + s_length(h1));
	out_stream(&message, h1);
	s_mark_end(&message);
	s_free(h3);
	s_free(h2);
	s_free(h1);

	// Construct ASN.1 message
	out = ber_wrap_hdr_data(BER_TAG_SEQUENCE | BER_TAG_CONSTRUCTED, &message);

	// cleanup
	xfree(message.data);
	xfree(tmp.data);

	return out;
}

RD_BOOL
cssp_send_tsrequest(STREAM token, STREAM auth, STREAM pubkey)
{
	STREAM s;
	STREAM h1, h2, h3, h4, h5;

	struct stream tmp = { 0 };
	struct stream message = { 0 };

	memset(&message, 0, sizeof(message));
	memset(&tmp, 0, sizeof(tmp));

	// version [0]
	s_realloc(&tmp, sizeof(uint8));
	s_reset(&tmp);
	out_uint8(&tmp, 2);
	s_mark_end(&tmp);
	h2 = ber_wrap_hdr_data(BER_TAG_INTEGER, &tmp);
	h1 = ber_wrap_hdr_data(BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 0, h2);
	s_realloc(&message, s_length(&message) + s_length(h1));
	out_stream(&message, h1);
	s_mark_end(&message);
	s_free(h2);
	s_free(h1);

	// negoToken [1]
	if (token && s_length(token))
	{
		h5 = ber_wrap_hdr_data(BER_TAG_OCTET_STRING, token);
		h4 = ber_wrap_hdr_data(BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 0, h5);
		h3 = ber_wrap_hdr_data(BER_TAG_SEQUENCE | BER_TAG_CONSTRUCTED, h4);
		h2 = ber_wrap_hdr_data(BER_TAG_SEQUENCE | BER_TAG_CONSTRUCTED, h3);
		h1 = ber_wrap_hdr_data(BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 1, h2);
		s_realloc(&message, s_length(&message) + s_length(h1));
		out_stream(&message, h1);
		s_mark_end(&message);
		s_free(h5);
		s_free(h4);
		s_free(h3);
		s_free(h2);
		s_free(h1);
	}

	// authInfo [2]
	if (auth && s_length(auth))
	{
		h2 = ber_wrap_hdr_data(BER_TAG_OCTET_STRING, auth);
		h1 = ber_wrap_hdr_data(BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 2, h2);

		s_realloc(&message, s_length(&message) + s_length(h1));
		out_stream(&message, h1);

		s_free(h2);
		s_free(h1);
	}

	// pubKeyAuth [3]
	if (pubkey && s_length(pubkey))
	{
		h2 = ber_wrap_hdr_data(BER_TAG_OCTET_STRING, pubkey);
		h1 = ber_wrap_hdr_data(BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 3, h2);

		s_realloc(&message, s_length(&message) + s_length(h1));
		out_stream(&message, h1);
		s_mark_end(&message);
		s_free(h2);
		s_free(h1);
	}
	s_mark_end(&message);

	// Construct ASN.1 Message
	// Todo: can h1 be send directly instead of tcp_init() approach
	h1 = ber_wrap_hdr_data(BER_TAG_SEQUENCE | BER_TAG_CONSTRUCTED, &message);
	s = tcp_init(s_length(h1));
	out_stream(s, h1);
	s_mark_end(s);
	s_free(h1);

	tcp_send(s);
	s_free(s);

	// cleanup
	xfree(message.data);
	xfree(tmp.data);

	return True;
}


STREAM
cssp_read_tsrequest(RD_BOOL pubkey)
{
	STREAM s, out;
	int length;
	int tagval;
	struct stream packet;

	s = tcp_recv(NULL, 4);

	if (s == NULL)
		return NULL;

	// get and verify the header
	if (!ber_in_header(s, &tagval, &length) ||
	    tagval != (BER_TAG_SEQUENCE | BER_TAG_CONSTRUCTED))
		return NULL;

	// We've already read 4 bytes, but the header might have been
	// less than that, so we need to adjust the length
	length -= s_remaining(s);

	// receive the remainings of message
	s = tcp_recv(s, length);
	if (s == NULL)
		return NULL;
	packet = *s;

	// version [0]
	if (!ber_in_header(s, &tagval, &length) ||
	    tagval != (BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 0))
		return NULL;

	if (!s_check_rem(s, length))
	{
		 rdp_protocol_error("consume of version from stream would overrun",
				    &packet);
	}
	in_uint8s(s, length);

	// negoToken [1]
	if (!pubkey)
	{
		if (!ber_in_header(s, &tagval, &length)
		    || tagval != (BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 1))
			return NULL;
		if (!ber_in_header(s, &tagval, &length)
		    || tagval != (BER_TAG_SEQUENCE | BER_TAG_CONSTRUCTED))
			return NULL;
		if (!ber_in_header(s, &tagval, &length)
		    || tagval != (BER_TAG_SEQUENCE | BER_TAG_CONSTRUCTED))
			return NULL;
		if (!ber_in_header(s, &tagval, &length)
		    || tagval != (BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 0))
			return NULL;

		if (!ber_in_header(s, &tagval, &length) || tagval != BER_TAG_OCTET_STRING)
			return NULL;

		if (!s_check_rem(s, length))
		{
			rdp_protocol_error("consume of token from stream would overrun",
					   &packet);
		}

		out = s_alloc(length);
		out_uint8stream(out, s, length);
		s_mark_end(out);
		s_seek(out, 0);
	}
	// pubKey [3]
	else
	{
		if (!ber_in_header(s, &tagval, &length)
		    || tagval != (BER_TAG_CTXT_SPECIFIC | BER_TAG_CONSTRUCTED | 3))
			return NULL;

		if (!ber_in_header(s, &tagval, &length) || tagval != BER_TAG_OCTET_STRING)
			return NULL;

		out = s_alloc(length);
		out_uint8stream(out, s, length);
		s_mark_end(out);
		s_seek(out, 0);
	}


	return out;
}

RD_BOOL
cssp_connect(char *server, char *user, char *domain, char *password, STREAM s)
{
	UNUSED(s);
	OM_uint32 actual_time;
	gss_cred_id_t cred;
	gss_buffer_desc input_tok, output_tok;
	gss_name_t target_name;
	OM_uint32 major_status, minor_status;
	int context_established = 0;
	gss_ctx_id_t gss_ctx;
	gss_OID desired_mech = &_gss_spnego_krb5_mechanism_oid_desc;

	STREAM ts_creds;
	STREAM token;
	STREAM pubkey, pubkey_cmp;
	unsigned char *pubkey_data;
	unsigned char *pubkey_cmp_data;
	unsigned char first_byte;

	RD_BOOL ret;
	STREAM blob;

	// Verify that system gss support spnego
	if (!cssp_gss_mech_available(desired_mech))
	{
		logger(Core, Debug,
		       "cssp_connect(), system doesn't have support for desired authentication mechanism");
		return False;
	}

	// Get service name
	if (!cssp_gss_get_service_name(server, &target_name))
	{
		logger(Core, Debug, "cssp_connect(), failed to get target service name");
		return False;
	}

	// Establish TLS connection to server
	if (!tcp_tls_connect())
	{
		logger(Core, Debug, "cssp_connect(), failed to establish TLS connection");
		return False;
	}

	pubkey = tcp_tls_get_server_pubkey();
	if (pubkey == NULL)
		return False;
	pubkey_cmp = NULL;

	// Enter the spnego loop
	OM_uint32 actual_services;
	gss_OID actual_mech;

	gss_ctx = GSS_C_NO_CONTEXT;
	cred = GSS_C_NO_CREDENTIAL;

	token = NULL;
	input_tok.length = 0;
	output_tok.length = 0;
	minor_status = 0;

	int i = 0;

	do
	{
		major_status = gss_init_sec_context(&minor_status,
						    cred,
						    &gss_ctx,
						    target_name,
						    desired_mech,
						    GSS_C_MUTUAL_FLAG | GSS_C_DELEG_FLAG,
						    GSS_C_INDEFINITE,
						    GSS_C_NO_CHANNEL_BINDINGS,
						    &input_tok,
						    &actual_mech,
						    &output_tok, &actual_services, &actual_time);

		// input_tok might have pointed to token's data,
		// but it's safe to free it now after the call
		s_free(token);
		token = NULL;

		if (GSS_ERROR(major_status))
		{
			if (i == 0)
				logger(Core, Notice,
				       "Failed to initialize NLA, do you have correct Kerberos TGT initialized ?");
			else
				logger(Core, Error, "cssp_connect(), negotiation failed");

			cssp_gss_report_error(GSS_C_GSS_CODE, "cssp_connect(), negotiation failed.",
					      major_status, minor_status);
			goto bail_out;
		}

		// validate required services
		if (!(actual_services & GSS_C_CONF_FLAG))
		{
			logger(Core, Error,
			       "cssp_connect(), confidentiality service required but is not available");
			goto bail_out;
		}

		// Send token to server
		if (output_tok.length != 0)
		{
			token = s_alloc(output_tok.length);
			out_uint8a(token, output_tok.value, output_tok.length);
			s_mark_end(token);

			ret = cssp_send_tsrequest(token, NULL, NULL);

			s_free(token);
			token = NULL;
			(void) gss_release_buffer(&minor_status, &output_tok);

			if (!ret)
				goto bail_out;
		}

		// Read token from server
		if (major_status & GSS_S_CONTINUE_NEEDED)
		{
			token = cssp_read_tsrequest(False);
			if (token == NULL)
				goto bail_out;

			input_tok.length = s_length(token);
			in_uint8p(token, input_tok.value, input_tok.length);
		}
		else
		{
			// Send encrypted pubkey for verification to server
			context_established = 1;

			blob = cssp_gss_wrap(gss_ctx, pubkey);
			if (blob == NULL)
				goto bail_out;

			ret = cssp_send_tsrequest(NULL, NULL, blob);

			s_free(blob);

			if (!ret)
				goto bail_out;

			context_established = 1;
		}

		i++;

	}
	while (!context_established);

	s_free(token);

	// read tsrequest response and decrypt for public key validation
	blob = cssp_read_tsrequest(True);
	if (blob == NULL)
		goto bail_out;

	pubkey_cmp = cssp_gss_unwrap(gss_ctx, blob);
	s_free(blob);
	if (pubkey_cmp == NULL)
		goto bail_out;

	// the first byte gets 1 added before being sent by the server
	// in order to protect against replays of the data sent earlier
	// by the client
	in_uint8(pubkey_cmp, first_byte);
	s_seek(pubkey_cmp, 0);
	out_uint8(pubkey_cmp, first_byte - 1);
	s_seek(pubkey_cmp, 0);

	// validate public key
	in_uint8p(pubkey, pubkey_data, s_length(pubkey));
	in_uint8p(pubkey_cmp, pubkey_cmp_data, s_length(pubkey_cmp));
	if ((s_length(pubkey) != s_length(pubkey_cmp)) ||
	    (memcmp(pubkey_data, pubkey_cmp_data, s_length(pubkey)) != 0))
	{
		logger(Core, Error,
		       "cssp_connect(), public key mismatch, cannot guarantee integrity of server connection");
		goto bail_out;
	}

	s_free(pubkey);
	s_free(pubkey_cmp);

	// Send TSCredentials
	ts_creds = cssp_encode_tscredentials(user, password, domain);

	blob = cssp_gss_wrap(gss_ctx, ts_creds);

	s_free(ts_creds);

	if (blob == NULL)
		goto bail_out;

	ret = cssp_send_tsrequest(NULL, blob, NULL);

	s_free(blob);

	if (!ret)
		goto bail_out;

	return True;

      bail_out:
	s_free(token);
	s_free(pubkey);
	s_free(pubkey_cmp);
	return False;
}



const uint32 NtLmNegotiate = 0x01;
const uint32 NtLmChallenge = 0x02;
const uint32 NtLmAuthenticate = 0x03;

// See 2.2.2.5 NEGOTIATE
const uint32 NTLMSSP_NEGOTIATE_56 = 0x80000000;
const uint32 NTLMSSP_NEGOTIATE_KEY_EXCH = 0x40000000;
const uint32 NTLMSSP_NEGOTIATE_128 = 0x20000000;
const uint32 r1 = 0;
const uint32 r2 = 0;
const uint32 r3 = 0;
const uint32 NTLMSSP_NEGOTIATE_VERSION = 0x02000000;
const uint32 r4 = 0;

const uint32 NTLMSSP_NEGOTIATE_TARGET_INFO = 0x00800000;
const uint32 NTLMSSP_REQUEST_NON_NT_SESSION_KEY = 0x00400000;
const uint32 r5 = 0;
const uint32 NTLMSSP_NEGOTIATE_IDENTIFY = 0x00100000;
const uint32 NTLMSSP_NEGOTIATE_EXTENDED_SESSIONSECURITY = 0x00080000;
const uint32 r6 = 0;
const uint32 NTLMSSP_TARGET_TYPE_SERVER = 0x00020000;
const uint32 NTLMSSP_TARGET_TYPE_DOMAIN = 0x00010000;

const uint32 NTLMSSP_NEGOTIATE_ALWAYS_SIGN = 0x00008000;
const uint32 r7 = 0;
const uint32 NTLMSSP_NEGOTIATE_OEM_WORKSTATION_SUPPLIED	= 0x00002000;
const uint32 NTLMSSP_NEGOTIATE_OEM_DOMAIN_SUPPLIED = 0x00001000;
const uint32 J = 0x00000800;
const uint32 r8 = 0;
const uint32 NTLMSSP_NEGOTIATE_NTLM = 0x00000200;
const uint32 r9 = 0;

const uint32 NTLMSSP_NEGOTIATE_LM_KEY = 0x00000080;
const uint32 NTLMSSP_NEGOTIATE_DATAGRAM = 0x00000040;
const uint32 NTLMSSP_NEGOTIATE_SEAL = 0x00000020;
const uint32 NTLMSSP_NEGOTIATE_SIGN = 0x00000010;
const uint32 r10 = 0;
const uint32 NTLMSSP_REQUEST_TARGET = 0x00000004;
const uint32 NTLM_NEGOTIATE_OEM = 0x00000002;
const uint32 NTLMSSP_NEGOTIATE_UNICODE = 0x00000001;

STREAM ntlm_create_negotiate_message(char* domain, char* workstation, bool connectionLess)
{
	STREAM out;
	int length;
	
	// first calculate the length
	length += 8;	// Signature
	length += 4;	// MessageType
	length += 4;	// NegotiateFlags
	length += 8;	// DomainNameFields
	length += 8;	// WorkstationFields
	
	uint32 negotiateFlag = ntlm_create_negotiate_flags_negotiate_message();
	
	// TODO: might need some character encoding stuff here. 2 bytes per char maybe?
	int domainNameLen = 0;
	if (domain)
	{
		negotiateFlag |= NTLMSSP_NEGOTIATE_OEM_DOMAIN_SUPPLIED;
		domainNameLen = strlen(domain);
	}
	
	int workstationNameLen = 0;
	if (workstationName)
	{
		negotiateFlag |= NTLMSSP_NEGOTIATE_OEM_WORKSTATION_SUPPLIED;
		workstationNameLen = strlen(workstation);
	}
	
	length += domainNameLen;
	length += workstationNameLen;
	
	if (negotiateFlag & NTLMSSP_NEGOTIATE_VERSION)
	{
		length += 8;
	}
	
	int constantPartLength = length;
	
	// Allocate the stream
	out = s_alloc(length);
	
	// MS-NLMP Section 2.2
	
	// Signature
	out_uint8(out, 'N');
	out_uint8(out, 'T');
	out_uint8(out, 'L');
	out_uint8(out, 'M');
	out_uint8(out, 'S');
	out_uint8(out, 'S');
	out_uint8(out, 'P');
	out_uint8(out, '\0');
	
	// MessageType
	out_uint32(out, NtLmNegotiate);
	
	// NegotiateFlags
	if (connectionLess)
	{
		negotiateFlag |= NTLMSSP_NEGOTIATE_DATAGRAM
	}
	
	out_uint32(negotiateFlag);
	
	// DomainNameFields
	out_uint32(out, domainNameLen); // DomainNameLen
	out_uint32(out, domainNameLen); // DomainNameMaxLen
	
	out_uint32(out, constantPartLength); // DomainNameBufferOffset
	
	// WorkstationFields
	out_uint32(out, workstationNameLen); // WorkstationLen
	out_uint32(out, workstationNameLen); // WorkstationMaxLen
	
	out_uint32(out, constantPartLength + domainNameLen); // WorkstationBufferOffset
	
	// Payload
	if (domainNameLen > 0)
	{
		out_utf16s(out, domain); // DomainName
	}
	
	if (workstationNameLen > 0)
	{
		out_utf16s(out, workstation); // WorkstationName
	}
	
	// Version
	if (negotiateFlag & NTLMSSP_NEGOTIATE_VERSION)
	{
		out_uint32(out, 0x00000000);
		out_uint32(out, 0x00000000);
	}
	
	return out;
}

bool AssertNextChar(Stream s, char expectedValue)
{
	char readChar;
	in_uint8(s, readChar);
	if (readChar != expectedValue)
	{
		// TODO: report some kind of error
		return 0;
	}
	
	return 1;
}

bool AssertNextUint32(Stream s, uint32 expectedValue)
{
	char readValue;
	in_uint32(s, readValue);
	if (readChar != expectedValue)
	{
		// TODO: report some kind of error
		return 0;
	}
	
	return 1;
}

STREAM ntlm_reply_to_challenge_message(Stream s, int totalLength, uint32 originalNegotiateFlag)
{
	uint length = 48;
	if (totalLength < length)
	{
		// TODO: report some kind of error
		return NULL;
	}
	
	// Signature
	if (!AssertNextChar(s, 'N')
		|| !AssertNextChar(s, 'T')
		|| !AssertNextChar(s, 'L')
		|| !AssertNextChar(s, 'M')
		|| !AssertNextChar(s, 'S')
		|| !AssertNextChar(s, 'S')
		|| !AssertNextChar(s, 'P')
		|| !AssertNextChar(s, '\0'))
	{
		return NULL;
	}
	
	// MessageType
	if (!AssertNextUint32(s, NtLmChallenge))
	{
		return NULL;
	}
	
	// TargetNameFields
	uint16 TargetNameLen, TargetNameMaxLen;
	in_uint16(s, TargetNameLen);
	in_uint16(s, TargetNameMaxLen);
	uint32 TargetNameBufferOffset;
	in_uint32(s, TargetNameBufferOffset);
	
	if (originalNegotiateFlag & NTLMSSP_REQUEST_TARGET)
	{
		if (TargetNameLen == 0 || TargetNameBufferOffset == 0)
		{
			// TODO: report some kind of error
			return NULL;
		}
		
		if (originalNegotiateFlag & NTLMSSP_NEGOTIATE_UNICODE && (TargetNameBufferOffset % 2 != 0 || TargetNameLen % 2 != 0))
		{
			// TODO: report some kind of error
			return NULL;
		}
	}
	
	// NegotiateFlags
	uint32 negotiateFlag;
	in_uint32(s, negotiateFlag);
	
	// ServerChallenge
	byte ServerChallenge[8];
	ServerChallenge = in_uint8s(s, 8); // TODO: I'm pretty sure that's not how I consume the stream;
	
	// Reserved
	if (!AssertNextUint32(s, 0)
		|| !AssertNextUint32(s, 0))
	{
		// TODO: report some kind of error
		return NULL;
	}
	
	// TargetInfoFields
	uint16 TargetInfoLen, TargetInfoMaxLen;
	in_uint16(s, TargetInfoLen);
	in_uint16(s, TargetInfoMaxLen);
	uint32 TargetInfoBufferOffset;
	in_uint32(s, TargetInfoBufferOffset);
	
	// Version
	for(int i = 0; i < 8; i++)
	{
		uint32 ignore;
		in_uint8(s, ignore);
	}
	
	// Payload
	Stream targetNameStream;
	Stream targetInfoStream;
	
	if (TargetInfoLen > 0 && TargetInfoBufferOffset <= TargetNameBufferOffset)
	{
		uint32 skip = TargetInfoBufferOffset - length;
		length = TargetInfoBufferOffset + TargetInfoLen;
		if (totalLength < length)
		{
			// TODO: report some kind of error
			return NULL;	
		}
		
		for(int i = 0; i < skip; i++)
		{
			uint8 ignore;
			in_uint8(s, ignore);
		}
		
		out_uint8stream(targetInfoStream, s, TargetInfoLen);
	}
	
	if (TargetNameLen > 0)
	{
		uint32 skip = TargetNameBufferOffset - length;
		length = TargetNameBufferOffset + TargetNameLen
		if (totalLength < length)
		{
			// TODO: report some kind of error
			return NULL;	
		}
		
		for(int i = 0; i < skip; i++)
		{
			uint8 ignore;
			in_uint8(s, ignore);
		}
		
		out_uint8stream(targetNameStream, s, TargetNameLen);
	}
	
	if (TargetInfoLen > 0 && TargetInfoBufferOffset > TargetNameBufferOffset)
	{
		uint32 skip = TargetInfoBufferOffset - length;
		length = TargetInfoBufferOffset + TargetInfoLen
		if (totalLength < length)
		{
			// TODO: report some kind of error
			return NULL;	
		}
		
		for(int i = 0; i < skip; i++)
		{
			uint8 ignore;
			in_uint8(s, ignore);
		}
		
		out_uint8stream(targetInfoStream, s, TargetInfoLen);
	}
	
	// TODO: Validate the contents of the AV_PAIRs
	// 2.2.2.1 - Some are required, the sequence must end with MsvAvEOL
	
	// TODO: If present, validate the name in targetNameStream
	// If the target is in a domain, it should be the domain name.
	// Otherwise, it should be the server name.
	
	Stream out;
	
	uint32 out_length = 0;
	out_length += 8; // Signature
	out_length += 4; // MessageType
	out_length += 8; // LmChallengeResponseFields
	out_length += 8; // NtChallengeResponseFields
	out_length += 8; // DomainNameFields
	out_length += 8; // UserNameFields
	out_length += 8; // WorkstationFields
	out_length += 8; // EncryptedRandomSessionKeyFields
	out_length += 4; // NegotiateFlags
	out_length += 8; // Version
	out_length += 16; // MIC
	
	// TODO: calculate the length of Payload
	// 
	uint32 payload;
	out_length += payload;
	
	// TODO: build the response
	
	
	
	
	
	return out;
}
	
uint ntlm_create_negotiate_flags_negotiate_message()
{
	uint32 result = 
		// chosing to omit 56 bit encryption
		NTLMSSP_NEGOTIATE_KEY_EXCH 
		| NTLMSSP_NEGOTIATE_128
		// NTLMSSP_NEGOTIATE_VERSION set separately if at all

		//| NTLMSSP_NEGOTIATE_IDENTIFY // TODO: has to do with GSS_C_IDENTIFY_FLAG, which may come from credssp

		// NTLMSSP_NEGOTIATE_OEM_WORKSTATION_SUPPLIED set separately
		// NTLMSSP_NEGOTIATE_OEM_DOMAIN_SUPPLIED set separately

		| NTLMSSP_NEGOTIATE_LM_KEY // Use NTLMv2 only
		// NTLMSSP_NEGOTIATE_DATAGRAM set separately
		| NTLMSSP_NEGOTIATE_SEAL
		| NTLMSSP_NEGOTIATE_SIGN
		| NTLMSSP_REQUEST_TARGET
		| NTLM_NEGOTIATE_OEM;
}
