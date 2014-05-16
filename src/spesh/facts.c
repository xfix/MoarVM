#include "moar.h"

/* The code in this file walks the spesh graph, recording facts we discover
 * about each version of each local variable, and propagating the info as it
 * can. */

/* Copies facts from one var to another. */
static void copy_facts(MVMThreadContext *tc, MVMSpeshGraph *g, MVMuint16 to_orig,
                       MVMuint16 to_i, MVMuint16 from_orig, MVMuint16 from_i) {
    MVMSpeshFacts *tfacts = &g->facts[to_orig][to_i];
    MVMSpeshFacts *ffacts = &g->facts[from_orig][from_i];
    tfacts->flags         = ffacts->flags;
    tfacts->type          = ffacts->type;
    tfacts->decont_type   = ffacts->decont_type;
    tfacts->value         = ffacts->value;
}

/* Handles object-creating instructions. */
static void create_facts(MVMThreadContext *tc, MVMSpeshGraph *g, MVMuint16 obj_orig,
                         MVMuint16 obj_i, MVMuint16 type_orig, MVMuint16 type_i) {
    /* The type is carried. */
    if (g->facts[type_orig][type_i].flags & MVM_SPESH_FACT_KNOWN_TYPE) {
        g->facts[obj_orig][obj_i].type   = g->facts[type_orig][type_i].type;
        g->facts[obj_orig][obj_i].flags |= MVM_SPESH_FACT_KNOWN_TYPE;
    }

    /* We know it's a concrete object. */
    g->facts[obj_orig][obj_i].flags |= MVM_SPESH_FACT_CONCRETE;

    /* If we know the original value, then we can check the type to see if
     * it's a container type. */
    if (g->facts[type_orig][type_i].flags & MVM_SPESH_FACT_KNOWN_VALUE) {
        MVMObject *value = g->facts[type_orig][type_i].value.o;
        if (value && !STABLE(value)->container_spec)
            g->facts[obj_orig][obj_i].flags |= MVM_SPESH_FACT_DECONTED;
    }
}

/* Adds facts from knowing the exact value being put into an object local. */
static void object_facts(MVMThreadContext *tc, MVMSpeshGraph *g, MVMuint16 tgt_orig,
                         MVMuint16 tgt_i, MVMObject *obj) {
    /* Ensure it's non-null. */
    if (!obj)
        return;

    /* Set the value itself. */
    g->facts[tgt_orig][tgt_i].value.o  = obj;
    g->facts[tgt_orig][tgt_i].flags   |= MVM_SPESH_FACT_KNOWN_VALUE;

    /* We also know the type. */
    g->facts[tgt_orig][tgt_i].type   = STABLE(obj)->WHAT;
    g->facts[tgt_orig][tgt_i].flags |= MVM_SPESH_FACT_KNOWN_TYPE;

    /* Set concreteness and decontainerized flags. */
    if (IS_CONCRETE(obj)) {
        g->facts[tgt_orig][tgt_i].flags |= MVM_SPESH_FACT_CONCRETE;
        if (!STABLE(obj)->container_spec)
            g->facts[tgt_orig][tgt_i].flags |= MVM_SPESH_FACT_DECONTED;
    }
    else {
        g->facts[tgt_orig][tgt_i].flags |= MVM_SPESH_FACT_TYPEOBJ | MVM_SPESH_FACT_DECONTED;
    }
}

/* Propagates information relating to decontainerization. */
static void decont_facts(MVMThreadContext *tc, MVMSpeshGraph *g, MVMuint16 out_orig,
                         MVMuint16 out_i, MVMuint16 in_orig, MVMuint16 in_i) {
    /* If we know the original is decontainerized already, just copy its
     * info. */
    MVMint32 in_flags = g->facts[in_orig][in_i].flags;
    if (in_flags & MVM_SPESH_FACT_DECONTED)
        copy_facts(tc, g, out_orig, out_i, in_orig, in_i);

    /* We know the result is decontainerized. */
    g->facts[out_orig][out_i].flags |= MVM_SPESH_FACT_DECONTED;

    /* We may also know the original was containerized, and have some facts
     * about its contents. */
    if (in_flags & MVM_SPESH_FACT_KNOWN_DECONT_TYPE) {
        g->facts[out_orig][out_i].type = g->facts[in_orig][in_i].decont_type;
        g->facts[out_orig][out_i].flags |= MVM_SPESH_FACT_KNOWN_TYPE;
    }
    if (in_flags & MVM_SPESH_FACT_DECONT_CONCRETE)
        g->facts[out_orig][out_i].flags |= MVM_SPESH_FACT_CONCRETE;
    else if (in_flags & MVM_SPESH_FACT_DECONT_TYPEOBJ)
        g->facts[out_orig][out_i].flags |= MVM_SPESH_FACT_TYPEOBJ;
}

