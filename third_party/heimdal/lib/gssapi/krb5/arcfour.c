/*
 * Copyright (c) 2003 - 2006 Kungliga Tekniska Högskolan
 * (Royal Institute of Technology, Stockholm, Sweden).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "gsskrb5_locl.h"

/*
 * Implements draft-brezak-win2k-krb-rc4-hmac-04.txt
 *
 * The arcfour message have the following formats:
 *
 * MIC token
 * 	TOK_ID[2] = 01 01
 *	SGN_ALG[2] = 11 00
 *	Filler[4]
 *	SND_SEQ[8]
 *	SGN_CKSUM[8]
 *
 * WRAP token
 *	TOK_ID[2] = 02 01
 *	SGN_ALG[2];
 *	SEAL_ALG[2]
 *	Filler[2]
 *	SND_SEQ[2]
 *	SGN_CKSUM[8]
 *	Confounder[8]
 */

/*
 * WRAP in DCE-style have a fixed size header, the oid and length over
 * the WRAP header is a total of
 * GSS_ARCFOUR_WRAP_TOKEN_DCE_DER_HEADER_SIZE +
 * GSS_ARCFOUR_WRAP_TOKEN_SIZE byte (ie total of 45 bytes overhead,
 * remember the 2 bytes from APPL [0] SEQ).
 */

#define GSS_ARCFOUR_WRAP_TOKEN_SIZE 32
#define GSS_ARCFOUR_WRAP_TOKEN_DCE_DER_HEADER_SIZE 13


static krb5_error_code
arcfour_mic_key(krb5_context context, krb5_keyblock *key,
		const void *cksum_data, size_t cksum_size,
		void *key6_data, size_t key6_size)
{
    krb5_error_code ret;

    Checksum cksum_k5;
    krb5_keyblock key5;
    char k5_data[16];

    Checksum cksum_k6;

    char T[4];

    memset(T, 0, 4);
    cksum_k5.checksum.data = k5_data;
    cksum_k5.checksum.length = sizeof(k5_data);

    if (key->keytype == KRB5_ENCTYPE_ARCFOUR_HMAC_MD5_56) {
	char L40[14] = "fortybits";

	memcpy(L40 + 10, T, sizeof(T));
	ret = krb5_hmac(context, CKSUMTYPE_RSA_MD5,
			L40, 14, 0, key, &cksum_k5);
	memset(&k5_data[7], 0xAB, 9);
    } else {
	ret = krb5_hmac(context, CKSUMTYPE_RSA_MD5,
			T, 4, 0, key, &cksum_k5);
    }
    if (ret)
	return ret;

    key5.keytype = KRB5_ENCTYPE_ARCFOUR_HMAC_MD5;
    key5.keyvalue = cksum_k5.checksum;

    cksum_k6.checksum.data = key6_data;
    cksum_k6.checksum.length = key6_size;

    return krb5_hmac(context, CKSUMTYPE_RSA_MD5,
		     cksum_data, cksum_size, 0, &key5, &cksum_k6);
}


static krb5_error_code
arcfour_mic_cksum_iov(krb5_context context,
		      krb5_keyblock *key, unsigned usage,
		      u_char *sgn_cksum, size_t sgn_cksum_sz,
		      const u_char *v1, size_t l1,
		      const void *v2, size_t l2,
		      const gss_iov_buffer_desc *iov,
		      int iov_count,
		      const gss_iov_buffer_desc *padding)
{
    Checksum CKSUM;
    u_char *ptr;
    size_t len;
    size_t ofs = 0;
    int i;
    krb5_crypto crypto;
    krb5_error_code ret;

    assert(sgn_cksum_sz == 8);

    len = l1 + l2;

    for (i=0; i < iov_count; i++) {
	switch (GSS_IOV_BUFFER_TYPE(iov[i].type)) {
	case GSS_IOV_BUFFER_TYPE_DATA:
	case GSS_IOV_BUFFER_TYPE_SIGN_ONLY:
	    break;
	default:
	    continue;
	}

	len += iov[i].buffer.length;
    }

    if (padding) {
	len += padding->buffer.length;
    }

    ptr = malloc(len);
    if (ptr == NULL)
	return ENOMEM;

    memcpy(ptr + ofs, v1, l1);
    ofs += l1;
    memcpy(ptr + ofs, v2, l2);
    ofs += l2;

    for (i=0; i < iov_count; i++) {
	switch (GSS_IOV_BUFFER_TYPE(iov[i].type)) {
	case GSS_IOV_BUFFER_TYPE_DATA:
	case GSS_IOV_BUFFER_TYPE_SIGN_ONLY:
	    break;
	default:
	    continue;
	}

	if (iov[i].buffer.length > 0) {
	    assert(iov[i].buffer.value != NULL);
	    memcpy(ptr + ofs,
		   iov[i].buffer.value,
		   iov[i].buffer.length);
	    ofs += iov[i].buffer.length;
	}
    }

    if (padding) {
	memcpy(ptr + ofs,
	       padding->buffer.value,
	       padding->buffer.length);
	/* ofs += padding->buffer.length; */
    }

    ret = krb5_crypto_init(context, key, 0, &crypto);
    if (ret) {
	free(ptr);
	return ret;
    }

    ret = krb5_create_checksum(context,
			       crypto,
			       usage,
			       0,
			       ptr, len,
			       &CKSUM);
    memset(ptr, 0, len);
    free(ptr);
    if (ret == 0) {
	memcpy(sgn_cksum, CKSUM.checksum.data, sgn_cksum_sz);
	free_Checksum(&CKSUM);
    }
    krb5_crypto_destroy(context, crypto);

    return ret;
}

