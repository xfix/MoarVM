MVMuint32 MVM_nc_NativeCall_repr_id(void);
MVMuint32 MVM_nc_CStruct_repr_id(void);
MVMuint32 MVM_nc_CPointer_repr_id(void);
MVMuint32 MVM_nc_CArray_repr_id(void);

MVMObject *MVM_nc_make_int_result(MVMThreadContext *tc, MVMObject *type, MVMint64 value);
MVMObject *MVM_nc_make_num_result(MVMThreadContext *tc, MVMObject *type, MVMnum64 value);
MVMObject *MVM_nc_make_str_result(MVMThreadContext *tc, MVMObject *type, MVMuint16 ret_type, char *cstring);
MVMObject *MVM_nc_make_cstruct_result(MVMThreadContext *tc, MVMObject *type, void *cstruct);
MVMObject *MVM_nc_make_cpointer_result(MVMThreadContext *tc, MVMObject *type, void *ptr);
MVMObject *MVM_nc_make_carray_result(MVMThreadContext *tc, MVMObject *type, void *carray);