/* Looks up a wval and adds information based on it. */
static void wval_facts(MVMThreadContext *tc, MVMSpeshGraph *g, MVMuint16 tgt_orig,
                       MVMuint16 tgt_i, MVMuint16 dep, MVMint64 idx) {
    MVMCompUnit *cu = g->sf->body.cu;
    if (dep >= 0 && dep < cu->body.num_scs) {
        MVMSerializationContext *sc = MVM_sc_get_sc(tc, cu, dep);
        if (sc)
            object_facts(tc, g, tgt_orig, tgt_i, MVM_sc_get_object(tc, sc, idx));
    }
}

/* constant ops on literals give us a specialize-time-known value */
static void literal_facts(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshIns *ins) {
    MVMSpeshFacts *tgt_facts = &g->facts[ins->operands[0].reg.orig][ins->operands[0].reg.i];
    switch (ins->info->opcode) {
        case MVM_OP_const_i64:
            tgt_facts->value.i64 = ins->operands[1].lit_i64;
            break;
        case MVM_OP_const_i32:
            tgt_facts->value.i32 = ins->operands[1].lit_i32;
            break;
        case MVM_OP_const_i16:
            tgt_facts->value.i32 = ins->operands[1].lit_i16;
            break;
        case MVM_OP_const_i8:
            tgt_facts->value.i32 = ins->operands[1].lit_i8;
            break;
        case MVM_OP_const_n32:
            tgt_facts->value.n32 = ins->operands[1].lit_n32;
            break;
        case MVM_OP_const_n64:
            tgt_facts->value.n64 = ins->operands[1].lit_n64;
            break;
        case MVM_OP_const_i64_32:
            tgt_facts->value.i64 = ins->operands[1].lit_i32;
            break;
        case MVM_OP_const_i64_16:
            tgt_facts->value.i64 = ins->operands[1].lit_i16;
            break;
        case MVM_OP_const_s:
            tgt_facts->value.s   = g->sf->body.cu->body.strings[ins->operands[1].lit_str_idx];
            break;
        default:
            return;
    }
    tgt_facts->flags |= MVM_SPESH_FACT_KNOWN_VALUE;
}

/* Check for stability of what was logged, and if it looks sane then add facts
 * and turn the log instruction into a  */
