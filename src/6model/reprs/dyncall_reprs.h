INTVAL get_nc_repr_id(void);
INTVAL get_cs_repr_id(void);
INTVAL get_cp_repr_id(void);
INTVAL get_ca_repr_id(void);

MVMObject *make_int_result(MVMThreadContext *tc, MVMObject *type, INTVAL value);
MVMObject *make_num_result(MVMThreadContext *tc, MVMObject *type, MVMnum64 value);
MVMObject *make_str_result(MVMThreadContext *tc, MVMObject *type, INTVAL ret_type, char *cstring);
MVMObject *make_cstruct_result(MVMThreadContext *tc, MVMObject *type, void *cstruct);
MVMObject *make_cpointer_result(MVMThreadContext *tc, MVMObject *type, void *ptr);
MVMObject *make_carray_result(MVMThreadContext *tc, MVMObject *type, void *carray);