static krb5_error_code
arcfour_mic_cksum(krb5_context context,
		  krb5_keyblock *key, unsigned usage,
		  u_char *sgn_cksum, size_t sgn_cksum_sz,
		  const u_char *v1, size_t l1,
		  const void *v2, size_t l2,
		  const void *v3, size_t l3)
{
    gss_iov_buffer_desc iov;

    iov.type = GSS_IOV_BUFFER_TYPE_SIGN_ONLY;
    iov.buffer.value = rk_UNCONST(v3);
    iov.buffer.length = l3;

    return arcfour_mic_cksum_iov(context, key, usage,
				 sgn_cksum, sgn_cksum_sz,
				 v1, l1, v2, l2,
				 &iov, 1, NULL);
}


OM_uint32
_gssapi_get_mic_arcfour(OM_uint32 * minor_status,
			const gsskrb5_ctx context_handle,
			krb5_context context,
			gss_qop_t qop_req,
			const gss_buffer_t message_buffer,
			gss_buffer_t message_token,
			krb5_keyblock *key)
{
    krb5_error_code ret;
    int32_t seq_number;
    size_t len, total_len;
    u_char k6_data[16], *p0, *p;
    EVP_CIPHER_CTX rc4_key;

    _gsskrb5_encap_length (22, &len, &total_len, GSS_KRB5_MECHANISM);

    message_token->length = total_len;
    message_token->value  = malloc (total_len);
    if (message_token->value == NULL) {
	*minor_status = ENOMEM;
	return GSS_S_FAILURE;
    }

    p0 = _gssapi_make_mech_header(message_token->value,
				  len,
				  GSS_KRB5_MECHANISM);
    p = p0;

    *p++ = 0x01; /* TOK_ID */
    *p++ = 0x01;
    *p++ = 0x11; /* SGN_ALG */
    *p++ = 0x00;
    *p++ = 0xff; /* Filler */
    *p++ = 0xff;
    *p++ = 0xff;
    *p++ = 0xff;

    p = NULL;

    ret = arcfour_mic_cksum(context,
			    key, KRB5_KU_USAGE_SIGN,
			    p0 + 16, 8,  /* SGN_CKSUM */
			    p0, 8, /* TOK_ID, SGN_ALG, Filer */
			    message_buffer->value, message_buffer->length,
			    NULL, 0);
    if (ret) {
	_gsskrb5_release_buffer(minor_status, message_token);
	*minor_status = ret;
	return GSS_S_FAILURE;
    }

    ret = arcfour_mic_key(context, key,
			  p0 + 16, 8, /* SGN_CKSUM */
			  k6_data, sizeof(k6_data));
    if (ret) {
	_gsskrb5_release_buffer(minor_status, message_token);
	*minor_status = ret;
	return GSS_S_FAILURE;
    }

    HEIMDAL_MUTEX_lock(&context_handle->ctx_id_mutex);
    krb5_auth_con_getlocalseqnumber (context,
				     context_handle->auth_context,
				     &seq_number);
    p = p0 + 8; /* SND_SEQ */
    _gss_mg_encode_be_uint32(seq_number, p);

    krb5_auth_con_setlocalseqnumber (context,
				     context_handle->auth_context,
				     ++seq_number);
    HEIMDAL_MUTEX_unlock(&context_handle->ctx_id_mutex);

    memset (p + 4, (context_handle->more_flags & LOCAL) ? 0 : 0xff, 4);

    EVP_CIPHER_CTX_init(&rc4_key);
    EVP_CipherInit_ex(&rc4_key, EVP_rc4(), NULL, k6_data, NULL, 1);
    EVP_Cipher(&rc4_key, p, p, 8);
    EVP_CIPHER_CTX_cleanup(&rc4_key);

    memset_s(k6_data, sizeof(k6_data), 0, sizeof(k6_data));

    *minor_status = 0;
    return GSS_S_COMPLETE;
}


OM_uint32
_gssapi_verify_mic_arcfour(OM_uint32 * minor_status,
			   const gsskrb5_ctx context_handle,
			   krb5_context context,
			   const gss_buffer_t message_buffer,
			   const gss_buffer_t token_buffer,
			   gss_qop_t * qop_state,
			   krb5_keyblock *key,
			   const char *type)
{
    krb5_error_code ret;
    uint32_t seq_number;
    OM_uint32 omret;
    u_char SND_SEQ[8], cksum_data[8], *p;
    char k6_data[16];
    int cmp;

    if (qop_state)
	*qop_state = 0;

    p = token_buffer->value;
    omret = _gsskrb5_verify_header (&p,
				       token_buffer->length,
				       type,
				       GSS_KRB5_MECHANISM);
    if (omret)
	return omret;

    if (memcmp(p, "\x11\x00", 2) != 0) /* SGN_ALG = HMAC MD5 ARCFOUR */
	return GSS_S_BAD_SIG;
    p += 2;
    if (memcmp (p, "\xff\xff\xff\xff", 4) != 0)
	return GSS_S_BAD_MIC;
    p += 4;

    ret = arcfour_mic_cksum(context,
			    key, KRB5_KU_USAGE_SIGN,
			    cksum_data, sizeof(cksum_data),
			    p - 8, 8,
			    message_buffer->value, message_buffer->length,
			    NULL, 0);
    if (ret) {
	*minor_status = ret;
	return GSS_S_FAILURE;
    }

    ret = arcfour_mic_key(context, key,
			  cksum_data, sizeof(cksum_data),
			  k6_data, sizeof(k6_data));
    if (ret) {
	*minor_status = ret;
	return GSS_S_FAILURE;
    }

    cmp = (ct_memcmp(cksum_data, p + 8, 8) != 0);
    if (cmp) {
	*minor_status = 0;
	return GSS_S_BAD_MIC;
    }

    {
	EVP_CIPHER_CTX rc4_key;

	EVP_CIPHER_CTX_init(&rc4_key);
	EVP_CipherInit_ex(&rc4_key, EVP_rc4(), NULL, (void *)k6_data, NULL, 0);
	EVP_Cipher(&rc4_key, SND_SEQ, p, 8);
	EVP_CIPHER_CTX_cleanup(&rc4_key);

	memset(k6_data, 0, sizeof(k6_data));
    }

    _gss_mg_decode_be_uint32(SND_SEQ, &seq_number);

    if (context_handle->more_flags & LOCAL)
	cmp = (ct_memcmp(&SND_SEQ[4], "\xff\xff\xff\xff", 4) != 0);
    else
	cmp = (ct_memcmp(&SND_SEQ[4], "\x00\x00\x00\x00", 4) != 0);

    memset_s(SND_SEQ, sizeof(SND_SEQ), 0, sizeof(SND_SEQ));
    if (cmp != 0) {
	*minor_status = 0;
	return GSS_S_BAD_MIC;
    }

    HEIMDAL_MUTEX_lock(&context_handle->ctx_id_mutex);
    omret = _gssapi_msg_order_check(context_handle->order, seq_number);
    HEIMDAL_MUTEX_unlock(&context_handle->ctx_id_mutex);
    if (omret)
	return omret;

    *minor_status = 0;
    return GSS_S_COMPLETE;
}

