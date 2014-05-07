MVM_PUBLIC MVMString * MVM_string_utf8_decode(MVMThreadContext *tc, MVMObject *result_type, const MVMuint8 *utf8, size_t bytes);
MVM_PUBLIC void MVM_string_utf8_decodestream(MVMThreadContext *tc, MVMDecodeStream *ds, MVMint32 *stopper_chars, MVMint32 *stopper_sep);
MVM_PUBLIC MVMuint8 * MVM_string_utf8_encode_substr(MVMThreadContext *tc,
        MVMString *str, MVMuint64 *output_size, MVMint64 start, MVMint64 length);
MVM_PUBLIC MVMuint8 * MVM_string_utf8_encode(MVMThreadContext *tc, MVMString *str, MVMuint64 *output_size);
MVM_PUBLIC char * MVM_string_utf8_encode_C_string(MVMThreadContext *tc, MVMString *str);
