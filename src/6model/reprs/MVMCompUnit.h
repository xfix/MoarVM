struct MVMExtOpRecord {
    /* Used to query the extop registry. */
    MVMString *name;

    /* Resolved by the validator. */
    MVMOpInfo *info;

    /* The actual function executed by the interpreter.
     * Resolved by the validator. */
    MVMExtOpFunc *func;

    /* Tells the interpreter by how much to increment
     * the instruction pointer. */
    MVMuint16 operand_bytes;

    /* Read from the bytecode stream. */
    MVMuint8 operand_descriptor[MVM_MAX_OPERANDS];
};

/* How to release memory. */
typedef enum {
    MVM_DEALLOCATE_NOOP,
    MVM_DEALLOCATE_FREE,
    MVM_DEALLOCATE_UNMAP
} MVMDeallocate;

/* Representation for a compilation unit in the VM. */
struct MVMCompUnitBody {
    /* The start and size of the raw data for this compilation unit. */
    MVMuint8  *data_start;
    MVMuint32  data_size;

    /* The various static frames in the compilation unit, along with a
     * code object for each one. */
    MVMStaticFrame **frames;
    MVMObject      **coderefs;
    MVMuint32        num_frames;
    MVMStaticFrame  *main_frame;
    MVMStaticFrame  *load_frame;
    MVMStaticFrame  *deserialize_frame;

    /* The callsites in the compilation unit. */
    MVMCallsite **callsites;
    MVMuint32     num_callsites;
    MVMuint16     max_callsite_size;

    /* The extension ops used by the compilation unit. */
    MVMExtOpRecord *extops;
    MVMuint16       num_extops;

    /* The string heap and number of strings. */
    MVMString **strings;
    MVMuint32   num_strings;

    /* Serialized data, if any. */
    char     *serialized;
    MVMint32  serialized_size;

    /* Array of the resolved serialization contexts, and how many we
     * have. A null in the list indicates not yet resolved */
    MVMSerializationContext **scs;
    MVMuint32                 num_scs;

    /* List of serialization contexts in need of resolution. This is an
     * array of string handles; its length is determined by num_scs above.
     * once an SC has been resolved, the entry on this list is NULLed. If
     * all are resolved, this pointer itself becomes NULL. */
    MVMSerializationContextBody **scs_to_resolve;

    /* List of SC handle string indexes. */
    MVMint32 *sc_handle_idxs;

    /* HLL configuration for this compilation unit. */
    MVMHLLConfig *hll_config;
    MVMString    *hll_name;

    /* Filename, if any, that we loaded it from. */
    MVMString *filename;

    /* Handle, if any, associated with a mapped file. */
    void *handle;

    /* How we should deallocate data_start. */
    MVMDeallocate deallocate;
};
struct MVMCompUnit {
    MVMObject common;
    MVMCompUnitBody body;
};

struct MVMLoadedCompUnitName {
    /* Loaded filename. */
    MVMString *filename;

    /* Inline handle to the loaded filenames hash (in MVMInstance). */
    UT_hash_handle hash_handle;
};

/* Function for REPR setup. */
const MVMREPROps * MVMCompUnit_initialize(MVMThreadContext *tc);