static void log_facts(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshIns *ins) {
    MVMObject *stable_value = NULL;
    MVMObject *stable_cont  = NULL;

    /* See if all the recorded facts match up; a NULL means there was a code
     * path that never reached making a log entry. */
    MVMuint16 log_start = ins->operands[1].lit_i16 * MVM_SPESH_LOG_RUNS;
    MVMuint16 i;
    for (i = log_start; i < log_start + MVM_SPESH_LOG_RUNS; i++) {
        MVMObject *consider = (MVMObject *)g->log_slots[i];
        if (consider) {
            if (!stable_value) {
                stable_value = consider;
            }
            else if (STABLE(stable_value) != STABLE(consider)
                    || IS_CONCRETE(stable_value) != IS_CONCRETE(consider)) {
                stable_value = NULL;
                break;
            }
        }
    }
    if (!stable_value)
        return;

    /* If the value is a container type, need to look inside of it. */
    if (STABLE(stable_value)->container_spec && IS_CONCRETE(stable_value)) {
        MVMContainerSpec const *contspec = STABLE(stable_value)->container_spec;
        if (!contspec->fetch_never_invokes)
            return;
        stable_cont  = stable_value;
        stable_value = NULL;
        for (i = log_start; i < log_start + MVM_SPESH_LOG_RUNS; i++) {
            MVMRegister r;
            contspec->fetch(tc, stable_cont, &r);
            if (r.o) {
                if (!stable_value) {
                    stable_value = r.o;
                }
                else if (STABLE(stable_value) != STABLE(r.o)
                        || IS_CONCRETE(stable_value) != IS_CONCRETE(r.o)) {
                    stable_value = NULL;
                    break;
                }
            }
        }
        if (!stable_value)
            return;
    }

    /* Produce a guard op and set facts. */
    if (stable_cont) {
        MVMSpeshOperand reg   = ins->operands[0];
        MVMSpeshFacts  *facts = &g->facts[reg.reg.orig][reg.reg.i];
        facts->type           = STABLE(stable_cont)->WHAT;
        facts->flags         |= (MVM_SPESH_FACT_KNOWN_TYPE | MVM_SPESH_FACT_CONCRETE |
                                MVM_SPESH_FACT_KNOWN_DECONT_TYPE);
        facts->decont_type    = STABLE(stable_value)->WHAT;
        if (IS_CONCRETE(stable_value)) {
            facts->flags |= MVM_SPESH_FACT_DECONT_CONCRETE;
            ins->info = MVM_op_get_op(MVM_OP_sp_guardcontconc);
        }
        else {
            facts->flags |= MVM_SPESH_FACT_DECONT_TYPEOBJ;
            ins->info = MVM_op_get_op(MVM_OP_sp_guardconttype);
        }
        ins->operands = MVM_spesh_alloc(tc, g, 3 * sizeof(MVMSpeshOperand));
        ins->operands[0] = reg;
        ins->operands[1].lit_i16 = MVM_spesh_add_spesh_slot(tc, g, (MVMCollectable *)STABLE(stable_cont));
        ins->operands[2].lit_i16 = MVM_spesh_add_spesh_slot(tc, g, (MVMCollectable *)STABLE(stable_value));
    }
    else {
        MVMSpeshFacts *facts = &g->facts[ins->operands[0].reg.orig][ins->operands[0].reg.i];
        facts->type          = STABLE(stable_value)->WHAT;
        facts->flags        |= (MVM_SPESH_FACT_KNOWN_TYPE | MVM_SPESH_FACT_DECONTED);
        if (IS_CONCRETE(stable_value)) {
            facts->flags |= MVM_SPESH_FACT_CONCRETE;
            ins->info = MVM_op_get_op(MVM_OP_sp_guardconc);
        }
        else {
            facts->flags |= MVM_SPESH_FACT_TYPEOBJ;
            ins->info = MVM_op_get_op(MVM_OP_sp_guardtype);
        }
        ins->operands[1].lit_i16 = MVM_spesh_add_spesh_slot(tc, g, (MVMCollectable *)STABLE(stable_value));
    }
}

