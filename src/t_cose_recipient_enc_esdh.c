/**
 * \file t_cose_recipient_enc_esdh.c
 *
 * Copyright (c) 2022, Arm Limited. All rights reserved.
 * Copyright (c) 2023, Laurence Lundblade. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * See BSD-3-Clause license in README.md
 *
 */

#ifndef T_COSE_DISABLE_ESDH

#include <stdint.h>
#include "qcbor/qcbor_encode.h"
#include "t_cose/t_cose_recipient_enc.h"
#include "t_cose/t_cose_recipient_enc_esdh.h" /* Interface implemented */
#include "t_cose/t_cose_encrypt_enc.h"
#include "t_cose/t_cose_common.h"
#include "t_cose/q_useful_buf.h"
#include "t_cose/t_cose_standard_constants.h"
#include "t_cose_crypto.h"
#include "t_cose_util.h"
#include "t_cose/t_cose_recipient_enc_keywrap.h"


static enum t_cose_err_t
ephem_special_encode_cb(const struct t_cose_parameter  *parameter,
                        QCBOREncodeContext             *cbor_encoder)
{
    enum t_cose_err_t      result;
    int32_t                cose_curve;
    MakeUsefulBufOnStack(  x_coord_buf, 64); // TODO: buffer size
    MakeUsefulBufOnStack(  y_coord_buf, 64); // TODO: buffer size
    struct q_useful_buf_c  x_coord;
    struct q_useful_buf_c  y_coord;
    bool                   y_sign;

    result = t_cose_crypto_export_ec2_key(parameter->value.special_encode.data.key,
                                         &cose_curve,
                                          x_coord_buf,
                                         &x_coord,
                                          y_coord_buf,
                                         &y_coord,
                                        &y_sign);
    if(result != T_COSE_SUCCESS) {
        return result;
    }

    /* Create ephemeral key parameter map */
    QCBOREncode_OpenMapInMapN(cbor_encoder, T_COSE_HEADER_ALG_PARAM_EPHEMERAL_KEY);

    QCBOREncode_AddInt64ToMapN(cbor_encoder, T_COSE_KEY_COMMON_KTY, T_COSE_KEY_TYPE_EC2);
    QCBOREncode_AddInt64ToMapN(cbor_encoder, T_COSE_KEY_PARAM_CRV, cose_curve);
    QCBOREncode_AddBytesToMapN(cbor_encoder, T_COSE_KEY_PARAM_X_COORDINATE, x_coord);
    if(q_useful_buf_c_is_null(y_coord)) {
        QCBOREncode_AddBoolToMapN(cbor_encoder, T_COSE_KEY_PARAM_Y_COORDINATE, y_sign);
    } else {
        QCBOREncode_AddBytesToMapN(cbor_encoder, T_COSE_KEY_PARAM_Y_COORDINATE, y_coord);
    }

    QCBOREncode_CloseMap(cbor_encoder);

    // TODO: should cbor encode error be checked here?
    return T_COSE_SUCCESS;
}


/*
 * See documentation in t_cose_recipient_enc_esdh.h
 */