OM_uint32
_gssapi_wrap_arcfour(OM_uint32 * minor_status,
		     const gsskrb5_ctx context_handle,
		     krb5_context context,
		     int conf_req_flag,
		     gss_qop_t qop_req,
		     const gss_buffer_t input_message_buffer,
		     int * conf_state,
		     gss_buffer_t output_message_buffer,
		     krb5_keyblock *key)
{
    u_char Klocaldata[16], k6_data[16], *p, *p0;
    size_t len, total_len, datalen;
    krb5_keyblock Klocal;
    krb5_error_code ret;
    int32_t seq_number;

    if (conf_state)
	*conf_state = 0;

    datalen = input_message_buffer->length;

    if (IS_DCE_STYLE(context_handle)) {
	len = GSS_ARCFOUR_WRAP_TOKEN_SIZE;
	_gssapi_encap_length(len, &len, &total_len, GSS_KRB5_MECHANISM);
	total_len += datalen;
    } else {
	datalen += 1; /* padding */
	len = datalen + GSS_ARCFOUR_WRAP_TOKEN_SIZE;
	_gssapi_encap_length(len, &len, &total_len, GSS_KRB5_MECHANISM);
    }

    output_message_buffer->length = total_len;
    output_message_buffer->value  = malloc (total_len);
    if (output_message_buffer->value == NULL) {
	*minor_status = ENOMEM;
	return GSS_S_FAILURE;
    }

    p0 = _gssapi_make_mech_header(output_message_buffer->value,
				  len,
				  GSS_KRB5_MECHANISM);
    p = p0;

    *p++ = 0x02; /* TOK_ID */
    *p++ = 0x01;
    *p++ = 0x11; /* SGN_ALG */
    *p++ = 0x00;
    if (conf_req_flag) {
	*p++ = 0x10; /* SEAL_ALG */
	*p++ = 0x00;
    } else {
	*p++ = 0xff; /* SEAL_ALG */
	*p++ = 0xff;
    }
    *p++ = 0xff; /* Filler */
    *p++ = 0xff;

    p = NULL;

    HEIMDAL_MUTEX_lock(&context_handle->ctx_id_mutex);
    krb5_auth_con_getlocalseqnumber (context,
				     context_handle->auth_context,
				     &seq_number);

    _gss_mg_encode_be_uint32(seq_number, p0 + 8);

    krb5_auth_con_setlocalseqnumber (context,
				     context_handle->auth_context,
				     ++seq_number);
    HEIMDAL_MUTEX_unlock(&context_handle->ctx_id_mutex);

    memset (p0 + 8 + 4,
	    (context_handle->more_flags & LOCAL) ? 0 : 0xff,
	    4);

    krb5_generate_random_block(p0 + 24, 8); /* fill in Confounder */

    /* p points to data */
    p = p0 + GSS_ARCFOUR_WRAP_TOKEN_SIZE;
    memcpy(p, input_message_buffer->value, input_message_buffer->length);

    if (!IS_DCE_STYLE(context_handle))
	p[input_message_buffer->length] = 1; /* padding */

    ret = arcfour_mic_cksum(context,
			    key, KRB5_KU_USAGE_SEAL,
			    p0 + 16, 8, /* SGN_CKSUM */
			    p0, 8, /* TOK_ID, SGN_ALG, SEAL_ALG, Filler */
			    p0 + 24, 8, /* Confounder */
			    p0 + GSS_ARCFOUR_WRAP_TOKEN_SIZE,
			    datalen);
    if (ret) {
	*minor_status = ret;
	_gsskrb5_release_buffer(minor_status, output_message_buffer);
	return GSS_S_FAILURE;
    }

    {
	int i;

	Klocal.keytype = key->keytype;
	Klocal.keyvalue.data = Klocaldata;
	Klocal.keyvalue.length = sizeof(Klocaldata);

	for (i = 0; i < 16; i++)
	    Klocaldata[i] = ((u_char *)key->keyvalue.data)[i] ^ 0xF0;
    }
    ret = arcfour_mic_key(context, &Klocal,
			  p0 + 8, 4, /* SND_SEQ */
			  k6_data, sizeof(k6_data));
    memset_s(Klocaldata, sizeof(Klocaldata), 0, sizeof(Klocaldata));
    if (ret) {
	_gsskrb5_release_buffer(minor_status, output_message_buffer);
	*minor_status = ret;
	return GSS_S_FAILURE;
    }


    if(conf_req_flag) {
	EVP_CIPHER_CTX rc4_key;

	EVP_CIPHER_CTX_init(&rc4_key);
	EVP_CipherInit_ex(&rc4_key, EVP_rc4(), NULL, k6_data, NULL, 1);
	EVP_Cipher(&rc4_key, p0 + 24, p0 + 24, 8 + datalen);
	EVP_CIPHER_CTX_cleanup(&rc4_key);
    }
    memset(k6_data, 0, sizeof(k6_data));

    ret = arcfour_mic_key(context, key,
			  p0 + 16, 8, /* SGN_CKSUM */
			  k6_data, sizeof(k6_data));
    if (ret) {
	_gsskrb5_release_buffer(minor_status, output_message_buffer);
	*minor_status = ret;
	return GSS_S_FAILURE;
    }

    {
	EVP_CIPHER_CTX rc4_key;

	EVP_CIPHER_CTX_init(&rc4_key);
	EVP_CipherInit_ex(&rc4_key, EVP_rc4(), NULL, k6_data, NULL, 1);
	EVP_Cipher(&rc4_key, p0 + 8, p0 + 8 /* SND_SEQ */, 8);
	EVP_CIPHER_CTX_cleanup(&rc4_key);
	memset_s(k6_data, sizeof(k6_data), 0, sizeof(k6_data));
    }

    if (conf_state)
	*conf_state = conf_req_flag;

    *minor_status = 0;
    return GSS_S_COMPLETE;
}

