void MVM_gc_run_gc(MVMThreadContext *tc, MVMuint8 what_to_do);
void MVM_gc_process_in_tray(MVMThreadContext *tc, MVMuint8 gen, MVMuint32 *put_vote);
void MVM_gc_register_for_gc_run(MVMThreadContext *tc);