enum t_cose_err_t
t_cose_recipient_create_esdh_cb_private(struct t_cose_recipient_enc  *me_x,
                                        struct q_useful_buf_c         cek,
                                        const struct t_cose_alg_and_bits ce_alg,
                                        QCBOREncodeContext           *cbor_encoder)
{
    uint8_t                 kek[T_COSE_CIPHER_ENCRYPT_OUTPUT_MAX_SIZE(T_COSE_MAX_SYMMETRIC_KEY_LENGTH)];
    size_t                  pkR_len = T_COSE_EXPORT_PUBLIC_KEY_MAX_SIZE;
    uint8_t                 pkR[T_COSE_EXPORT_PUBLIC_KEY_MAX_SIZE] = {0};
    enum t_cose_err_t       return_value;
    struct t_cose_key       ephemeral_key;
    MakeUsefulBufOnStack(   info_struct_buf, 200); // TODO: allow this to be
                                                // supplied externally
    struct q_useful_buf_c   protected_hdr;
    struct q_useful_buf_c   info_struct;
    size_t                  output_kek_len;
    struct t_cose_parameter params[3];
    struct t_cose_parameter  *params2;
    struct q_useful_buf     encrypted_cek_destination;
    struct q_useful_buf_c   encrypted_cek_result;
    struct t_cose_key       kek_handle;
    size_t                  target_kek_len;
    int32_t                 hash_alg;
    int32_t                 kw_alg;
    size_t                  ecdhe_derived_key_len;
    struct t_cose_recipient_enc_esdh *context;
    Q_USEFUL_BUF_MAKE_STACK_UB(derived_key, T_COSE_RAW_KEY_AGREEMENT_OUTPUT_MAX_SIZE );

    context = (struct t_cose_recipient_enc_esdh *)me_x;

    switch (context->esdh_suite.ckd_id)
    {
    case T_COSE_ALGORITHM_ECDH_ES_A128KW:
        target_kek_len = 16;
        break;
    case T_COSE_ALGORITHM_ECDH_ES_A192KW:
        target_kek_len = 24;
        break;
    case T_COSE_ALGORITHM_ECDH_ES_A256KW:
        target_kek_len = 32;
        break;
    default:
        return(T_COSE_ERR_UNSUPPORTED_ENCRYPTION_ALG);
    }
    (void)target_kek_len; // Will use this at some point

    /* Create ephemeral key */
    return_value = t_cose_crypto_generate_key(&ephemeral_key,
                                              context->esdh_suite.curve_id);
    if (return_value != T_COSE_SUCCESS) {
        return(return_value);
    }

    /* Export pkR */
    return_value = t_cose_crypto_export_public_key(
                         context->pkR,
                         (struct q_useful_buf) {.ptr=pkR, .len=pkR_len},
                         &pkR_len);

    if (return_value != T_COSE_SUCCESS) {
        return(return_value);
    }


    /* Create COSE_recipient array */
    QCBOREncode_OpenArray(cbor_encoder);

    /* ---- Make linked list of parameters and encode them ---- */
    params[0]  = t_cose_param_make_alg_id(context->esdh_suite.ckd_id);

    params[1].value_type = T_COSE_PARAMETER_TYPE_SPECIAL;
    params[1].value.special_encode.data.key = ephemeral_key;
    params[1].value.special_encode.encode_cb = ephem_special_encode_cb;
    params[1].label = T_COSE_HEADER_ALG_PARAM_EPHEMERAL_KEY;
    params[0].next = &params[1];

    if(!q_useful_buf_c_is_null(context->kid)) {
        params[2] = t_cose_param_make_kid(context->kid);
        params[1].next = &params[2];
    }

    params2 = params;
    t_cose_params_append(&params2, context->added_params);

    return_value = t_cose_headers_encode(cbor_encoder,
                                         &params[0],
                                         &protected_hdr);



    /* --- Make Info structure ---- */

    (void)ce_alg; // TODO: put this to use
    return_value = create_info_structure(context->info->enc_alg,
                                         context->info->sender_identity_type_id,
                                         context->info->sender_identity,
                                         context->info->recipient_identity_type_id,
                                         context->info->recipient_identity,
                                         protected_hdr,
                                         context->info->enc_ctx->hash_cose_algorithm_id,
                                         (struct q_useful_buf_c)
                                         {.ptr = context->info->enc_ctx->extern_hash_buffer.ptr,
                                          .len =  context->info->enc_ctx->extern_hash_buffer.len
                                         },
                                         info_struct_buf,
                                        &info_struct);

    if (return_value != T_COSE_SUCCESS) {
        return(return_value);
    }

    /* --- Generation of ECDHE derived key with HKDF applied afterwards ---- */
    return_value = t_cose_crypto_key_agreement(
                        context->esdh_suite.ckd_id, // Content Key Distribution Method
                        ephemeral_key,              // Private Key
                        context->pkR,               // Public Key
                        derived_key,                // Derived Key
                        &ecdhe_derived_key_len);

    if(return_value) {
        return T_COSE_ERR_KEY_AGREEMENT_FAIL;
    }

    /* Determine hash algorithm for HKDF and the length of the
     * KEK to be produced. The length of the KEK aligns with
     * the AES KW algorithm selected.
     *
     * All three ES-DH content key distribution algorithms use SHA256
     * with the HKDF-based key derivation function.
     */

    switch(context->esdh_suite.ckd_id) {
    case T_COSE_ALGORITHM_ECDH_ES_A128KW:
        output_kek_len = 128/8;
        hash_alg = T_COSE_ALGORITHM_SHA_256;
        break;
    case T_COSE_ALGORITHM_ECDH_ES_A192KW:
        output_kek_len = 192/8;
        hash_alg = T_COSE_ALGORITHM_SHA_256;
        break;
    case T_COSE_ALGORITHM_ECDH_ES_A256KW:
        output_kek_len = 256/8;
        hash_alg = T_COSE_ALGORITHM_SHA_256;
        break;
    default:
        return T_COSE_ERR_UNSUPPORTED_CONTENT_KEY_DISTRIBUTION_ALG;
    }

    /* HKDF-based Key Derivation */
    return_value = t_cose_crypto_hkdf(hash_alg,       // Hash Algorithm
                             NULL_Q_USEFUL_BUF_C,     // Empty Salt
                             (struct q_useful_buf_c)  // IKM
                                {
                                    derived_key.ptr,
                                    ecdhe_derived_key_len
                                },
                             info_struct,             // Info Context Structure
                             (struct q_useful_buf)  // OKM
                                {
                                    kek,
                                    output_kek_len
                                });
    if(return_value) {
        return T_COSE_ERR_HKDF_FAIL;
    }

    /* Free ephemeral key (which is not a symmetric key!) */
    /* TBD: Rename the function to t_cose_crypto_free_key() */
    t_cose_crypto_free_symmetric_key(ephemeral_key);

    if (return_value != T_COSE_SUCCESS) {
        return(return_value);
    }

    switch(context->esdh_suite.ckd_id)
    {
    case T_COSE_ALGORITHM_ECDH_ES_A256KW:
        return_value = t_cose_crypto_make_symmetric_key_handle(T_COSE_ALGORITHM_A256GCM,
                                      (struct q_useful_buf_c) {.ptr = kek, .len = output_kek_len},
                                                &kek_handle);
        kw_alg = T_COSE_ALGORITHM_A256KW;
        break;
    case T_COSE_ALGORITHM_ECDH_ES_A192KW:
        return_value = t_cose_crypto_make_symmetric_key_handle(T_COSE_ALGORITHM_A192GCM,
                                      (struct q_useful_buf_c) {.ptr = kek, .len = output_kek_len},
                                                &kek_handle);
        kw_alg = T_COSE_ALGORITHM_A192KW;
        break;
    case T_COSE_ALGORITHM_ECDH_ES_A128KW:
        return_value = t_cose_crypto_make_symmetric_key_handle(T_COSE_ALGORITHM_A128GCM,
                                      (struct q_useful_buf_c) {.ptr = kek, .len = output_kek_len},
                                                &kek_handle);
        kw_alg = T_COSE_ALGORITHM_A128KW;
        break;
    default:
        return(T_COSE_ERR_UNSUPPORTED_CONTENT_KEY_DISTRIBUTION_ALG);
    }

    if (return_value != T_COSE_SUCCESS) {
        return(return_value);
    }

    /* Do the keywrap directly into the output buffer
     * t_cose_crypto_kw_wrap() will catch incorrect algorithm ID errors
     */
    QCBOREncode_OpenBytes(cbor_encoder, &encrypted_cek_destination);
    return_value = t_cose_crypto_kw_wrap(kw_alg,        // key wrap algorithm
                                         kek_handle,    // key encryption key
                                         cek,           // "plaintext" = cek
                                         encrypted_cek_destination,
                                        &encrypted_cek_result);
    if (return_value != T_COSE_SUCCESS) {
        return(return_value);
    }
    QCBOREncode_CloseBytes(cbor_encoder, encrypted_cek_result.len);

    /* Close recipient array */
    QCBOREncode_CloseArray(cbor_encoder);

    /* Free KEK */
    t_cose_crypto_free_symmetric_key(kek_handle);

    return(T_COSE_SUCCESS);
}


#else

/* Place holder for compiler tools that don't like files with no functions */
void t_cose_recipient_enc_esdh_placeholder(void) {}

#endif /* T_COSE_DISABLE_ESDH */