OM_uint32 _gssapi_unwrap_arcfour(OM_uint32 *minor_status,
				 const gsskrb5_ctx context_handle,
				 krb5_context context,
				 const gss_buffer_t input_message_buffer,
				 gss_buffer_t output_message_buffer,
				 int *conf_state,
				 gss_qop_t *qop_state,
				 krb5_keyblock *key)
{
    u_char Klocaldata[16];
    krb5_keyblock Klocal;
    krb5_error_code ret;
    uint32_t seq_number;
    size_t datalen;
    OM_uint32 omret;
    u_char k6_data[16], SND_SEQ[8], Confounder[8];
    u_char cksum_data[8];
    u_char *p, *p0;
    int cmp;
    int conf_flag;
    size_t padlen = 0, len;

    if (conf_state)
	*conf_state = 0;
    if (qop_state)
	*qop_state = 0;

    p0 = input_message_buffer->value;

    if (IS_DCE_STYLE(context_handle)) {
	len = GSS_ARCFOUR_WRAP_TOKEN_SIZE +
	    GSS_ARCFOUR_WRAP_TOKEN_DCE_DER_HEADER_SIZE;
	if (input_message_buffer->length < len)
	    return GSS_S_BAD_MECH;
    } else {
	len = input_message_buffer->length;
    }

    omret = _gssapi_verify_mech_header(&p0,
				       len,
				       GSS_KRB5_MECHANISM);
    if (omret)
	return omret;

    /* length of mech header */
    len = (p0 - (u_char *)input_message_buffer->value) +
	GSS_ARCFOUR_WRAP_TOKEN_SIZE;

    if (len > input_message_buffer->length)
	return GSS_S_BAD_MECH;

    /* length of data */
    datalen = input_message_buffer->length - len;

    p = p0;

    if (memcmp(p, "\x02\x01", 2) != 0)
	return GSS_S_BAD_SIG;
    p += 2;
    if (memcmp(p, "\x11\x00", 2) != 0) /* SGN_ALG = HMAC MD5 ARCFOUR */
	return GSS_S_BAD_SIG;
    p += 2;

    if (memcmp (p, "\x10\x00", 2) == 0)
	conf_flag = 1;
    else if (memcmp (p, "\xff\xff", 2) == 0)
	conf_flag = 0;
    else
	return GSS_S_BAD_SIG;

    p += 2;
    if (memcmp (p, "\xff\xff", 2) != 0)
	return GSS_S_BAD_MIC;
    p = NULL;

    ret = arcfour_mic_key(context, key,
			  p0 + 16, 8, /* SGN_CKSUM */
			  k6_data, sizeof(k6_data));
    if (ret) {
	*minor_status = ret;
	return GSS_S_FAILURE;
    }

    {
	EVP_CIPHER_CTX rc4_key;

	EVP_CIPHER_CTX_init(&rc4_key);
	EVP_CipherInit_ex(&rc4_key, EVP_rc4(), NULL, k6_data, NULL, 1);
	EVP_Cipher(&rc4_key, SND_SEQ, p0 + 8, 8);
	EVP_CIPHER_CTX_cleanup(&rc4_key);
	memset_s(k6_data, sizeof(k6_data), 0, sizeof(k6_data));
    }

    _gss_mg_decode_be_uint32(SND_SEQ, &seq_number);

    if (context_handle->more_flags & LOCAL)
	cmp = (ct_memcmp(&SND_SEQ[4], "\xff\xff\xff\xff", 4) != 0);
    else
	cmp = (ct_memcmp(&SND_SEQ[4], "\x00\x00\x00\x00", 4) != 0);

    if (cmp != 0) {
	*minor_status = 0;
	return GSS_S_BAD_MIC;
    }

    {
	int i;

	Klocal.keytype = key->keytype;
	Klocal.keyvalue.data = Klocaldata;
	Klocal.keyvalue.length = sizeof(Klocaldata);

	for (i = 0; i < 16; i++)
	    Klocaldata[i] = ((u_char *)key->keyvalue.data)[i] ^ 0xF0;
    }
    ret = arcfour_mic_key(context, &Klocal,
			  SND_SEQ, 4,
			  k6_data, sizeof(k6_data));
    memset_s(Klocaldata, sizeof(Klocaldata), 0, sizeof(Klocaldata));
    if (ret) {
	*minor_status = ret;
	return GSS_S_FAILURE;
    }

    output_message_buffer->value = malloc(datalen);
    if (output_message_buffer->value == NULL) {
	*minor_status = ENOMEM;
	return GSS_S_FAILURE;
    }
    output_message_buffer->length = datalen;

    if(conf_flag) {
	EVP_CIPHER_CTX rc4_key;

	EVP_CIPHER_CTX_init(&rc4_key);
	EVP_CipherInit_ex(&rc4_key, EVP_rc4(), NULL, k6_data, NULL, 1);
	EVP_Cipher(&rc4_key, Confounder, p0 + 24, 8);
	EVP_Cipher(&rc4_key, output_message_buffer->value, p0 + GSS_ARCFOUR_WRAP_TOKEN_SIZE, datalen);
	EVP_CIPHER_CTX_cleanup(&rc4_key);
    } else {
	memcpy(Confounder, p0 + 24, 8); /* Confounder */
	memcpy(output_message_buffer->value,
	       p0 + GSS_ARCFOUR_WRAP_TOKEN_SIZE,
	       datalen);
    }
    memset(k6_data, 0, sizeof(k6_data));

    if (!IS_DCE_STYLE(context_handle)) {
	ret = _gssapi_verify_pad(output_message_buffer, datalen, &padlen);
	if (ret) {
	    _gsskrb5_release_buffer(minor_status, output_message_buffer);
	    *minor_status = 0;
	    return ret;
	}
	output_message_buffer->length -= padlen;
    }

    ret = arcfour_mic_cksum(context,
			    key, KRB5_KU_USAGE_SEAL,
			    cksum_data, sizeof(cksum_data),
			    p0, 8,
			    Confounder, sizeof(Confounder),
			    output_message_buffer->value,
			    output_message_buffer->length + padlen);
    if (ret) {
	_gsskrb5_release_buffer(minor_status, output_message_buffer);
	*minor_status = ret;
	return GSS_S_FAILURE;
    }

    cmp = ct_memcmp(cksum_data, p0 + 16, 8); /* SGN_CKSUM */
    if (cmp) {
	_gsskrb5_release_buffer(minor_status, output_message_buffer);
	*minor_status = 0;
	return GSS_S_BAD_MIC;
    }

    if (conf_state)
	*conf_state = conf_flag;

    HEIMDAL_MUTEX_lock(&context_handle->ctx_id_mutex);
    omret = _gssapi_msg_order_check(context_handle->order, seq_number);
    HEIMDAL_MUTEX_unlock(&context_handle->ctx_id_mutex);
    if (omret)
	return omret;

    *minor_status = 0;
    return GSS_S_COMPLETE;
}

