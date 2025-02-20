/* This represents the root of the serialization data; everything hangs
 * off this. In read mode, we don't do much besides populate and then
 * read this. In write mode, however, the tables and data chunks will be
 * filled out and grown as needed. */
struct MVMSerializationRoot {
    /* The version of the serialization format. */
    MVMint32 version;

    /* The SC we're serializing/deserializing. */
    MVMSerializationContext *sc;

    /* List of the serialization context objects that we depend on. */
    MVMSerializationContext **dependent_scs;

    /* The number of dependencies, as well as a pointer to the
     * dependencies table. */
    char     *dependencies_table;
    MVMint32  num_dependencies;

    /* The number of STables, as well as pointers to the STables
     * table and data chunk. */
    MVMint32  num_stables;
    char     *stables_table;
    char     *stables_data;

    /* The number of objects, as well as pointers to the objects
     * table and data chunk. */
    char     *objects_table;
    char     *objects_data;
    MVMint32  num_objects;

    /* The number of closures, as we as a pointer to the closures
     * table. */
    MVMint32  num_closures;
    char     *closures_table;

    /* The number of contexts (e.g. frames), as well as pointers to
     * the contexts table and data chunk. */
    char     *contexts_table;
    char     *contexts_data;
    MVMint32  num_contexts;

    /* The number of repossessions and pointer to repossessions table. */
    MVMint32  num_repos;
    char     *repos_table;

    /* Array of STRINGs. */
    MVMObject *string_heap;
};

/* Represents the serialization reader and the various functions available
 * on it. */
struct MVMSerializationReader {
    /* Serialization root data. */
    MVMSerializationRoot root;

    /* The object repossession conflicts list. */
    MVMObject *repo_conflicts_list;

    /* Current offsets for the data chunks (also correspond to the amount of
     * data written in to them). */
    MVMint32 stables_data_offset;
    MVMint32 objects_data_offset;
    MVMint32 contexts_data_offset;

    /* Limits up to where we can read stables, objects and contexts data. */
    char *stables_data_end;
    char *objects_data_end;
    char *contexts_data_end;

    /* Where to find details related to the current buffer we're reading from:
     * the buffer pointer itself, the current offset and the amount that is
     * allocated. These are all pointers back into this data structure. */
    char     **cur_read_buffer;
    MVMint32  *cur_read_offset;
    char     **cur_read_end;

    /* Various reading functions. */
    MVMint64    (*read_int)   (MVMThreadContext *tc, MVMSerializationReader *reader);
    MVMint64    (*read_varint)(MVMThreadContext *tc, MVMSerializationReader *reader);
    MVMnum64    (*read_num)   (MVMThreadContext *tc, MVMSerializationReader *reader);
    MVMString * (*read_str)   (MVMThreadContext *tc, MVMSerializationReader *reader);
    MVMObject * (*read_ref)   (MVMThreadContext *tc, MVMSerializationReader *reader);
    MVMSTable * (*read_stable_ref) (MVMThreadContext *tc, MVMSerializationReader *reader);

    /* List of code objects (static first, then all the closures). */
    MVMObject *codes_list;

    /* Array of contexts (num_contexts in length). */
    MVMFrame **contexts;

    /* The current object we're deserializing. */
    MVMObject *current_object;

    /* The data, which we'll want to free after deserialization. */
    char *data;
};

/* Represents the serialization writer and the various functions available
 * on it. */
struct MVMSerializationWriter {
    /* Serialization root data. */
    MVMSerializationRoot root;

    /* The code refs and contexts lists we're working through/adding to. */
    MVMObject  *codes_list;
    MVMObject  *contexts_list;

    /* Current position in the stables, objects and contexts lists. */
    MVMint64 stables_list_pos;
    MVMint64 objects_list_pos;
    MVMint64 contexts_list_pos;

    /* Hash of strings we've already seen while serializing to the index they
     * are placed at in the string heap. */
    MVMObject *seen_strings;

    /* Amount of memory allocated for various things. */
    MVMuint32 dependencies_table_alloc;
    MVMuint32 stables_table_alloc;
    MVMuint32 stables_data_alloc;
    MVMuint32 objects_table_alloc;
    MVMuint32 objects_data_alloc;
    MVMuint32 closures_table_alloc;
    MVMuint32 contexts_table_alloc;
    MVMuint32 contexts_data_alloc;
    MVMuint32 repos_table_alloc;

    /* Current offsets for the data chunks (also correspond to the amount of
     * data written in to them). */
    MVMuint32 stables_data_offset;
    MVMuint32 objects_data_offset;
    MVMuint32 contexts_data_offset;

    /* Where to find details related to the current buffer we're writing in
     * to: the buffer pointer itself, the current offset and the amount that
     * is allocated. These are all pointers back into this data structure. */
    char      **cur_write_buffer;
    MVMuint32  *cur_write_offset;
    MVMuint32  *cur_write_limit;

    /* Various writing functions. */
    void (*write_int) (MVMThreadContext *tc, MVMSerializationWriter *writer, MVMint64 value);
    void (*write_varint) (MVMThreadContext *tc, MVMSerializationWriter *writer, MVMint64 value);
    void (*write_num) (MVMThreadContext *tc, MVMSerializationWriter *writer, MVMnum64 value);
    void (*write_str) (MVMThreadContext *tc, MVMSerializationWriter *writer, MVMString *value);
    void (*write_ref) (MVMThreadContext *tc, MVMSerializationWriter *writer, MVMObject *value);
    void (*write_stable_ref) (MVMThreadContext *tc, MVMSerializationWriter *writer, MVMSTable *st);
};

/* Core serialize and deserialize functions. */
void MVM_serialization_deserialize(MVMThreadContext *tc, MVMSerializationContext *sc,
    MVMObject *string_heap, MVMObject *codes_static, MVMObject *repo_conflicts,
    MVMString *data);
MVMString * MVM_sha1(MVMThreadContext *tc, MVMString *str);
MVMString * MVM_serialization_serialize(MVMThreadContext *tc, MVMSerializationContext *sc,
    MVMObject *empty_string_heap);