/* Visits the blocks in dominator tree order, recursively. */
static void add_bb_facts(MVMThreadContext *tc, MVMSpeshGraph *g, MVMSpeshBB *bb) {
    MVMint32 i;

    /* Look for instructions that provide or propagate facts. */
    MVMSpeshIns *ins = bb->first_ins;
    while (ins) {
        /* Look through operands for usages and record them. */
        MVMint32 is_phi = ins->info->opcode == MVM_SSA_PHI;
        for (i = 0; i < ins->info->num_operands; i++)
            if (is_phi && i > 0 || !is_phi &&
                (ins->info->operands[i] & MVM_operand_rw_mask) == MVM_operand_read_reg)
                g->facts[ins->operands[i].reg.orig][ins->operands[i].reg.i].usages++;

        /* Look for ops that are fact-interesting. */
        switch (ins->info->opcode) {
        case MVM_OP_inc_i:
        case MVM_OP_inc_u:
        case MVM_OP_dec_i:
        case MVM_OP_dec_u:
            /* These all read as well as write a value, so bump usages. */
            g->facts[ins->operands[0].reg.orig][ins->operands[0].reg.i - 1].usages++;
            break;
        case MVM_OP_set:
            copy_facts(tc, g,
                ins->operands[0].reg.orig, ins->operands[0].reg.i,
                ins->operands[1].reg.orig, ins->operands[1].reg.i);
            break;
        case MVM_OP_create:
            create_facts(tc, g,
                ins->operands[0].reg.orig, ins->operands[0].reg.i,
                ins->operands[1].reg.orig, ins->operands[1].reg.i);
            break;
        case MVM_OP_box_s:
        case MVM_OP_box_i:
        case MVM_OP_box_n:
            create_facts(tc, g,
                ins->operands[0].reg.orig, ins->operands[0].reg.i,
                ins->operands[2].reg.orig, ins->operands[2].reg.i);
            break;
        case MVM_OP_bootint:
            object_facts(tc, g,
                ins->operands[0].reg.orig, ins->operands[0].reg.i,
                tc->instance->boot_types.BOOTInt);
            break;
        case MVM_OP_bootnum:
            object_facts(tc, g,
                ins->operands[0].reg.orig, ins->operands[0].reg.i,
                tc->instance->boot_types.BOOTNum);
            break;
        case MVM_OP_bootstr:
            object_facts(tc, g,
                ins->operands[0].reg.orig, ins->operands[0].reg.i,
                tc->instance->boot_types.BOOTStr);
            break;
        case MVM_OP_bootarray:
            object_facts(tc, g,
                ins->operands[0].reg.orig, ins->operands[0].reg.i,
                tc->instance->boot_types.BOOTArray);
            break;
        case MVM_OP_bootintarray:
            object_facts(tc, g,
                ins->operands[0].reg.orig, ins->operands[0].reg.i,
                tc->instance->boot_types.BOOTIntArray);
            break;
        case MVM_OP_bootnumarray:
            object_facts(tc, g,
                ins->operands[0].reg.orig, ins->operands[0].reg.i,
                tc->instance->boot_types.BOOTNumArray);
            break;
        case MVM_OP_bootstrarray:
            object_facts(tc, g,
                ins->operands[0].reg.orig, ins->operands[0].reg.i,
                tc->instance->boot_types.BOOTStrArray);
            break;
        case MVM_OP_boothash:
            object_facts(tc, g,
                ins->operands[0].reg.orig, ins->operands[0].reg.i,
                tc->instance->boot_types.BOOTHash);
            break;
        case MVM_OP_hllboxtype_i:
            object_facts(tc, g,
                ins->operands[0].reg.orig, ins->operands[0].reg.i,
                g->sf->body.cu->body.hll_config->int_box_type);
            break;
        case MVM_OP_hllboxtype_n:
            object_facts(tc, g,
                ins->operands[0].reg.orig, ins->operands[0].reg.i,
                g->sf->body.cu->body.hll_config->num_box_type);
            break;
        case MVM_OP_hllboxtype_s:
            object_facts(tc, g,
                ins->operands[0].reg.orig, ins->operands[0].reg.i,
                g->sf->body.cu->body.hll_config->str_box_type);
            break;
        case MVM_OP_hlllist:
            object_facts(tc, g,
                ins->operands[0].reg.orig, ins->operands[0].reg.i,
                g->sf->body.cu->body.hll_config->slurpy_array_type);
            break;
        case MVM_OP_hllhash:
            object_facts(tc, g,
                ins->operands[0].reg.orig, ins->operands[0].reg.i,
                g->sf->body.cu->body.hll_config->slurpy_hash_type);
            break;
        case MVM_OP_decont:
            decont_facts(tc, g,
                ins->operands[0].reg.orig, ins->operands[0].reg.i,
                ins->operands[1].reg.orig, ins->operands[1].reg.i);
            break;
        case MVM_OP_wval:
            wval_facts(tc, g,
                ins->operands[0].reg.orig, ins->operands[0].reg.i,
                ins->operands[1].lit_i16, ins->operands[2].lit_i16);
            break;
        case MVM_OP_wval_wide:
            wval_facts(tc, g,
                ins->operands[0].reg.orig, ins->operands[0].reg.i,
                ins->operands[1].lit_i16, ins->operands[2].lit_i64);
            break;
        case MVM_OP_const_i64:
        case MVM_OP_const_i32:
        case MVM_OP_const_i16:
        case MVM_OP_const_i8:
        case MVM_OP_const_n64:
        case MVM_OP_const_n32:
        case MVM_OP_const_i64_32:
        case MVM_OP_const_i64_16:
        case MVM_OP_const_s:
            literal_facts(tc, g, ins);
            break;
        case MVM_OP_sp_log: {
            MVMuint16 po = ins->prev->info->opcode;
            if (po != MVM_OP_getlexstatic_o && po != MVM_OP_getlexperinvtype_o)
                log_facts(tc, g, ins);
            break;
        }
        }
        ins = ins->next;
    }

    /* Visit children. */
    for (i = 0; i < bb->num_children; i++)
        add_bb_facts(tc, g, bb->children[i]);
}

/* Exception handlers that use a block to store the handler must not have the
 * instructions that install the block eliminated. This tweaks the usage of
 * them. */
void tweak_block_handler_usage(MVMThreadContext *tc, MVMSpeshGraph *g) {
    MVMint32 i;
    for (i = 0; i < g->sf->body.num_handlers; i++) {
        if (g->sf->body.handlers[i].action == MVM_EX_ACTION_INVOKE)
            g->facts[g->sf->body.handlers[i].block_reg][1].usages++;
    }
}

/* Kicks off fact discovery from the top of the (dominator) tree. */
void MVM_spesh_facts_discover(MVMThreadContext *tc, MVMSpeshGraph *g) {
    add_bb_facts(tc, g, g->entry);
    tweak_block_handler_usage(tc, g);
}