static OM_uint32
max_wrap_length_arcfour(const gsskrb5_ctx ctx,
			krb5_crypto crypto,
			size_t input_length,
			OM_uint32 *max_input_size)
{
    /*
     * if GSS_C_DCE_STYLE is in use:
     *  - we only need to encapsulate the WRAP token
     * However, since this is a fixed since, we just
     */
    if (IS_DCE_STYLE(ctx)) {
	size_t len, total_len;

	len = GSS_ARCFOUR_WRAP_TOKEN_SIZE;
	_gssapi_encap_length(len, &len, &total_len, GSS_KRB5_MECHANISM);

	if (input_length < len)
	    *max_input_size = 0;
	else
	    *max_input_size = input_length - len;

    } else {
	size_t extrasize = GSS_ARCFOUR_WRAP_TOKEN_SIZE;
	size_t blocksize = 8;
	size_t len, total_len;

	len = 8 + input_length + blocksize + extrasize;

	_gsskrb5_encap_length(len, &len, &total_len, GSS_KRB5_MECHANISM);

	total_len -= input_length; /* token length */
	if (total_len < input_length) {
	    *max_input_size = (input_length - total_len);
	    (*max_input_size) &= (~(OM_uint32)(blocksize - 1));
	} else {
	    *max_input_size = 0;
	}
    }

    return GSS_S_COMPLETE;
}

OM_uint32
_gssapi_wrap_size_arcfour(OM_uint32 *minor_status,
			  const gsskrb5_ctx ctx,
			  krb5_context context,
			  int conf_req_flag,
			  gss_qop_t qop_req,
			  OM_uint32 req_output_size,
			  OM_uint32 *max_input_size,
			  krb5_keyblock *key)
{
    krb5_error_code ret;
    krb5_crypto crypto;

    ret = krb5_crypto_init(context, key, 0, &crypto);
    if (ret != 0) {
	*minor_status = ret;
	return GSS_S_FAILURE;
    }

    ret = max_wrap_length_arcfour(ctx, crypto,
				  req_output_size, max_input_size);
    if (ret != 0) {
	*minor_status = ret;
	krb5_crypto_destroy(context, crypto);
	return GSS_S_FAILURE;
    }

    krb5_crypto_destroy(context, crypto);

    return GSS_S_COMPLETE;
}

OM_uint32
_gssapi_wrap_iov_length_arcfour(OM_uint32 *minor_status,
				gsskrb5_ctx ctx,
				krb5_context context,
				int conf_req_flag,
				gss_qop_t qop_req,
				int *conf_state,
				gss_iov_buffer_desc *iov,
				int iov_count)
{
    OM_uint32 major_status;
    size_t data_len = 0;
    int i;
    gss_iov_buffer_desc *header = NULL;
    gss_iov_buffer_desc *padding = NULL;
    gss_iov_buffer_desc *trailer = NULL;

    *minor_status = 0;

    for (i = 0; i < iov_count; i++) {
	switch(GSS_IOV_BUFFER_TYPE(iov[i].type)) {
	case GSS_IOV_BUFFER_TYPE_EMPTY:
	    break;
	case GSS_IOV_BUFFER_TYPE_DATA:
	    data_len += iov[i].buffer.length;
	    break;
	case GSS_IOV_BUFFER_TYPE_HEADER:
	    if (header != NULL) {
		*minor_status = EINVAL;
		return GSS_S_FAILURE;
	    }
	    header = &iov[i];
	    break;
	case GSS_IOV_BUFFER_TYPE_TRAILER:
	    if (trailer != NULL) {
		*minor_status = EINVAL;
		return GSS_S_FAILURE;
	    }
	    trailer = &iov[i];
	    break;
	case GSS_IOV_BUFFER_TYPE_PADDING:
	    if (padding != NULL) {
		*minor_status = EINVAL;
		return GSS_S_FAILURE;
	    }
	    padding = &iov[i];
	    break;
	case GSS_IOV_BUFFER_TYPE_SIGN_ONLY:
	    break;
	default:
	    *minor_status = EINVAL;
	    return GSS_S_FAILURE;
	}
    }

    if (header == NULL) {
        *minor_status = EINVAL;
        return GSS_S_FAILURE;
    }

    major_status = _gk_verify_buffers(minor_status, ctx, header,
				      padding, trailer, FALSE);
    if (major_status != GSS_S_COMPLETE) {
	    return major_status;
    }

    if (IS_DCE_STYLE(ctx)) {
	size_t len = GSS_ARCFOUR_WRAP_TOKEN_SIZE;
	size_t total_len;
	_gssapi_encap_length(len, &len, &total_len, GSS_KRB5_MECHANISM);
	header->buffer.length = total_len;
    } else {
	size_t len;
	size_t total_len;
	if (padding) {
	    data_len += 1; /* padding */
	}
	len = data_len + GSS_ARCFOUR_WRAP_TOKEN_SIZE;
	_gssapi_encap_length(len, &len, &total_len, GSS_KRB5_MECHANISM);
	header->buffer.length = total_len - data_len;
    }

    if (trailer) {
	trailer->buffer.length = 0;
    }

    if (padding) {
	padding->buffer.length = 1;
    }

    return GSS_S_COMPLETE;
}

OM_uint32
_gssapi_wrap_iov_arcfour(OM_uint32 *minor_status,
			 gsskrb5_ctx ctx,
			 krb5_context context,
			 int conf_req_flag,
			 int *conf_state,
			 gss_iov_buffer_desc *iov,
			 int iov_count,
			 krb5_keyblock *key)
{
    OM_uint32 major_status, junk;
    gss_iov_buffer_desc *header, *padding, *trailer;
    krb5_error_code kret;
    int32_t seq_number;
    u_char Klocaldata[16], k6_data[16], *p, *p0;
    size_t make_len = 0;
    size_t header_len = 0;
    size_t data_len = 0;
    krb5_keyblock Klocal;
    int i;

    header = _gk_find_buffer(iov, iov_count, GSS_IOV_BUFFER_TYPE_HEADER);
    padding = _gk_find_buffer(iov, iov_count, GSS_IOV_BUFFER_TYPE_PADDING);
    trailer = _gk_find_buffer(iov, iov_count, GSS_IOV_BUFFER_TYPE_TRAILER);

    major_status = _gk_verify_buffers(minor_status, ctx, header,
				      padding, trailer, FALSE);
    if (major_status != GSS_S_COMPLETE) {
	return major_status;
    }

    for (i = 0; i < iov_count; i++) {
	switch (GSS_IOV_BUFFER_TYPE(iov[i].type)) {
	case GSS_IOV_BUFFER_TYPE_DATA:
	    break;
	default:
	    continue;
	}

	data_len += iov[i].buffer.length;
    }

    if (padding) {
	data_len += 1;
    }

    if (IS_DCE_STYLE(ctx)) {
	size_t unwrapped_len;
	unwrapped_len = GSS_ARCFOUR_WRAP_TOKEN_SIZE;
	_gssapi_encap_length(unwrapped_len,
			     &make_len,
			     &header_len,
			     GSS_KRB5_MECHANISM);
    } else {
	size_t unwrapped_len;
	unwrapped_len = GSS_ARCFOUR_WRAP_TOKEN_SIZE + data_len;
	_gssapi_encap_length(unwrapped_len,
			     &make_len,
			     &header_len,
			     GSS_KRB5_MECHANISM);
	header_len -= data_len;
    }

    if (GSS_IOV_BUFFER_FLAGS(header->type) & GSS_IOV_BUFFER_FLAG_ALLOCATE) {
	major_status = _gk_allocate_buffer(minor_status, header,
					   header_len);
	if (major_status != GSS_S_COMPLETE)
	    goto failure;
    } else if (header->buffer.length < header_len) {
	*minor_status = KRB5_BAD_MSIZE;
	major_status = GSS_S_FAILURE;
	goto failure;
    } else {
	header->buffer.length = header_len;
    }

    if (padding) {
	if (GSS_IOV_BUFFER_FLAGS(padding->type) & GSS_IOV_BUFFER_FLAG_ALLOCATE) {
	    major_status = _gk_allocate_buffer(minor_status, padding, 1);
	    if (major_status != GSS_S_COMPLETE)
		goto failure;
	} else if (padding->buffer.length < 1) {
	    *minor_status = KRB5_BAD_MSIZE;
	    major_status = GSS_S_FAILURE;
	    goto failure;
	} else {
	    padding->buffer.length = 1;
	}
	memset(padding->buffer.value, 1, 1);
    }

    if (trailer) {
	trailer->buffer.length = 0;
	trailer->buffer.value = NULL;
    }

    p0 = _gssapi_make_mech_header(header->buffer.value,
				  make_len,
				  GSS_KRB5_MECHANISM);
    p = p0;

    *p++ = 0x02; /* TOK_ID */
    *p++ = 0x01;
    *p++ = 0x11; /* SGN_ALG */
    *p++ = 0x00;
    if (conf_req_flag) {
	*p++ = 0x10; /* SEAL_ALG */
	*p++ = 0x00;
    } else {
	*p++ = 0xff; /* SEAL_ALG */
	*p++ = 0xff;
    }
    *p++ = 0xff; /* Filler */
    *p++ = 0xff;

    p = NULL;

    HEIMDAL_MUTEX_lock(&ctx->ctx_id_mutex);
    krb5_auth_con_getlocalseqnumber(context,
				    ctx->auth_context,
				    &seq_number);
    _gss_mg_encode_be_uint32(seq_number, p0 + 8);

    krb5_auth_con_setlocalseqnumber(context,
				    ctx->auth_context,
				    ++seq_number);
    HEIMDAL_MUTEX_unlock(&ctx->ctx_id_mutex);

    memset(p0 + 8 + 4,
           (ctx->more_flags & LOCAL) ? 0 : 0xff,
           4);

    krb5_generate_random_block(p0 + 24, 8); /* fill in Confounder */

    /* Sign Data */
    kret = arcfour_mic_cksum_iov(context,
				 key, KRB5_KU_USAGE_SEAL,
				 p0 + 16, 8, /* SGN_CKSUM */
				 p0, 8, /* TOK_ID, SGN_ALG, SEAL_ALG, Filler */
				 p0 + 24, 8, /* Confounder */
				 iov, iov_count, /* Data + SignOnly */
				 padding); /* padding */
    if (kret) {
	*minor_status = kret;
	major_status = GSS_S_FAILURE;
	goto failure;
    }

    Klocal.keytype = key->keytype;
    Klocal.keyvalue.data = Klocaldata;
    Klocal.keyvalue.length = sizeof(Klocaldata);

    for (i = 0; i < 16; i++) {
	Klocaldata[i] = ((u_char *)key->keyvalue.data)[i] ^ 0xF0;
    }
    kret = arcfour_mic_key(context, &Klocal,
			   p0 + 8, 4, /* SND_SEQ */
			   k6_data, sizeof(k6_data));
    memset_s(Klocaldata, sizeof(Klocaldata), 0, sizeof(Klocaldata));
    if (kret) {
	*minor_status = kret;
	major_status = GSS_S_FAILURE;
	goto failure;
    }

    if (conf_req_flag) {
	EVP_CIPHER_CTX rc4_key;

	EVP_CIPHER_CTX_init(&rc4_key);
	EVP_CipherInit_ex(&rc4_key, EVP_rc4(), NULL, k6_data, NULL, 1);

	/* Confounder */
	EVP_Cipher(&rc4_key, p0 + 24, p0 + 24, 8);

	/* Seal Data */
	for (i=0; i < iov_count; i++) {
	    switch (GSS_IOV_BUFFER_TYPE(iov[i].type)) {
	    case GSS_IOV_BUFFER_TYPE_DATA:
		break;
	    default:
		continue;
	    }

	    EVP_Cipher(&rc4_key, iov[i].buffer.value,
		       iov[i].buffer.value, iov[i].buffer.length);
	}

	/* Padding */
	if (padding) {
	    EVP_Cipher(&rc4_key, padding->buffer.value,
		       padding->buffer.value, padding->buffer.length);
	}

	EVP_CIPHER_CTX_cleanup(&rc4_key);
    }
    memset(k6_data, 0, sizeof(k6_data));

    kret = arcfour_mic_key(context, key,
			   p0 + 16, 8, /* SGN_CKSUM */
			   k6_data, sizeof(k6_data));
    if (kret) {
	*minor_status = kret;
	major_status = GSS_S_FAILURE;
        return major_status;
    }

    {
	EVP_CIPHER_CTX rc4_key;

	EVP_CIPHER_CTX_init(&rc4_key);
	EVP_CipherInit_ex(&rc4_key, EVP_rc4(), NULL, k6_data, NULL, 1);
	EVP_Cipher(&rc4_key, p0 + 8, p0 + 8, 8); /* SND_SEQ */
	EVP_CIPHER_CTX_cleanup(&rc4_key);

	memset(k6_data, 0, sizeof(k6_data));
    }

    if (conf_state)
	*conf_state = conf_req_flag;

    *minor_status = 0;
    return GSS_S_COMPLETE;

failure:

    gss_release_iov_buffer(&junk, iov, iov_count);

    return major_status;
}

OM_uint32
_gssapi_unwrap_iov_arcfour(OM_uint32 *minor_status,
			   gsskrb5_ctx ctx,
			   krb5_context context,
			   int *pconf_state,
			   gss_qop_t *pqop_state,
			   gss_iov_buffer_desc *iov,
			   int iov_count,
			   krb5_keyblock *key)
{
    OM_uint32 major_status;
    gss_iov_buffer_desc *header, *padding, *trailer;
    krb5_keyblock Klocal;
    uint8_t Klocaldata[16];
    uint8_t k6_data[16], snd_seq[8], Confounder[8];
    uint8_t cksum_data[8];
    uint8_t *_p = NULL;
    const uint8_t *p, *p0;
    size_t verify_len = 0;
    uint32_t seq_number;
    size_t hlen = 0;
    int conf_state;
    int cmp;
    size_t i;
    krb5_error_code kret;
    OM_uint32 ret;

    if (pconf_state != NULL) {
	*pconf_state = 0;
    }
    if (pqop_state != NULL) {
	*pqop_state = 0;
    }

    header = _gk_find_buffer(iov, iov_count, GSS_IOV_BUFFER_TYPE_HEADER);
    padding = _gk_find_buffer(iov, iov_count, GSS_IOV_BUFFER_TYPE_PADDING);
    trailer = _gk_find_buffer(iov, iov_count, GSS_IOV_BUFFER_TYPE_TRAILER);

    /* Check if the packet is correct */
    major_status = _gk_verify_buffers(minor_status,
				      ctx,
				      header,
				      padding,
				      trailer,
				      FALSE); /* behaves as stream cipher */
    if (major_status != GSS_S_COMPLETE) {
	return major_status;
    }

    if (padding != NULL && padding->buffer.length != 1) {
	*minor_status = EINVAL;
	return GSS_S_FAILURE;
    }

    verify_len = header->buffer.length;

    if (!IS_DCE_STYLE(ctx)) {
	for (i = 0; i < iov_count; i++) {
	    /* length in header also includes data and padding */
	    if (GSS_IOV_BUFFER_TYPE(iov[i].type) == GSS_IOV_BUFFER_TYPE_DATA)
		verify_len += iov[i].buffer.length;
	}

	if (padding)
	    verify_len += padding->buffer.length;
    }

    _p = header->buffer.value;

    ret = _gssapi_verify_mech_header(&_p,
				     verify_len,
				     GSS_KRB5_MECHANISM);
    if (ret) {
	return ret;
    }
    p0 = _p;

    /* length of mech header */
    hlen = (p0 - (uint8_t *)header->buffer.value);
    hlen += GSS_ARCFOUR_WRAP_TOKEN_SIZE;

    if (hlen > header->buffer.length) {
	return GSS_S_BAD_MECH;
    }

    p = p0;

    if (memcmp(p, "\x02\x01", 2) != 0)
	return GSS_S_BAD_SIG;
    p += 2;
    if (memcmp(p, "\x11\x00", 2) != 0) /* SGN_ALG = HMAC MD5 ARCFOUR */
	return GSS_S_BAD_SIG;
    p += 2;

    if (memcmp (p, "\x10\x00", 2) == 0)
	conf_state = 1;
    else if (memcmp (p, "\xff\xff", 2) == 0)
	conf_state = 0;
    else
	return GSS_S_BAD_SIG;

    p += 2;
    if (memcmp (p, "\xff\xff", 2) != 0)
	return GSS_S_BAD_MIC;
    p = NULL;

    kret = arcfour_mic_key(context,
			   key,
			   p0 + 16, /* SGN_CKSUM */
			   8,       /* SGN_CKSUM_LEN */
			   k6_data,
			   sizeof(k6_data));
    if (kret) {
	*minor_status = kret;
	return GSS_S_FAILURE;
    }

    {
	EVP_CIPHER_CTX rc4_key;

	EVP_CIPHER_CTX_init(&rc4_key);
	EVP_CipherInit_ex(&rc4_key, EVP_rc4(), NULL, k6_data, NULL, 1);
	EVP_Cipher(&rc4_key, snd_seq, p0 + 8, 8); /* SND_SEQ */
	EVP_CIPHER_CTX_cleanup(&rc4_key);

	memset(k6_data, 0, sizeof(k6_data));
    }

    _gss_mg_decode_be_uint32(snd_seq, &seq_number);

    if (ctx->more_flags & LOCAL) {
	cmp = (ct_memcmp(&snd_seq[4], "\xff\xff\xff\xff", 4) != 0);
    } else {
	cmp = (ct_memcmp(&snd_seq[4], "\x00\x00\x00\x00", 4) != 0);
    }
    if (cmp != 0) {
	*minor_status = 0;
	return GSS_S_BAD_MIC;
    }

    /* keyblock */
    Klocal.keytype = key->keytype;
    Klocal.keyvalue.data = Klocaldata;
    Klocal.keyvalue.length = sizeof(Klocaldata);

    for (i = 0; i < 16; i++) {
	Klocaldata[i] = ((u_char *)key->keyvalue.data)[i] ^ 0xF0;
    }

    kret = arcfour_mic_key(context,
			   &Klocal,
			   snd_seq,
			   4,
			   k6_data, sizeof(k6_data));
    memset_s(Klocaldata, sizeof(Klocaldata), 0, sizeof(Klocaldata));
    if (kret) {
	*minor_status = kret;
	return GSS_S_FAILURE;
    }

    if (conf_state == 1) {
	EVP_CIPHER_CTX rc4_key;

	EVP_CIPHER_CTX_init(&rc4_key);
	EVP_CipherInit_ex(&rc4_key, EVP_rc4(), NULL, k6_data, NULL, 1);

	/* Confounder */
	EVP_Cipher(&rc4_key, Confounder, p0 + 24, 8);

	/* Data */
	for (i = 0; i < iov_count; i++) {
	    switch (GSS_IOV_BUFFER_TYPE(iov[i].type)) {
	    case GSS_IOV_BUFFER_TYPE_DATA:
		break;
	    default:
		continue;
	    }

	    EVP_Cipher(&rc4_key, iov[i].buffer.value,
		       iov[i].buffer.value, iov[i].buffer.length);
	}

	/* Padding */
	if (padding) {
	    EVP_Cipher(&rc4_key, padding->buffer.value,
		       padding->buffer.value, padding->buffer.length);
	}

	EVP_CIPHER_CTX_cleanup(&rc4_key);
    } else {
	/* Confounder */
	memcpy(Confounder, p0 + 24, 8);
    }
    memset(k6_data, 0, sizeof(k6_data));

    /* Prepare the buffer for signing */
    kret = arcfour_mic_cksum_iov(context,
				 key, KRB5_KU_USAGE_SEAL,
				 cksum_data, sizeof(cksum_data),
				 p0, 8,
				 Confounder, sizeof(Confounder),
				 iov, iov_count,
				 padding);
    if (kret) {
	*minor_status = kret;
	return GSS_S_FAILURE;
    }

    cmp = (ct_memcmp(cksum_data, p0 + 16, 8) != 0); /* SGN_CKSUM */
    if (cmp) {
	*minor_status = 0;
	return GSS_S_BAD_MIC;
    }

    if (padding) {
	size_t plen;

	ret = _gssapi_verify_pad(&padding->buffer, 1, &plen);
	if (ret) {
	    *minor_status = 0;
	    return ret;
	}
    }

    if (pconf_state) {
	*pconf_state = conf_state;
    }

    HEIMDAL_MUTEX_lock(&ctx->ctx_id_mutex);
    ret = _gssapi_msg_order_check(ctx->order, seq_number);
    HEIMDAL_MUTEX_unlock(&ctx->ctx_id_mutex);
    if (ret != 0) {
	return ret;
    }

    *minor_status = 0;
    return GSS_S_COMPLETE;
}
