/* Copyright (C) 2007-2025 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 *
 * signature parser
 */

#include "suricata-common.h"

#include "detect.h"
#include "detect-engine.h"
#include "detect-engine-address.h"
#include "detect-engine-port.h"
#include "detect-engine-mpm.h"
#include "detect-engine-state.h"
#include "detect-engine-build.h"

#include "detect-content.h"
#include "detect-bsize.h"
#include "detect-isdataat.h"
#include "detect-pcre.h"
#include "detect-uricontent.h"
#include "detect-reference.h"
#include "detect-ipproto.h"
#include "detect-flow.h"
#include "detect-app-layer-protocol.h"
#include "detect-lua.h"
#include "detect-app-layer-event.h"
#include "detect-http-method.h"

#include "pkt-var.h"
#include "host.h"
#include "util-profiling.h"
#include "decode.h"

#include "flow.h"

#include "util-rule-vars.h"
#include "conf.h"
#include "conf-yaml-loader.h"

#include "app-layer.h"
#include "app-layer-protos.h"
#include "app-layer-parser.h"
#include "app-layer-htp.h"

#include "util-classification-config.h"
#include "util-unittest.h"
#include "util-unittest-helper.h"
#include "util-debug.h"
#include "string.h"
#include "detect-parse.h"
#include "detect-engine-iponly.h"
#include "detect-engine-file.h"
#include "app-layer-detect-proto.h"

#include "action-globals.h"
#include "util-validate.h"

/* Table with all SigMatch registrations */
SigTableElmt *sigmatch_table = NULL;

extern bool sc_set_caps;

static void SigMatchTransferSigMatchAcrossLists(SigMatch *sm,
        SigMatch **src_sm_list, SigMatch **src_sm_list_tail,
        SigMatch **dst_sm_list, SigMatch **dst_sm_list_tail);

/**
 * \brief Registration table for file handlers
 */
/**
 * \brief We use this as data to the hash table DetectEngineCtx->dup_sig_hash_table.
 */
typedef struct SigDuplWrapper_ {
    /* the signature we want to wrap */
    Signature *s;
    /* the signature right before the above signature in the det_ctx->sig_list */
    Signature *s_prev;
} SigDuplWrapper;

/** helper structure for sig parsing */
typedef struct SignatureParser_ {
    char action[DETECT_MAX_RULE_SIZE];
    char protocol[DETECT_MAX_RULE_SIZE];
    char direction[DETECT_MAX_RULE_SIZE];
    char src[DETECT_MAX_RULE_SIZE];
    char dst[DETECT_MAX_RULE_SIZE];
    char sp[DETECT_MAX_RULE_SIZE];
    char dp[DETECT_MAX_RULE_SIZE];
    char opts[DETECT_MAX_RULE_SIZE];
} SignatureParser;

const char *DetectListToHumanString(int list)
{
#define CASE_CODE_STRING(E, S)  case E: return S; break
    switch (list) {
        CASE_CODE_STRING(DETECT_SM_LIST_MATCH, "packet");
        CASE_CODE_STRING(DETECT_SM_LIST_PMATCH, "payload");
        CASE_CODE_STRING(DETECT_SM_LIST_BASE64_DATA, "base64_data");
        CASE_CODE_STRING(DETECT_SM_LIST_POSTMATCH, "postmatch");
        CASE_CODE_STRING(DETECT_SM_LIST_TMATCH, "tag");
        CASE_CODE_STRING(DETECT_SM_LIST_SUPPRESS, "suppress");
        CASE_CODE_STRING(DETECT_SM_LIST_THRESHOLD, "threshold");
        CASE_CODE_STRING(DETECT_SM_LIST_MAX, "max (internal)");
    }
#undef CASE_CODE_STRING
    return "unknown";
}

#define CASE_CODE(E)  case E: return #E
const char *DetectListToString(int list)
{
    switch (list) {
        CASE_CODE(DETECT_SM_LIST_MATCH);
        CASE_CODE(DETECT_SM_LIST_PMATCH);
        CASE_CODE(DETECT_SM_LIST_BASE64_DATA);
        CASE_CODE(DETECT_SM_LIST_TMATCH);
        CASE_CODE(DETECT_SM_LIST_POSTMATCH);
        CASE_CODE(DETECT_SM_LIST_SUPPRESS);
        CASE_CODE(DETECT_SM_LIST_THRESHOLD);
        CASE_CODE(DETECT_SM_LIST_MAX);
    }
    return "unknown";
}

/** \param arg NULL or empty string */
int DetectEngineContentModifierBufferSetup(DetectEngineCtx *de_ctx,
        Signature *s, const char *arg, int sm_type, int sm_list,
        AppProto alproto)
{
    SigMatch *sm = NULL;
    int ret = -1;

    if (arg != NULL && strcmp(arg, "") != 0) {
        SCLogError("%s shouldn't be supplied "
                   "with an argument",
                sigmatch_table[sm_type].name);
        goto end;
    }

    if (s->init_data->list != DETECT_SM_LIST_NOTSET) {
        SCLogError("\"%s\" keyword seen "
                   "with a sticky buffer still set.  Reset sticky buffer "
                   "with pkt_data before using the modifier.",
                sigmatch_table[sm_type].name);
        goto end;
    }
    if (s->alproto != ALPROTO_UNKNOWN && !AppProtoEquals(s->alproto, alproto)) {
        SCLogError("rule contains conflicting "
                   "alprotos set");
        goto end;
    }

    sm = DetectGetLastSMByListId(s,
            DETECT_SM_LIST_PMATCH, DETECT_CONTENT, -1);
    if (sm == NULL) {
        SCLogError("\"%s\" keyword "
                   "found inside the rule without a content context.  "
                   "Please use a \"content\" keyword before using the "
                   "\"%s\" keyword",
                sigmatch_table[sm_type].name, sigmatch_table[sm_type].name);
        goto end;
    }
    DetectContentData *cd = (DetectContentData *)sm->ctx;
    if (cd->flags & DETECT_CONTENT_RAWBYTES) {
        SCLogError("%s rule can not "
                   "be used with the rawbytes rule keyword",
                sigmatch_table[sm_type].name);
        goto end;
    }
    if (cd->flags & DETECT_CONTENT_REPLACE) {
        SCLogError("%s rule can not "
                   "be used with the replace rule keyword",
                sigmatch_table[sm_type].name);
        goto end;
    }
    if (cd->flags & (DETECT_CONTENT_WITHIN | DETECT_CONTENT_DISTANCE)) {
        SigMatch *pm = DetectGetLastSMByListPtr(s, sm->prev,
            DETECT_CONTENT, DETECT_PCRE, -1);
        if (pm != NULL) {
            if (pm->type == DETECT_CONTENT) {
                DetectContentData *tmp_cd = (DetectContentData *)pm->ctx;
                tmp_cd->flags &= ~DETECT_CONTENT_RELATIVE_NEXT;
            } else {
                DetectPcreData *tmp_pd = (DetectPcreData *)pm->ctx;
                tmp_pd->flags &= ~DETECT_PCRE_RELATIVE_NEXT;
            }
        }

        if (s->init_data->curbuf != NULL && (int)s->init_data->curbuf->id == sm_list) {
            pm = DetectGetLastSMByListPtr(
                    s, s->init_data->curbuf->tail, DETECT_CONTENT, DETECT_PCRE, -1);
            if (pm != NULL) {
                if (pm->type == DETECT_CONTENT) {
                    DetectContentData *tmp_cd = (DetectContentData *)pm->ctx;
                    tmp_cd->flags |= DETECT_CONTENT_RELATIVE_NEXT;
                } else {
                    DetectPcreData *tmp_pd = (DetectPcreData *)pm->ctx;
                    tmp_pd->flags |= DETECT_PCRE_RELATIVE_NEXT;
                }
            }
        }
    }
    s->alproto = alproto;
    s->flags |= SIG_FLAG_APPLAYER;

    if (s->init_data->curbuf == NULL || (int)s->init_data->curbuf->id != sm_list) {
        if (s->init_data->curbuf != NULL && s->init_data->curbuf->head == NULL) {
            SCLogError("no matches for previous buffer");
            return -1;
        }
        bool reuse_buffer = false;
        if (s->init_data->curbuf != NULL && (int)s->init_data->curbuf->id != sm_list) {
            for (uint32_t x = 0; x < s->init_data->buffer_index; x++) {
                if (s->init_data->buffers[x].id == (uint32_t)sm_list) {
                    s->init_data->curbuf = &s->init_data->buffers[x];
                    reuse_buffer = true;
                    break;
                }
            }
        }

        if (!reuse_buffer) {
            if (SignatureInitDataBufferCheckExpand(s) < 0) {
                SCLogError("failed to expand rule buffer array");
                return -1;
            }

            /* initialize a new buffer */
            s->init_data->curbuf = &s->init_data->buffers[s->init_data->buffer_index++];
            s->init_data->curbuf->id = sm_list;
            s->init_data->curbuf->head = NULL;
            s->init_data->curbuf->tail = NULL;
            SCLogDebug("idx %u list %d set up curbuf %p s->init_data->buffer_index %u",
                    s->init_data->buffer_index - 1, sm_list, s->init_data->curbuf,
                    s->init_data->buffer_index);
        }
    }

    /* transfer the sm from the pmatch list to sm_list */
    SigMatchTransferSigMatchAcrossLists(sm, &s->init_data->smlists[DETECT_SM_LIST_PMATCH],
            &s->init_data->smlists_tail[DETECT_SM_LIST_PMATCH], &s->init_data->curbuf->head,
            &s->init_data->curbuf->tail);

    if (sm->type == DETECT_CONTENT) {
        s->init_data->max_content_list_id =
                MAX(s->init_data->max_content_list_id, (uint32_t)sm_list);
    }

    ret = 0;
 end:
    return ret;
}

SigMatch *SigMatchAlloc(void)
{
    SigMatch *sm = SCCalloc(1, sizeof(SigMatch));
    if (unlikely(sm == NULL))
        return NULL;

    sm->prev = NULL;
    sm->next = NULL;
    return sm;
}

/** \brief free a SigMatch
 *  \param sm SigMatch to free.
 */
void SigMatchFree(DetectEngineCtx *de_ctx, SigMatch *sm)
{
    if (sm == NULL)
        return;

    /** free the ctx, for that we call the Free func */
    if (sm->ctx != NULL) {
        if (sigmatch_table[sm->type].Free != NULL) {
            sigmatch_table[sm->type].Free(de_ctx, sm->ctx);
        }
    }
    SCFree(sm);
}

static enum DetectKeywordId SigTableGetIndex(const SigTableElmt *e)
{
    const SigTableElmt *table = &sigmatch_table[0];
    ptrdiff_t offset = e - table;
    BUG_ON(offset >= DETECT_TBLSIZE);
    return (enum DetectKeywordId)offset;
}

/* Get the detection module by name */
static SigTableElmt *SigTableGet(char *name)
{
    SigTableElmt *st = NULL;
    int i = 0;

    for (i = 0; i < DETECT_TBLSIZE; i++) {
        st = &sigmatch_table[i];

        if (st->name != NULL) {
            if (strcasecmp(name,st->name) == 0)
                return st;
            if (st->alias != NULL && strcasecmp(name,st->alias) == 0)
                return st;
        }
    }

    return NULL;
}

bool SigMatchSilentErrorEnabled(const DetectEngineCtx *de_ctx,
        const enum DetectKeywordId id)
{
    return de_ctx->sm_types_silent_error[id];
}

bool SigMatchStrictEnabled(const enum DetectKeywordId id)
{
    if ((int)id < DETECT_TBLSIZE) {
        return ((sigmatch_table[id].flags & SIGMATCH_STRICT_PARSING) != 0);
    }
    return false;
}

void SigTableApplyStrictCommandLineOption(const char *str)
{
    if (str == NULL) {
        /* nothing to be done */
        return;
    }

    /* "all" just sets the flag for each keyword */
    if (strcmp(str, "all") == 0) {
        for (int i = 0; i < DETECT_TBLSIZE; i++) {
            SigTableElmt *st = &sigmatch_table[i];
            st->flags |= SIGMATCH_STRICT_PARSING;
        }
        return;
    }

    char *copy = SCStrdup(str);
    if (copy == NULL)
        FatalError("could not duplicate opt string");

    char *xsaveptr = NULL;
    char *key = strtok_r(copy, ",", &xsaveptr);
    while (key != NULL) {
        SigTableElmt *st = SigTableGet(key);
        if (st != NULL) {
            st->flags |= SIGMATCH_STRICT_PARSING;
        } else {
            SCLogWarning("'strict' command line "
                         "argument '%s' not found",
                    key);
        }
        key = strtok_r(NULL, ",", &xsaveptr);
    }

    SCFree(copy);
}

/**
 * \brief Append a SigMatch to the list type.
 *
 * \param s    Signature.
 * \param new  The sig match to append.
 * \param list The list to append to.
 */
SigMatch *SCSigMatchAppendSMToList(
        DetectEngineCtx *de_ctx, Signature *s, uint16_t type, SigMatchCtx *ctx, const int list)
{
    SigMatch *new = SigMatchAlloc();
    if (new == NULL)
        return NULL;

    new->type = type;
    new->ctx = ctx;

    if (new->type == DETECT_CONTENT) {
        s->init_data->max_content_list_id = MAX(s->init_data->max_content_list_id, (uint32_t)list);
    }

    SCLogDebug("s:%p new:%p list:%d: %s, s->init_data->list_set %s s->init_data->list %d", s, new,
            list, sigmatch_table[new->type].name, BOOL2STR(s->init_data->list_set),
            s->init_data->list);

    if (list < DETECT_SM_LIST_MAX) {
        if (s->init_data->smlists[list] == NULL) {
            s->init_data->smlists[list] = new;
            s->init_data->smlists_tail[list] = new;
            new->next = NULL;
            new->prev = NULL;
        } else {
            SigMatch *cur = s->init_data->smlists_tail[list];
            cur->next = new;
            new->prev = cur;
            new->next = NULL;
            s->init_data->smlists_tail[list] = new;
        }
        new->idx = s->init_data->sm_cnt;
        s->init_data->sm_cnt++;

    } else {
        /* app-layer-events (and possibly others?) can get here w/o a "list"
         * already set up. */

        /* unset any existing list if it isn't the same as the new */
        if (s->init_data->list != DETECT_SM_LIST_NOTSET && list != s->init_data->list) {
            SCLogDebug("reset: list %d != s->init_data->list %d", list, s->init_data->list);
            s->init_data->list = DETECT_SM_LIST_NOTSET;
        }

        if (s->init_data->curbuf != NULL && (int)s->init_data->curbuf->id != list) {
            for (uint32_t x = 0; x < s->init_data->buffer_index; x++) {
                if (s->init_data->buffers[x].id == (uint32_t)list &&
                        !s->init_data->buffers[x].multi_capable) {
                    SCLogDebug("reusing buffer %u as it isn't multi-capable", x);
                    s->init_data->curbuf = &s->init_data->buffers[x];
                    break;
                }
            }
        }

        if ((s->init_data->curbuf != NULL && (int)s->init_data->curbuf->id != list) ||
                s->init_data->curbuf == NULL) {
            if (SignatureInitDataBufferCheckExpand(s) < 0) {
                SCLogError("failed to expand rule buffer array");
                new->ctx = NULL;
                SigMatchFree(de_ctx, new);
                return NULL;
            } else {
                /* initialize new buffer */
                s->init_data->curbuf = &s->init_data->buffers[s->init_data->buffer_index++];
                s->init_data->curbuf->id = list;
                /* buffer set up by sigmatch is tracked in case we add a stickybuffer for the
                 * same list. */
                s->init_data->curbuf->sm_init = true;
                if (s->init_data->init_flags & SIG_FLAG_INIT_FORCE_TOCLIENT) {
                    s->init_data->curbuf->only_tc = true;
                }
                if (s->init_data->init_flags & SIG_FLAG_INIT_FORCE_TOSERVER) {
                    s->init_data->curbuf->only_ts = true;
                }
                SCLogDebug("s->init_data->buffer_index %u", s->init_data->buffer_index);
            }
        }
        BUG_ON(s->init_data->curbuf == NULL);

        new->prev = s->init_data->curbuf->tail;
        if (s->init_data->curbuf->tail)
            s->init_data->curbuf->tail->next = new;
        if (s->init_data->curbuf->head == NULL)
            s->init_data->curbuf->head = new;
        s->init_data->curbuf->tail = new;
        new->idx = s->init_data->sm_cnt;
        s->init_data->sm_cnt++;
        SCLogDebug("appended %s to list %d, rule pos %u (s->init_data->list %d)",
                sigmatch_table[new->type].name, list, new->idx, s->init_data->list);

        for (SigMatch *sm = s->init_data->curbuf->head; sm != NULL; sm = sm->next) {
            SCLogDebug("buf:%p: id:%u: '%s' pos %u", s->init_data->curbuf, s->init_data->curbuf->id,
                    sigmatch_table[sm->type].name, sm->idx);
        }
    }
    return new;
}

void SigMatchRemoveSMFromList(Signature *s, SigMatch *sm, int sm_list)
{
    if (sm == s->init_data->smlists[sm_list]) {
        s->init_data->smlists[sm_list] = sm->next;
    }
    if (sm == s->init_data->smlists_tail[sm_list]) {
        s->init_data->smlists_tail[sm_list] = sm->prev;
    }
    if (sm->prev != NULL)
        sm->prev->next = sm->next;
    if (sm->next != NULL)
        sm->next->prev = sm->prev;
}

/**
 * \brief Returns a pointer to the last SigMatch instance of a particular type
 *        in a Signature of the payload list.
 *
 * \param s    Pointer to the tail of the sigmatch list
 * \param type SigMatch type which has to be searched for in the Signature.
 *
 * \retval match Pointer to the last SigMatch instance of type 'type'.
 */
static SigMatch *SigMatchGetLastSMByType(SigMatch *sm, int type)
{
    while (sm != NULL) {
        if (sm->type == type) {
            return sm;
        }
        sm = sm->prev;
    }

    return NULL;
}

/** \brief get the last SigMatch from lists that support
 *         MPM.
 *  \note only supports the lists that are registered through
 *        DetectBufferTypeSupportsMpm().
 */
SigMatch *DetectGetLastSMFromMpmLists(const DetectEngineCtx *de_ctx, const Signature *s)
{
    SigMatch *sm_last = NULL;
    SigMatch *sm_new;
    uint32_t sm_type;

    for (uint32_t i = 0; i < s->init_data->buffer_index; i++) {
        const int id = s->init_data->buffers[i].id;
        if (DetectEngineBufferTypeSupportsMpmGetById(de_ctx, id)) {
            sm_new = DetectGetLastSMByListPtr(s, s->init_data->buffers[i].tail, DETECT_CONTENT, -1);
            if (sm_new == NULL)
                continue;
            if (sm_last == NULL || sm_new->idx > sm_last->idx)
                sm_last = sm_new;
        }
    }
    /* otherwise brute force it */
    for (sm_type = 0; sm_type < DETECT_SM_LIST_MAX; sm_type++) {
        if (!DetectEngineBufferTypeSupportsMpmGetById(de_ctx, sm_type))
            continue;
        SigMatch *sm_list = s->init_data->smlists_tail[sm_type];
        sm_new = SigMatchGetLastSMByType(sm_list, DETECT_CONTENT);
        if (sm_new == NULL)
            continue;
        if (sm_last == NULL || sm_new->idx > sm_last->idx)
            sm_last = sm_new;
    }

    return sm_last;
}

/**
 * \brief Returns the sm with the largest index (added latest) from the lists
 *        passed to us.
 *
 * \retval Pointer to Last sm.
 */
SigMatch *DetectGetLastSMFromLists(const Signature *s, ...)
{
    SigMatch *sm_last = NULL;
    SigMatch *sm_new;

    SCLogDebug("s->init_data->buffer_index %u", s->init_data->buffer_index);
    for (uint32_t x = 0; x < s->init_data->buffer_index; x++) {
        if (s->init_data->list != DETECT_SM_LIST_NOTSET &&
                s->init_data->list != (int)s->init_data->buffers[x].id) {
            SCLogDebug("skip x %u s->init_data->list %d (int)s->init_data->buffers[x].id %d", x,
                    s->init_data->list, (int)s->init_data->buffers[x].id);

            continue;
        }
        int sm_type;
        va_list ap;
        va_start(ap, s);

        for (sm_type = va_arg(ap, int); sm_type != -1; sm_type = va_arg(ap, int)) {
            sm_new = SigMatchGetLastSMByType(s->init_data->buffers[x].tail, sm_type);
            if (sm_new == NULL)
                continue;
            if (sm_last == NULL || sm_new->idx > sm_last->idx)
                sm_last = sm_new;
        }
        va_end(ap);
    }

    for (int buf_type = 0; buf_type < DETECT_SM_LIST_MAX; buf_type++) {
        if (s->init_data->smlists[buf_type] == NULL)
            continue;
        if (s->init_data->list != DETECT_SM_LIST_NOTSET &&
            buf_type != s->init_data->list)
            continue;

        int sm_type;
        va_list ap;
        va_start(ap, s);

        for (sm_type = va_arg(ap, int); sm_type != -1; sm_type = va_arg(ap, int))
        {
            sm_new = SigMatchGetLastSMByType(s->init_data->smlists_tail[buf_type], sm_type);
            if (sm_new == NULL)
                continue;
            if (sm_last == NULL || sm_new->idx > sm_last->idx)
                sm_last = sm_new;
        }
        va_end(ap);
    }

    return sm_last;
}

/**
 * \brief Returns the sm with the largest index (added last) from the list
 *        passed to us as a pointer.
 *
 * \param sm_list pointer to the SigMatch we should look before
 * \param va_args list of keyword types terminated by -1
 *
 * \retval sm_last to last sm.
 */
SigMatch *DetectGetLastSMByListPtr(const Signature *s, SigMatch *sm_list, ...)
{
    SigMatch *sm_last = NULL;
    SigMatch *sm_new;
    int sm_type;

    va_list ap;
    va_start(ap, sm_list);

    for (sm_type = va_arg(ap, int); sm_type != -1; sm_type = va_arg(ap, int))
    {
        sm_new = SigMatchGetLastSMByType(sm_list, sm_type);
        if (sm_new == NULL)
            continue;
        if (sm_last == NULL || sm_new->idx > sm_last->idx)
            sm_last = sm_new;
    }

    va_end(ap);

    return sm_last;
}

/**
 * \brief Returns the sm with the largest index (added last) from the list
 *        passed to us as an id.
 *
 * \param list_id id of the list to be searched
 * \param va_args list of keyword types terminated by -1
 *
 * \retval sm_last to last sm.
 */
SigMatch *DetectGetLastSMByListId(const Signature *s, int list_id, ...)
{
    SigMatch *sm_last = NULL;
    SigMatch *sm_new;
    int sm_type;

    if ((uint32_t)list_id >= DETECT_SM_LIST_MAX) {
        for (uint32_t x = 0; x < s->init_data->buffer_index; x++) {
            sm_new = s->init_data->buffers[x].tail;
            if (sm_new == NULL)
                continue;

            va_list ap;
            va_start(ap, list_id);

            for (sm_type = va_arg(ap, int); sm_type != -1; sm_type = va_arg(ap, int)) {
                sm_new = SigMatchGetLastSMByType(s->init_data->buffers[x].tail, sm_type);
                if (sm_new == NULL)
                    continue;
                if (sm_last == NULL || sm_new->idx > sm_last->idx)
                    sm_last = sm_new;
            }

            va_end(ap);
        }
    } else {
        SigMatch *sm_list = s->init_data->smlists_tail[list_id];
        if (sm_list == NULL)
            return NULL;

        va_list ap;
        va_start(ap, list_id);

        for (sm_type = va_arg(ap, int); sm_type != -1; sm_type = va_arg(ap, int)) {
            sm_new = SigMatchGetLastSMByType(sm_list, sm_type);
            if (sm_new == NULL)
                continue;
            if (sm_last == NULL || sm_new->idx > sm_last->idx)
                sm_last = sm_new;
        }

        va_end(ap);
    }
    return sm_last;
}

/**
 * \brief Returns the sm with the largest index (added latest) from this sig
 *
 * \retval sm_last Pointer to last sm
 */
SigMatch *DetectGetLastSM(const Signature *s)
{
    SigMatch *sm_last = NULL;
    SigMatch *sm_new;

    for (uint32_t x = 0; x < s->init_data->buffer_index; x++) {
        sm_new = s->init_data->buffers[x].tail;
        if (sm_new == NULL)
            continue;
        if (sm_last == NULL || sm_new->idx > sm_last->idx)
            sm_last = sm_new;
    }

    for (int i = 0; i < DETECT_SM_LIST_MAX; i++) {
        sm_new = s->init_data->smlists_tail[i];
        if (sm_new == NULL)
            continue;
        if (sm_last == NULL || sm_new->idx > sm_last->idx)
            sm_last = sm_new;
    }

    return sm_last;
}

static void SigMatchTransferSigMatchAcrossLists(SigMatch *sm,
        SigMatch **src_sm_list, SigMatch **src_sm_list_tail,
        SigMatch **dst_sm_list, SigMatch **dst_sm_list_tail)
{
    /* we won't do any checks for args */

    if (sm->prev != NULL)
        sm->prev->next = sm->next;
    if (sm->next != NULL)
        sm->next->prev = sm->prev;

    if (sm == *src_sm_list)
        *src_sm_list = sm->next;
    if (sm == *src_sm_list_tail)
        *src_sm_list_tail = sm->prev;

    if (*dst_sm_list == NULL) {
        *dst_sm_list = sm;
        *dst_sm_list_tail = sm;
        sm->next = NULL;
        sm->prev = NULL;
    } else {
        SigMatch *cur = *dst_sm_list_tail;
        cur->next = sm;
        sm->prev = cur;
        sm->next = NULL;
        *dst_sm_list_tail = sm;
    }
}

int SigMatchListSMBelongsTo(const Signature *s, const SigMatch *key_sm)
{
    if (key_sm == NULL)
        return -1;

    for (uint32_t x = 0; x < s->init_data->buffer_index; x++) {
        const SigMatch *sm = s->init_data->buffers[x].head;
        while (sm != NULL) {
            if (sm == key_sm)
                return s->init_data->buffers[x].id;
            sm = sm->next;
        }
    }

    for (int list = 0; list < DETECT_SM_LIST_MAX; list++) {
        const SigMatch *sm = s->init_data->smlists[list];
        while (sm != NULL) {
            if (sm == key_sm)
                return list;
            sm = sm->next;
        }
    }

    SCLogError("Unable to find the sm in any of the "
               "sm lists");
    return -1;
}

/**
 * \brief Parse and setup a direction
 *
 * \param s signature
 * \param str argument to the keyword
 * \param only_dir argument wether the keyword only accepts a direction
 *
 * \retval 0 on success, -1 on failure
 */
static int DetectSetupDirection(Signature *s, char **str, bool only_dir)
{
    char *orig = *str;
    if (strncmp(*str, "to_client", strlen("to_client")) == 0) {
        *str += strlen("to_client");
        // skip space
        while (**str && isblank(**str)) {
            (*str)++;
        }
        // check comma or nothing
        if (**str) {
            if (only_dir) {
                SCLogError("unknown option: only accepts to_server or to_client");
                return -1;
            }
            if (**str != ',') {
                // leave to_client_something for next parser if not only_dir
                *str = orig;
                return 0;
            } else {
                (*str)++;
            }
            while (**str && isblank(**str)) {
                (*str)++;
            }
        }
        s->init_data->init_flags |= SIG_FLAG_INIT_FORCE_TOCLIENT;
        if ((s->flags & SIG_FLAG_TXBOTHDIR) == 0) {
            if (s->flags & SIG_FLAG_TOSERVER) {
                SCLogError("contradictory directions");
                return -1;
            }
            s->flags |= SIG_FLAG_TOCLIENT;
        }
    } else if (strncmp(*str, "to_server", strlen("to_server")) == 0) {
        *str += strlen("to_server");
        // skip space
        while (**str && isblank(**str)) {
            (*str)++;
        }
        // check comma or nothing
        if (**str) {
            if (only_dir) {
                SCLogError("unknown option: only accepts to_server or to_client");
                return -1;
            }
            if (**str != ',') {
                // leave to_client_something for next parser if not only_dir
                *str = orig;
                return 0;
            } else {
                (*str)++;
            }
            while (**str && isblank(**str)) {
                (*str)++;
            }
        }
        s->init_data->init_flags |= SIG_FLAG_INIT_FORCE_TOSERVER;
        if ((s->flags & SIG_FLAG_TXBOTHDIR) == 0) {
            if (s->flags & SIG_FLAG_TOCLIENT) {
                SCLogError("contradictory directions");
                return -1;
            }
            s->flags |= SIG_FLAG_TOSERVER;
        }
    } else if (only_dir) {
        SCLogError("unknown option: only accepts to_server or to_client");
        return -1;
    }
    return 0;
}

static int SigParseOptions(DetectEngineCtx *de_ctx, Signature *s, char *optstr, char *output,
        size_t output_size, bool requires)
{
    SigTableElmt *st = NULL;
    char *optname = NULL;
    char *optvalue = NULL;

    /* Trim leading space. */
    while (isblank(*optstr)) {
        optstr++;
    }

    /* Look for the end of this option, handling escaped semicolons. */
    char *optend = optstr;
    for (;;) {
        optend = strchr(optend, ';');
        if (optend == NULL) {
            SCLogError("no terminating \";\" found");
            goto error;
        }
        else if (optend > optstr && *(optend -1 ) == '\\') {
            optend++;
        } else {
            break;
        }
    }
    *(optend++) = '\0';

    /* Find the start of the option value. */
    char *optvalptr = strchr(optstr, ':');
    if (optvalptr) {
        *(optvalptr++) = '\0';

        /* Trim trailing space from name. */
        for (size_t i = strlen(optvalptr); i > 0; i--) {
            if (isblank(optvalptr[i - 1])) {
                optvalptr[i - 1] = '\0';
            } else {
                break;
            }
        }

        optvalue = optvalptr;
    }

    /* Trim trailing space from name. */
    for (size_t i = strlen(optstr); i > 0; i--) {
        if (isblank(optstr[i - 1])) {
            optstr[i - 1] = '\0';
        } else {
            break;
        }
    }
    optname = optstr;

    /* Check for options that are only to be processed during the
     * first "requires" pass. */
    bool requires_only = strcasecmp(optname, "requires") == 0 || strcasecmp(optname, "sid") == 0;
    if ((requires && !requires_only) || (!requires && requires_only)) {
        goto finish;
    }

    /* Call option parsing */
    st = SigTableGet(optname);
    if (st == NULL || st->Setup == NULL) {
        SCLogError("unknown rule keyword '%s'.", optname);
        goto error;
    }

    if (!(st->flags & (SIGMATCH_NOOPT|SIGMATCH_OPTIONAL_OPT))) {
        if (optvalue == NULL || strlen(optvalue) == 0) {
            SCLogError(
                    "invalid formatting or malformed option to %s keyword: '%s'", optname, optstr);
            goto error;
        }
    } else if (st->flags & SIGMATCH_NOOPT) {
        if (optvalue && strlen(optvalue)) {
            SCLogError("unexpected option to %s keyword: '%s'", optname, optstr);
            goto error;
        }
    }
    s->init_data->negated = false;

    const enum DetectKeywordId idx = SigTableGetIndex(st);
    s->init_data->has_possible_prefilter |= de_ctx->sm_types_prefilter[idx];

    if (st->flags & SIGMATCH_INFO_DEPRECATED) {
#define URL "https://suricata.io/our-story/deprecation-policy/"
        if (st->alternative == 0)
            SCLogWarning("keyword '%s' is deprecated "
                         "and will be removed soon. See %s",
                    st->name, URL);
        else
            SCLogWarning("keyword '%s' is deprecated "
                         "and will be removed soon. Use '%s' instead. "
                         "See %s",
                    st->name, sigmatch_table[st->alternative].name, URL);
#undef URL
    }

    int setup_ret = 0;

    /* Validate double quoting, trimming trailing white space along the way. */
    if (optvalue != NULL && strlen(optvalue) > 0) {
        size_t ovlen = strlen(optvalue);
        char *ptr = optvalue;

        /* skip leading whitespace */
        while (ovlen > 0) {
            if (!isblank(*ptr))
                break;
            ptr++;
            ovlen--;
        }
        if (ovlen == 0) {
            SCLogError("invalid formatting or malformed option to %s keyword: \'%s\'", optname,
                    optstr);
            goto error;
        }

        if (s->init_data->firewall_rule && (st->flags & SIGMATCH_SUPPORT_FIREWALL) == 0) {
            SCLogWarning("keyword \'%s\' has not been tested for firewall rules", optname);
        }

        /* see if value is negated */
        if ((st->flags & SIGMATCH_HANDLE_NEGATION) && *ptr == '!') {
            s->init_data->negated = true;
            ptr++;
            ovlen--;
        }
        /* skip more whitespace */
        while (ovlen > 0) {
            if (!isblank(*ptr))
                break;
            ptr++;
            ovlen--;
        }
        if (ovlen == 0) {
            SCLogError("invalid formatting or malformed option to %s keyword: \'%s\'", optname,
                    optstr);
            goto error;
        }
        /* if quoting is mandatory, enforce it */
        if (st->flags & SIGMATCH_QUOTES_MANDATORY && ovlen && *ptr != '"') {
            SCLogError("invalid formatting to %s keyword: "
                       "value must be double quoted \'%s\'",
                    optname, optstr);
            goto error;
        }

        if ((st->flags & (SIGMATCH_QUOTES_OPTIONAL|SIGMATCH_QUOTES_MANDATORY))
                && ovlen && *ptr == '"')
        {
            for (; ovlen > 0; ovlen--) {
                if (isblank(ptr[ovlen - 1])) {
                    ptr[ovlen - 1] = '\0';
                } else {
                    break;
                }
            }
            if (ovlen && ptr[ovlen - 1] != '"') {
                SCLogError("bad option value formatting (possible missing semicolon) "
                           "for keyword %s: \'%s\'",
                        optname, optvalue);
                goto error;
            }
            if (ovlen > 1) {
                /* strip leading " */
                ptr++;
                ovlen--;
                ptr[ovlen - 1] = '\0';
                ovlen--;
            }
            if (ovlen == 0) {
                SCLogError("bad input "
                           "for keyword %s: \'%s\'",
                        optname, optvalue);
                goto error;
            }
        } else {
            if (*ptr == '"') {
                SCLogError(
                        "quotes on %s keyword that doesn't support them: \'%s\'", optname, optstr);
                goto error;
            }
        }
        /* setup may or may not add a new SigMatch to the list */
        if (st->flags & SIGMATCH_SUPPORT_DIR) {
            if (DetectSetupDirection(s, &ptr, st->flags & SIGMATCH_OPTIONAL_OPT) < 0) {
                SCLogError("%s failed to setup direction", st->name);
                goto error;
            }
        }
        setup_ret = st->Setup(de_ctx, s, ptr);
        s->init_data->init_flags &= ~SIG_FLAG_INIT_FORCE_TOSERVER;
        s->init_data->init_flags &= ~SIG_FLAG_INIT_FORCE_TOCLIENT;
    } else {
        /* setup may or may not add a new SigMatch to the list */
        setup_ret = st->Setup(de_ctx, s, NULL);
    }
    if (setup_ret < 0) {
        SCLogDebug("\"%s\" failed to setup", st->name);

        /* handle 'silent' error case */
        if (setup_ret == -2) {
            if (!de_ctx->sm_types_silent_error[idx]) {
                de_ctx->sm_types_silent_error[idx] = true;
                return -1;
            }
            return -2;
        }
        return setup_ret;
    }
    s->init_data->negated = false;

finish:
    if (strlen(optend) > 0) {
        strlcpy(output, optend, output_size);
        return 1;
    }

    return 0;

error:
    return -1;
}

/** \brief Parse address string and update signature
 *
 *  \retval 0 ok, -1 error
 */
static int SigParseAddress(DetectEngineCtx *de_ctx,
        Signature *s, const char *addrstr, char flag)
{
    SCLogDebug("Address Group \"%s\" to be parsed now", addrstr);

    /* pass on to the address(list) parser */
    if (flag == 0) {
        if (strcasecmp(addrstr, "any") == 0)
            s->flags |= SIG_FLAG_SRC_ANY;

        s->init_data->src = DetectParseAddress(de_ctx, addrstr,
                &s->init_data->src_contains_negation);
        if (s->init_data->src == NULL)
            goto error;
    } else {
        if (strcasecmp(addrstr, "any") == 0)
            s->flags |= SIG_FLAG_DST_ANY;

        s->init_data->dst = DetectParseAddress(de_ctx, addrstr,
                &s->init_data->dst_contains_negation);
        if (s->init_data->dst == NULL)
            goto error;
    }

    return 0;

error:
    return -1;
}

static bool IsBuiltIn(const char *n)
{
    if (strcmp(n, "request_started") == 0 || strcmp(n, "response_started") == 0) {
        return true;
    }
    if (strcmp(n, "request_complete") == 0 || strcmp(n, "response_complete") == 0) {
        return true;
    }
    return false;
}

/** \brief register app hooks as generic lists
 *
 *  Register each hook in each app protocol as:
 *  <alproto>:<hook name>:generic
 *  These lists can be used by lua scripts to hook into.
 *
 *  \todo move elsewhere? maybe a detect-engine-hook.c?
 */
void DetectRegisterAppLayerHookLists(void)
{
    for (AppProto a = ALPROTO_FAILED + 1; a < g_alproto_max; a++) {
        const char *alproto_name = AppProtoToString(a);
        if (strcmp(alproto_name, "http") == 0)
            alproto_name = "http1";
        SCLogDebug("alproto %u/%s", a, alproto_name);

        const int max_progress_ts =
                AppLayerParserGetStateProgressCompletionStatus(a, STREAM_TOSERVER);
        const int max_progress_tc =
                AppLayerParserGetStateProgressCompletionStatus(a, STREAM_TOCLIENT);

        char ts_tx_started[64];
        snprintf(ts_tx_started, sizeof(ts_tx_started), "%s:request_started:generic", alproto_name);
        DetectAppLayerInspectEngineRegister(
                ts_tx_started, a, SIG_FLAG_TOSERVER, 0, DetectEngineInspectGenericList, NULL);
        SCLogDebug("- hook %s:%s list %s (%u)", alproto_name, "request_name", ts_tx_started,
                (uint32_t)strlen(ts_tx_started));

        char tc_tx_started[64];
        snprintf(tc_tx_started, sizeof(tc_tx_started), "%s:response_started:generic", alproto_name);
        DetectAppLayerInspectEngineRegister(
                tc_tx_started, a, SIG_FLAG_TOCLIENT, 0, DetectEngineInspectGenericList, NULL);
        SCLogDebug("- hook %s:%s list %s (%u)", alproto_name, "response_name", tc_tx_started,
                (uint32_t)strlen(tc_tx_started));

        char ts_tx_complete[64];
        snprintf(ts_tx_complete, sizeof(ts_tx_complete), "%s:request_complete:generic",
                alproto_name);
        DetectAppLayerInspectEngineRegister(ts_tx_complete, a, SIG_FLAG_TOSERVER, max_progress_ts,
                DetectEngineInspectGenericList, NULL);
        SCLogDebug("- hook %s:%s list %s (%u)", alproto_name, "request_name", ts_tx_complete,
                (uint32_t)strlen(ts_tx_complete));

        char tc_tx_complete[64];
        snprintf(tc_tx_complete, sizeof(tc_tx_complete), "%s:response_complete:generic",
                alproto_name);
        DetectAppLayerInspectEngineRegister(tc_tx_complete, a, SIG_FLAG_TOCLIENT, max_progress_tc,
                DetectEngineInspectGenericList, NULL);
        SCLogDebug("- hook %s:%s list %s (%u)", alproto_name, "response_name", tc_tx_complete,
                (uint32_t)strlen(tc_tx_complete));

        for (int p = 0; p <= max_progress_ts; p++) {
            const char *name = AppLayerParserGetStateNameById(
                    IPPROTO_TCP /* TODO no ipproto */, a, p, STREAM_TOSERVER);
            if (name != NULL && !IsBuiltIn(name)) {
                char list_name[64];
                snprintf(list_name, sizeof(list_name), "%s:%s:generic", alproto_name, name);
                SCLogDebug("- hook %s:%s list %s (%u)", alproto_name, name, list_name,
                        (uint32_t)strlen(list_name));

                DetectAppLayerInspectEngineRegister(
                        list_name, a, SIG_FLAG_TOSERVER, p, DetectEngineInspectGenericList, NULL);
            }
        }
        for (int p = 0; p <= max_progress_tc; p++) {
            const char *name = AppLayerParserGetStateNameById(
                    IPPROTO_TCP /* TODO no ipproto */, a, p, STREAM_TOCLIENT);
            if (name != NULL && !IsBuiltIn(name)) {
                char list_name[64];
                snprintf(list_name, sizeof(list_name), "%s:%s:generic", alproto_name, name);
                SCLogDebug("- hook %s:%s list %s (%u)", alproto_name, name, list_name,
                        (uint32_t)strlen(list_name));

                DetectAppLayerInspectEngineRegister(
                        list_name, a, SIG_FLAG_TOCLIENT, p, DetectEngineInspectGenericList, NULL);
            }
        }
    }
}

#ifdef DEBUG
static const char *SignatureHookTypeToString(enum SignatureHookType t)
{
    switch (t) {
        case SIGNATURE_HOOK_TYPE_NOT_SET:
            return "not_set";
        case SIGNATURE_HOOK_TYPE_APP:
            return "app";
        case SIGNATURE_HOOK_TYPE_PKT:
            return "pkt";
    }
    return "unknown";
}
#endif

static enum SignatureHookPkt HookPktFromString(const char *str)
{
    if (strcmp(str, "flow_start") == 0) {
        return SIGNATURE_HOOK_PKT_FLOW_START;
    } else if (strcmp(str, "pre_flow") == 0) {
        return SIGNATURE_HOOK_PKT_PRE_FLOW;
    } else if (strcmp(str, "pre_stream") == 0) {
        return SIGNATURE_HOOK_PKT_PRE_STREAM;
    } else if (strcmp(str, "all") == 0) {
        return SIGNATURE_HOOK_PKT_ALL;
    }
    return SIGNATURE_HOOK_PKT_NOT_SET;
}

#ifdef DEBUG
static const char *HookPktToString(const enum SignatureHookPkt ph)
{
    switch (ph) {
        case SIGNATURE_HOOK_PKT_NOT_SET:
            return "not set";
        case SIGNATURE_HOOK_PKT_FLOW_START:
            return "flow_start";
        case SIGNATURE_HOOK_PKT_PRE_FLOW:
            return "pre_flow";
        case SIGNATURE_HOOK_PKT_PRE_STREAM:
            return "pre_stream";
        case SIGNATURE_HOOK_PKT_ALL:
            return "all";
    }
    return "error";
}
#endif

static SignatureHook SetPktHook(const char *hook_str)
{
    SignatureHook h = {
        .type = SIGNATURE_HOOK_TYPE_PKT,
        .t.pkt.ph = HookPktFromString(hook_str),
    };
    return h;
}

/**
 * \param proto_hook string of protocol and hook, e.g. dns:request_complete
 */
static int SigParseProtoHookPkt(Signature *s, const char *proto_hook, const char *p, const char *h)
{
    enum SignatureHookPkt hook = HookPktFromString(h);
    if (hook != SIGNATURE_HOOK_PKT_NOT_SET) {
        s->init_data->hook = SetPktHook(h);
        if (s->init_data->hook.t.pkt.ph == SIGNATURE_HOOK_PKT_NOT_SET) {
            return -1; // TODO unreachable?
        }
    } else {
        SCLogError("unknown pkt hook %s", h);
        return -1;
    }

    SCLogDebug("protocol:%s hook:%s: type:%s parsed hook:%s", p, h,
            SignatureHookTypeToString(s->init_data->hook.type),
            HookPktToString(s->init_data->hook.t.pkt.ph));
    return 0;
}

static SignatureHook SetAppHook(const AppProto alproto, int progress)
{
    SignatureHook h = {
        .type = SIGNATURE_HOOK_TYPE_APP,
        .t.app.alproto = alproto,
        .t.app.app_progress = progress,
    };
    return h;
}

/**
 * \param proto_hook string of protocol and hook, e.g. dns:request_complete
 */
static int SigParseProtoHookApp(Signature *s, const char *proto_hook, const char *p, const char *h)
{
    if (strcmp(h, "request_started") == 0) {
        s->flags |= SIG_FLAG_TOSERVER;
        s->init_data->hook =
                SetAppHook(s->alproto, 0); // state 0 should be the starting state in each protocol.
    } else if (strcmp(h, "response_started") == 0) {
        s->flags |= SIG_FLAG_TOCLIENT;
        s->init_data->hook =
                SetAppHook(s->alproto, 0); // state 0 should be the starting state in each protocol.
    } else if (strcmp(h, "request_complete") == 0) {
        s->flags |= SIG_FLAG_TOSERVER;
        s->init_data->hook = SetAppHook(s->alproto,
                AppLayerParserGetStateProgressCompletionStatus(s->alproto, STREAM_TOSERVER));
    } else if (strcmp(h, "response_complete") == 0) {
        s->flags |= SIG_FLAG_TOCLIENT;
        s->init_data->hook = SetAppHook(s->alproto,
                AppLayerParserGetStateProgressCompletionStatus(s->alproto, STREAM_TOCLIENT));
    } else {
        const int progress_ts = AppLayerParserGetStateIdByName(
                IPPROTO_TCP /* TODO */, s->alproto, h, STREAM_TOSERVER);
        if (progress_ts >= 0) {
            s->flags |= SIG_FLAG_TOSERVER;
            s->init_data->hook = SetAppHook(s->alproto, progress_ts);
        } else {
            const int progress_tc = AppLayerParserGetStateIdByName(
                    IPPROTO_TCP /* TODO */, s->alproto, h, STREAM_TOCLIENT);
            if (progress_tc < 0) {
                return -1;
            }
            s->flags |= SIG_FLAG_TOCLIENT;
            s->init_data->hook = SetAppHook(s->alproto, progress_tc);
        }
    }

    char generic_hook_name[64];
    snprintf(generic_hook_name, sizeof(generic_hook_name), "%s:generic", proto_hook);
    int list = DetectBufferTypeGetByName(generic_hook_name);
    if (list < 0) {
        SCLogError("no list registered as %s for hook %s", generic_hook_name, proto_hook);
        return -1;
    }
    s->init_data->hook.sm_list = list;

    SCLogDebug("protocol:%s hook:%s: type:%s alproto:%u hook:%d", p, h,
            SignatureHookTypeToString(s->init_data->hook.type), s->init_data->hook.t.app.alproto,
            s->init_data->hook.t.app.app_progress);

    s->app_progress_hook = (uint8_t)s->init_data->hook.t.app.app_progress;
    return 0;
}

/**
 * \brief Parses the protocol supplied by the Signature.
 *
 *        http://www.iana.org/assignments/protocol-numbers
 *
 * \param s        Pointer to the Signature instance to which the parsed
 *                 protocol has to be added.
 * \param protostr Pointer to the character string containing the protocol name.
 *
 * \retval  0 On successfully parsing the protocol sent as the argument.
 * \retval -1 On failure
 */
static int SigParseProto(Signature *s, const char *protostr)
{
    SCEnter();
    if (strlen(protostr) > 32)
        return -1;

    char proto[33];
    strlcpy(proto, protostr, 33);
    const char *p = proto;
    const char *h = NULL;

    bool has_hook = strchr(proto, ':') != NULL;
    if (has_hook) {
        char *xsaveptr = NULL;
        p = strtok_r(proto, ":", &xsaveptr);
        h = strtok_r(NULL, ":", &xsaveptr);
        SCLogDebug("p: '%s' h: '%s'", p, h);
    }
    if (p == NULL) {
        SCLogError("invalid protocol specification '%s'", proto);
        return -1;
    }

    int r = DetectProtoParse(&s->proto, p);
    if (r < 0) {
        s->alproto = AppLayerGetProtoByName(p);
        /* indicate that the signature is app-layer */
        if (s->alproto != ALPROTO_UNKNOWN) {
            s->flags |= SIG_FLAG_APPLAYER;

            AppLayerProtoDetectSupportedIpprotos(s->alproto, s->proto.proto);

            if (h) {
                if (SigParseProtoHookApp(s, protostr, p, h) < 0) {
                    SCLogError("protocol \"%s\" does not support hook \"%s\"", p, h);
                    SCReturnInt(-1);
                }
            }
        }
        else {
            SCLogError("protocol \"%s\" cannot be used "
                       "in a signature.  Either detection for this protocol "
                       "is not yet supported OR detection has been disabled for "
                       "protocol through the yaml option "
                       "app-layer.protocols.%s.detection-enabled",
                    p, p);
            SCReturnInt(-1);
        }
    } else if (h != NULL) {
        SCLogDebug("non-app-layer rule with %s:%s", p, h);

        if (SigParseProtoHookPkt(s, protostr, p, h) < 0) {
            SCLogError("protocol \"%s\" does not support hook \"%s\"", p, h);
            SCReturnInt(-1);
        }
    }

    /* if any of these flags are set they are set in a mutually exclusive
     * manner */
    if (s->proto.flags & DETECT_PROTO_ONLY_PKT) {
        s->flags |= SIG_FLAG_REQUIRE_PACKET;
    } else if (s->proto.flags & DETECT_PROTO_ONLY_STREAM) {
        s->flags |= SIG_FLAG_REQUIRE_STREAM;
    }

    SCReturnInt(0);
}

/**
 * \brief Parses the port(source or destination) field, from a Signature.
 *
 * \param s       Pointer to the signature which has to be updated with the
 *                port information.
 * \param portstr Pointer to the character string containing the port info.
 * \param         Flag which indicates if the portstr received is src or dst
 *                port.  For src port: flag = 0, dst port: flag = 1.
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
static int SigParsePort(const DetectEngineCtx *de_ctx,
        Signature *s, const char *portstr, char flag)
{
    int r = 0;

    /* XXX VJ exclude handling this for none UDP/TCP proto's */

    SCLogDebug("Port group \"%s\" to be parsed", portstr);

    if (flag == 0) {
        if (strcasecmp(portstr, "any") == 0)
            s->flags |= SIG_FLAG_SP_ANY;

        r = DetectPortParse(de_ctx, &s->sp, (char *)portstr);
    } else if (flag == 1) {
        if (strcasecmp(portstr, "any") == 0)
            s->flags |= SIG_FLAG_DP_ANY;

        r = DetectPortParse(de_ctx, &s->dp, (char *)portstr);
    }

    if (r < 0)
        return -1;

    return 0;
}

/** \retval 1 valid
 *  \retval 0 invalid
 */
static int SigParseActionRejectValidate(const char *action)
{
#ifdef HAVE_LIBNET11
#if defined HAVE_LIBCAP_NG && !defined HAVE_LIBNET_CAPABILITIES
    if (sc_set_caps) {
        SCLogError("Libnet 1.1 is "
                   "incompatible with POSIX based capabilities with privs dropping. "
                   "For rejects to work, run as root/super user.");
        return 0;
    }
#endif
#else /* no libnet 1.1 */
    SCLogError("Libnet 1.1.x is "
               "required for action \"%s\" but is not compiled into Suricata",
            action);
    return 0;
#endif
    return 1;
}

/** \retval 0 on error
 *  \retval flags on success
 */
static uint8_t ActionStringToFlags(const char *action)
{
    if (strcasecmp(action, "alert") == 0) {
        return ACTION_ALERT;
    } else if (strcasecmp(action, "drop") == 0) {
        return ACTION_DROP | ACTION_ALERT;
    } else if (strcasecmp(action, "pass") == 0) {
        return ACTION_PASS;
    } else if (strcasecmp(action, "reject") == 0 ||
               strcasecmp(action, "rejectsrc") == 0)
    {
        if (!(SigParseActionRejectValidate(action)))
            return 0;
        return ACTION_REJECT | ACTION_DROP | ACTION_ALERT;
    } else if (strcasecmp(action, "rejectdst") == 0) {
        if (!(SigParseActionRejectValidate(action)))
            return 0;
        return ACTION_REJECT_DST | ACTION_DROP | ACTION_ALERT;
    } else if (strcasecmp(action, "rejectboth") == 0) {
        if (!(SigParseActionRejectValidate(action)))
            return 0;
        return ACTION_REJECT_BOTH | ACTION_DROP | ACTION_ALERT;
    } else if (strcasecmp(action, "config") == 0) {
        return ACTION_CONFIG;
    } else if (strcasecmp(action, "accept") == 0) {
        return ACTION_ACCEPT;
    } else {
        SCLogError("An invalid action \"%s\" was given", action);
        return 0;
    }
}

/**
 * \brief Parses the action that has been used by the Signature and allots it
 *        to its Signature instance.
 *
 * \param s      Pointer to the Signature instance to which the action belongs.
 * \param action Pointer to the action string used by the Signature.
 *
 * \retval  0 On successfully parsing the action string and adding it to the
 *            Signature.
 * \retval -1 On failure.
 */
static int SigParseAction(Signature *s, const char *action_in)
{
    char action[32];
    strlcpy(action, action_in, sizeof(action));
    const char *a = action;
    const char *o = NULL;

    bool has_scope = strchr(action, ':') != NULL;
    if (has_scope) {
        char *xsaveptr = NULL;
        a = strtok_r(action, ":", &xsaveptr);
        o = strtok_r(NULL, ":", &xsaveptr);
        SCLogDebug("a: '%s' o: '%s'", a, o);
    }
    if (a == NULL) {
        SCLogError("invalid protocol specification '%s'", action_in);
        return -1;
    }

    uint8_t flags = ActionStringToFlags(a);
    if (flags == 0)
        return -1;

    /* parse scope, if any */
    if (o) {
        uint8_t scope_flags = 0;
        if (flags & (ACTION_DROP | ACTION_PASS)) {
            if (strcmp(o, "packet") == 0) {
                scope_flags = (uint8_t)ACTION_SCOPE_PACKET;
            } else if (strcmp(o, "flow") == 0) {
                scope_flags = (uint8_t)ACTION_SCOPE_FLOW;
            } else {
                SCLogError("invalid action scope '%s' in action '%s': only 'packet' and 'flow' "
                           "allowed",
                        o, action_in);
                return -1;
            }
            s->action_scope = scope_flags;
        } else if (flags & (ACTION_ACCEPT)) {
            if (strcmp(o, "packet") == 0) {
                scope_flags = (uint8_t)ACTION_SCOPE_PACKET;
            } else if (strcmp(o, "hook") == 0) {
                scope_flags = (uint8_t)ACTION_SCOPE_HOOK;
            } else if (strcmp(o, "tx") == 0) {
                scope_flags = (uint8_t)ACTION_SCOPE_TX;
            } else if (strcmp(o, "flow") == 0) {
                scope_flags = (uint8_t)ACTION_SCOPE_FLOW;
            } else {
                SCLogError(
                        "invalid action scope '%s' in action '%s': only 'packet', 'flow', 'tx' and "
                        "'hook' allowed",
                        o, action_in);
                return -1;
            }
            s->action_scope = scope_flags;
        } else if (flags & (ACTION_CONFIG)) {
            if (strcmp(o, "packet") == 0) {
                scope_flags = (uint8_t)ACTION_SCOPE_PACKET;
            } else {
                SCLogError("invalid action scope '%s' in action '%s': only 'packet' allowed", o,
                        action_in);
                return -1;
            }
            s->action_scope = scope_flags;
        } else {
            SCLogError("invalid action scope '%s' in action '%s': scope only supported for actions "
                       "'drop', 'pass' and 'reject'",
                    o, action_in);
            return -1;
        }
    }

    /* require explicit action scope for fw rules */
    if (s->init_data->firewall_rule && s->action_scope == 0) {
        SCLogError("firewall rules require setting an explicit action scope");
        return -1;
    }

    if (!s->init_data->firewall_rule && (flags & ACTION_ACCEPT)) {
        SCLogError("'accept' action only supported for firewall rules");
        return -1;
    }

    if (s->init_data->firewall_rule && (flags & ACTION_PASS)) {
        SCLogError("'pass' action not supported for firewall rules");
        return -1;
    }

    s->action = flags;
    return 0;
}

/**
 * \brief Parse the next token in rule.
 *
 * For rule parsing a token is considered to be a string of characters
 * separated by white space.
 *
 * \param input double pointer to input buffer, will be advanced as input is
 *     parsed.
 * \param output buffer to copy token into.
 * \param output_size length of output buffer.
 */
static inline int SigParseToken(char **input, char *output,
    const size_t output_size)
{
    size_t len = *input == NULL ? 0 : strlen(*input);

    if (!len) {
        return 0;
    }

    while (len && isblank(**input)) {
        (*input)++;
        len--;
    }

    char *endptr = strpbrk(*input, " \t\n\r");
    if (endptr != NULL) {
        *(endptr++) = '\0';
    }
    strlcpy(output, *input, output_size);
    *input = endptr;

    return 1;
}

/**
 * \brief Parse the next rule "list" token.
 *
 * Parses rule tokens that may be lists such as addresses and ports
 * handling the case when they may not be lists.
 *
 * \param input double pointer to input buffer, will be advanced as input is
 *     parsed.
 * \param output buffer to copy token into.
 * \param output_size length of output buffer.
 */
static inline int SigParseList(char **input, char *output,
    const size_t output_size)
{
    int in_list = 0;
    size_t len = *input != NULL ? strlen(*input) : 0;

    if (len == 0) {
        return 0;
    }

    while (len && isblank(**input)) {
        (*input)++;
        len--;
    }

    size_t i = 0;
    for (i = 0; i < len; i++) {
        char c = (*input)[i];
        if (c == '[') {
            in_list++;
        } else if (c == ']') {
            in_list--;
        } else if (c == ' ') {
            if (!in_list) {
                break;
            }
        }
    }
    if (i == len) {
        *input = NULL;
        return 0;
    }
    (*input)[i] = '\0';
    strlcpy(output, *input, output_size);
    *input = *input + i + 1;

    return 1;
}

/**
 *  \internal
 *  \brief split a signature string into a few blocks for further parsing
 *
 *  \param scan_only just scan, don't validate
 */
static int SigParseBasics(DetectEngineCtx *de_ctx, Signature *s, const char *sigstr,
        SignatureParser *parser, uint8_t addrs_direction, bool scan_only)
{
    char *index, dup[DETECT_MAX_RULE_SIZE];

    strlcpy(dup, sigstr, DETECT_MAX_RULE_SIZE);
    index = dup;

    /* Action. */
    SigParseToken(&index, parser->action, sizeof(parser->action));

    /* Protocol. */
    SigParseList(&index, parser->protocol, sizeof(parser->protocol));

    /* Source. */
    SigParseList(&index, parser->src, sizeof(parser->src));

    /* Source port(s). */
    SigParseList(&index, parser->sp, sizeof(parser->sp));

    /* Direction. */
    SigParseToken(&index, parser->direction, sizeof(parser->direction));

    /* Destination. */
    SigParseList(&index, parser->dst, sizeof(parser->dst));

    /* Destination port(s). */
    SigParseList(&index, parser->dp, sizeof(parser->dp));

    /* Options. */
    if (index == NULL) {
        SCLogError("no rule options.");
        goto error;
    }
    while (isspace(*index) || *index == '(') {
        index++;
    }
    for (size_t i = strlen(index); i > 0; i--) {
        if (isspace(index[i - 1]) || index[i - 1] == ')') {
            index[i - 1] = '\0';
        } else {
            break;
        }
    }
    strlcpy(parser->opts, index, sizeof(parser->opts));

    if (scan_only) {
        return 0;
    }

    /* Parse Action */
    if (SigParseAction(s, parser->action) < 0)
        goto error;

    if (SigParseProto(s, parser->protocol) < 0)
        goto error;

    if (strcmp(parser->direction, "<>") == 0) {
        s->init_data->init_flags |= SIG_FLAG_INIT_BIDIREC;
    } else if (strcmp(parser->direction, "=>") == 0) {
        if (s->flags & SIG_FLAG_FIREWALL) {
            SCLogError("transactional bidirectional rules not supported for firewall rules");
            goto error;
        }

        s->flags |= SIG_FLAG_TXBOTHDIR;
    } else if (strcmp(parser->direction, "->") != 0) {
        SCLogError("\"%s\" is not a valid direction modifier, "
                   "\"->\" and \"<>\" are supported.",
                parser->direction);
        goto error;
    }

    /* Parse Address & Ports */
    if (SigParseAddress(de_ctx, s, parser->src, SIG_DIREC_SRC ^ addrs_direction) < 0)
       goto error;

    if (SigParseAddress(de_ctx, s, parser->dst, SIG_DIREC_DST ^ addrs_direction) < 0)
        goto error;

    /* By AWS - Traditionally we should be doing this only for tcp/udp/sctp,
     * but we do it for regardless of ip proto, since the dns/dnstcp/dnsudp
     * changes that we made sees to it that at this point of time we don't
     * set the ip proto for the sig.  We do it a bit later. */
    if (SigParsePort(de_ctx, s, parser->sp, SIG_DIREC_SRC ^ addrs_direction) < 0)
        goto error;
    if (SigParsePort(de_ctx, s, parser->dp, SIG_DIREC_DST ^ addrs_direction) < 0)
        goto error;

    return 0;

error:
    return -1;
}

static inline bool CheckAscii(const char *str)
{
    for (size_t i = 0; i < strlen(str); i++) {
        if (str[i] < 0x20) {
            // LF CR TAB
            if (str[i] == 0x0a || str[i] == 0x0d || str[i] == 0x09) {
                continue;
            }
            return false;
        } else if (str[i] == 0x7f) {
            return false;
        }
    }
    return true;
}

/**
 *  \brief parse a signature
 *
 *  \param de_ctx detection engine ctx to add it to
 *  \param s memory structure to store the signature in
 *  \param sigstr the raw signature as a null terminated string
 *  \param addrs_direction direction (for bi-directional sigs)
 *  \param require only scan rule for requires
 *
 *  \param -1 parse error
 *  \param 0 ok
 */
static int SigParse(DetectEngineCtx *de_ctx, Signature *s, const char *sigstr,
        uint8_t addrs_direction, SignatureParser *parser, bool requires)
{
    SCEnter();

    if (!SCCheckUtf8(sigstr)) {
        SCLogError("rule is not valid UTF-8");
        SCReturnInt(-1);
    }

    if (!CheckAscii(sigstr)) {
        SCLogError("rule contains invalid (control) characters");
        SCReturnInt(-1);
    }

    int ret = SigParseBasics(de_ctx, s, sigstr, parser, addrs_direction, requires);
    if (ret < 0) {
        SCLogDebug("SigParseBasics failed");
        SCReturnInt(-1);
    }

    /* we can have no options, so make sure we have them */
    if (strlen(parser->opts) > 0) {
        size_t buffer_size = strlen(parser->opts) + 1;
        char input[buffer_size];
        char output[buffer_size];
        memset(input, 0x00, buffer_size);
        memcpy(input, parser->opts, strlen(parser->opts) + 1);

        /* loop the option parsing. Each run processes one option
         * and returns the rest of the option string through the
         * output variable. */
        do {
            memset(output, 0x00, buffer_size);
            ret = SigParseOptions(de_ctx, s, input, output, buffer_size, requires);
            if (ret == 1) {
                memcpy(input, output, buffer_size);
            }

        } while (ret == 1);

        if (ret < 0) {
            /* Suricata didn't meet the rule requirements, skip. */
            goto end;
        }
    }

end:
    DetectIPProtoRemoveAllSMs(de_ctx, s);

    SCReturnInt(ret);
}

/** \brief check if buffers array still has space left, expand if not
 */
int SignatureInitDataBufferCheckExpand(Signature *s)
{
    if (s->init_data->buffers_size >= 64)
        return -1;

    if (s->init_data->buffer_index + 1 == s->init_data->buffers_size) {
        void *ptr = SCRealloc(s->init_data->buffers,
                (s->init_data->buffers_size + 8) * sizeof(SignatureInitDataBuffer));
        if (ptr == NULL)
            return -1;
        s->init_data->buffers = ptr;
        for (uint32_t x = s->init_data->buffers_size; x < s->init_data->buffers_size + 8; x++) {
            SignatureInitDataBuffer *b = &s->init_data->buffers[x];
            memset(b, 0, sizeof(*b));
        }
        s->init_data->buffers_size += 8;
    }
    return 0;
}

Signature *SigAlloc (void)
{
    Signature *sig = SCCalloc(1, sizeof(Signature));
    if (unlikely(sig == NULL))
        return NULL;

    sig->init_data = SCCalloc(1, sizeof(SignatureInitData));
    if (sig->init_data == NULL) {
        SCFree(sig);
        return NULL;
    }
    sig->init_data->mpm_sm_list = -1;

    sig->init_data->buffers = SCCalloc(8, sizeof(SignatureInitDataBuffer));
    if (sig->init_data->buffers == NULL) {
        SCFree(sig->init_data);
        SCFree(sig);
        return NULL;
    }
    sig->init_data->buffers_size = 8;

    /* assign it to -1, so that we can later check if the value has been
     * overwritten after the Signature has been parsed, and if it hasn't been
     * overwritten, we can then assign the default value of 3 */
    sig->prio = -1;

    /* rule interdepency is false, at start */
    sig->init_data->is_rule_state_dependant = false;
    /* first index is 0 */
    sig->init_data->rule_state_dependant_sids_idx = 0;

    sig->init_data->list = DETECT_SM_LIST_NOTSET;
    return sig;
}

/**
 * \internal
 * \brief Free Metadata list
 *
 * \param s Pointer to the signature
 */
static void SigMetadataFree(Signature *s)
{
    SCEnter();

    DetectMetadata *mdata = NULL;
    DetectMetadata *next_mdata = NULL;

    if (s == NULL || s->metadata == NULL) {
        SCReturn;
    }

    SCLogDebug("s %p, s->metadata %p", s, s->metadata);

    for (mdata = s->metadata->list; mdata != NULL;)   {
        next_mdata = mdata->next;
        DetectMetadataFree(mdata);
        mdata = next_mdata;
    }
    SCFree(s->metadata->json_str);
    SCFree(s->metadata);
    s->metadata = NULL;

    SCReturn;
}

/**
 * \internal
 * \brief Free Reference list
 *
 * \param s Pointer to the signature
 */
static void SigRefFree (Signature *s)
{
    SCEnter();

    DetectReference *ref = NULL;
    DetectReference *next_ref = NULL;

    if (s == NULL) {
        SCReturn;
    }

    SCLogDebug("s %p, s->references %p", s, s->references);

    for (ref = s->references; ref != NULL;)   {
        next_ref = ref->next;
        DetectReferenceFree(ref);
        ref = next_ref;
    }

    s->references = NULL;

    SCReturn;
}

static void SigMatchFreeArrays(DetectEngineCtx *de_ctx, Signature *s, int ctxs)
{
    if (s != NULL) {
        int type;
        for (type = 0; type < DETECT_SM_LIST_MAX; type++) {
            if (s->sm_arrays[type] != NULL) {
                if (ctxs) {
                    SigMatchData *smd = s->sm_arrays[type];
                    while(1) {
                        if (sigmatch_table[smd->type].Free != NULL) {
                            sigmatch_table[smd->type].Free(de_ctx, smd->ctx);
                        }
                        if (smd->is_last)
                            break;
                        smd++;
                    }
                }

                SCFree(s->sm_arrays[type]);
            }
        }
    }
}

void SigFree(DetectEngineCtx *de_ctx, Signature *s)
{
    if (s == NULL)
        return;

    int i;

    if (s->init_data && s->init_data->transforms.cnt) {
        for(i = 0; i < s->init_data->transforms.cnt; i++) {
            if (s->init_data->transforms.transforms[i].options) {
                int transform = s->init_data->transforms.transforms[i].transform;
                sigmatch_table[transform].Free(
                        de_ctx, s->init_data->transforms.transforms[i].options);
                s->init_data->transforms.transforms[i].options = NULL;
            }
        }
    }
    if (s->init_data) {
        for (i = 0; i < DETECT_SM_LIST_MAX; i++) {
            SigMatch *sm = s->init_data->smlists[i];
            while (sm != NULL) {
                SigMatch *nsm = sm->next;
                SigMatchFree(de_ctx, sm);
                sm = nsm;
            }
        }

        for (uint32_t x = 0; x < s->init_data->buffer_index; x++) {
            SigMatch *sm = s->init_data->buffers[x].head;
            while (sm != NULL) {
                SigMatch *nsm = sm->next;
                SigMatchFree(de_ctx, sm);
                sm = nsm;
            }
        }
        if (s->init_data->cidr_dst != NULL)
            IPOnlyCIDRListFree(s->init_data->cidr_dst);

        if (s->init_data->cidr_src != NULL)
            IPOnlyCIDRListFree(s->init_data->cidr_src);

        SCFree(s->init_data->buffers);
        s->init_data->buffers = NULL;
    }
    SigMatchFreeArrays(de_ctx, s, (s->init_data == NULL));
    if (s->init_data) {
        SCFree(s->init_data);
        s->init_data = NULL;
    }

    if (s->sp != NULL) {
        DetectPortCleanupList(NULL, s->sp);
    }
    if (s->dp != NULL) {
        DetectPortCleanupList(NULL, s->dp);
    }

    if (s->msg != NULL)
        SCFree(s->msg);

    if (s->addr_src_match4 != NULL) {
        SCFree(s->addr_src_match4);
    }
    if (s->addr_dst_match4 != NULL) {
        SCFree(s->addr_dst_match4);
    }
    if (s->addr_src_match6 != NULL) {
        SCFree(s->addr_src_match6);
    }
    if (s->addr_dst_match6 != NULL) {
        SCFree(s->addr_dst_match6);
    }
    if (s->sig_str != NULL) {
        SCFree(s->sig_str);
    }

    SigRefFree(s);
    SigMetadataFree(s);

    DetectEngineAppInspectionEngineSignatureFree(de_ctx, s);

    SCFree(s);
}

/**
 * \brief this function is used to set multiple possible app-layer protos
 * \brief into the current signature (for example ja4 for both tls and quic)
 *
 * \param s pointer to the Current Signature
 * \param alprotos an array terminated by ALPROTO_UNKNOWN
 *
 * \retval 0 on Success
 * \retval -1 on Failure
 */
int DetectSignatureSetMultiAppProto(Signature *s, const AppProto *alprotos)
{
    if (s->alproto != ALPROTO_UNKNOWN) {
        // One alproto was set, check if it matches the new ones proposed
        while (*alprotos != ALPROTO_UNKNOWN) {
            if (s->alproto == *alprotos) {
                // alproto already set to only one
                return 0;
            }
            alprotos++;
        }
        // alproto already set and not matching the new set of alprotos
        return -1;
    }
    if (s->init_data->alprotos[0] != ALPROTO_UNKNOWN) {
        // check intersection of already used alprotos and new ones
        for (AppProto i = 0; i < SIG_ALPROTO_MAX; i++) {
            if (s->init_data->alprotos[i] == ALPROTO_UNKNOWN) {
                break;
            }
            // first disable the ones that do not match
            bool found = false;
            const AppProto *args = alprotos;
            while (*args != ALPROTO_UNKNOWN) {
                if (s->init_data->alprotos[i] == *args) {
                    found = true;
                    break;
                }
                args++;
            }
            if (!found) {
                s->init_data->alprotos[i] = ALPROTO_UNKNOWN;
            }
        }
        // Then put at the beginning every defined protocol
        for (AppProto i = 0; i < SIG_ALPROTO_MAX; i++) {
            if (s->init_data->alprotos[i] == ALPROTO_UNKNOWN) {
                for (AppProto j = SIG_ALPROTO_MAX - 1; j > i; j--) {
                    if (s->init_data->alprotos[j] != ALPROTO_UNKNOWN) {
                        s->init_data->alprotos[i] = s->init_data->alprotos[j];
                        s->init_data->alprotos[j] = ALPROTO_UNKNOWN;
                        break;
                    }
                }
                if (s->init_data->alprotos[i] == ALPROTO_UNKNOWN) {
                    if (i == 0) {
                        // there was no intersection
                        return -1;
                    } else if (i == 1) {
                        // intersection is singleton, set it as usual
                        AppProto alproto = s->init_data->alprotos[0];
                        s->init_data->alprotos[0] = ALPROTO_UNKNOWN;
                        return SCDetectSignatureSetAppProto(s, alproto);
                    }
                    break;
                }
            }
        }
    } else {
        if (alprotos[0] == ALPROTO_UNKNOWN) {
            // do not allow empty set
            return -1;
        }
        if (alprotos[1] == ALPROTO_UNKNOWN) {
            // allow singleton, but call traditional setter
            return SCDetectSignatureSetAppProto(s, alprotos[0]);
        }
        // first time we enforce alprotos
        for (AppProto i = 0; i < SIG_ALPROTO_MAX; i++) {
            if (alprotos[i] == ALPROTO_UNKNOWN) {
                break;
            }
            s->init_data->alprotos[i] = alprotos[i];
        }
    }
    return 0;
}

int SCDetectSignatureSetAppProto(Signature *s, AppProto alproto)
{
    if (!AppProtoIsValid(alproto)) {
        SCLogError("invalid alproto %u", alproto);
        return -1;
    }

    if (s->init_data->alprotos[0] != ALPROTO_UNKNOWN) {
        // Multiple alprotos were set, check if we restrict to one
        bool found = false;
        for (AppProto i = 0; i < SIG_ALPROTO_MAX; i++) {
            if (s->init_data->alprotos[i] == alproto) {
                found = true;
                break;
            }
        }
        if (!found) {
            // fail if we set to a alproto which was not in the set
            return -1;
        }
        // we will use s->alproto if there is a single alproto and
        // we reset s->init_data->alprotos to signal there are no longer multiple alprotos
        s->init_data->alprotos[0] = ALPROTO_UNKNOWN;
    }

    if (s->alproto != ALPROTO_UNKNOWN) {
        alproto = AppProtoCommon(s->alproto, alproto);
        if (alproto == ALPROTO_FAILED) {
            SCLogError("can't set rule app proto to %s: already set to %s",
                    AppProtoToString(alproto), AppProtoToString(s->alproto));
            return -1;
        }
    }

    if (AppLayerProtoDetectGetProtoName(alproto) == NULL) {
        SCLogError("disabled alproto %s, rule can never match", AppProtoToString(alproto));
        return -1;
    }
    s->alproto = alproto;
    s->flags |= SIG_FLAG_APPLAYER;
    return 0;
}

static DetectMatchAddressIPv4 *SigBuildAddressMatchArrayIPv4(
        const DetectAddress *head, uint16_t *match4_cnt)
{
    uint16_t cnt = 0;

    for (const DetectAddress *da = head; da != NULL; da = da->next) {
        cnt++;
    }
    if (cnt == 0) {
        return NULL;
    }
    DetectMatchAddressIPv4 *addr_match4 = SCCalloc(cnt, sizeof(DetectMatchAddressIPv4));
    if (addr_match4 == NULL) {
        exit(EXIT_FAILURE);
    }

    uint16_t idx = 0;
    for (const DetectAddress *da = head; da != NULL; da = da->next) {
        addr_match4[idx].ip = SCNtohl(da->ip.addr_data32[0]);
        addr_match4[idx].ip2 = SCNtohl(da->ip2.addr_data32[0]);
        idx++;
    }
    *match4_cnt = cnt;
    return addr_match4;
}

static DetectMatchAddressIPv6 *SigBuildAddressMatchArrayIPv6(
        const DetectAddress *head, uint16_t *match6_cnt)
{
    uint16_t cnt = 0;
    for (const DetectAddress *da = head; da != NULL; da = da->next) {
        cnt++;
    }
    if (cnt == 0) {
        return NULL;
    }

    DetectMatchAddressIPv6 *addr_match6 = SCCalloc(cnt, sizeof(DetectMatchAddressIPv6));
    if (addr_match6 == NULL) {
        exit(EXIT_FAILURE);
    }

    uint16_t idx = 0;
    for (const DetectAddress *da = head; da != NULL; da = da->next) {
        addr_match6[idx].ip[0] = SCNtohl(da->ip.addr_data32[0]);
        addr_match6[idx].ip[1] = SCNtohl(da->ip.addr_data32[1]);
        addr_match6[idx].ip[2] = SCNtohl(da->ip.addr_data32[2]);
        addr_match6[idx].ip[3] = SCNtohl(da->ip.addr_data32[3]);
        addr_match6[idx].ip2[0] = SCNtohl(da->ip2.addr_data32[0]);
        addr_match6[idx].ip2[1] = SCNtohl(da->ip2.addr_data32[1]);
        addr_match6[idx].ip2[2] = SCNtohl(da->ip2.addr_data32[2]);
        addr_match6[idx].ip2[3] = SCNtohl(da->ip2.addr_data32[3]);
        idx++;
    }
    *match6_cnt = cnt;
    return addr_match6;
}

/**
 *  \internal
 *  \brief build address match array for cache efficient matching
 *
 *  \param s the signature
 */
static void SigBuildAddressMatchArray(Signature *s)
{
    /* source addresses */
    s->addr_src_match4 =
            SigBuildAddressMatchArrayIPv4(s->init_data->src->ipv4_head, &s->addr_src_match4_cnt);
    /* destination addresses */
    s->addr_dst_match4 =
            SigBuildAddressMatchArrayIPv4(s->init_data->dst->ipv4_head, &s->addr_dst_match4_cnt);

    /* source addresses IPv6 */
    s->addr_src_match6 =
            SigBuildAddressMatchArrayIPv6(s->init_data->src->ipv6_head, &s->addr_src_match6_cnt);
    /* destination addresses IPv6 */
    s->addr_dst_match6 =
            SigBuildAddressMatchArrayIPv6(s->init_data->dst->ipv6_head, &s->addr_dst_match6_cnt);
}

static int SigMatchListLen(SigMatch *sm)
{
    int len = 0;
    for (; sm != NULL; sm = sm->next)
        len++;

    return len;
}

/** \brief convert SigMatch list to SigMatchData array
 *  \note ownership of sm->ctx is transferred to smd->ctx
 */
SigMatchData* SigMatchList2DataArray(SigMatch *head)
{
    int len = SigMatchListLen(head);
    if (len == 0)
        return NULL;

    SigMatchData *smd = (SigMatchData *)SCCalloc(len, sizeof(SigMatchData));
    if (smd == NULL) {
        FatalError("initializing the detection engine failed");
    }
    SigMatchData *out = smd;

    /* Copy sm type and Context into array */
    SigMatch *sm = head;
    for (; sm != NULL; sm = sm->next, smd++) {
        smd->type = sm->type;
        smd->ctx = sm->ctx;
        sm->ctx = NULL; // SigMatch no longer owns the ctx
        smd->is_last = (sm->next == NULL);
    }
    return out;
}

extern int g_skip_prefilter;

static void SigSetupPrefilter(DetectEngineCtx *de_ctx, Signature *s)
{
    SCEnter();
    SCLogDebug("s %u: set up prefilter/mpm", s->id);
    DEBUG_VALIDATE_BUG_ON(s->init_data->mpm_sm != NULL);

    if (s->init_data->prefilter_sm != NULL) {
        if (s->init_data->prefilter_sm->type == DETECT_CONTENT) {
            RetrieveFPForSig(de_ctx, s);
            if (s->init_data->mpm_sm != NULL) {
                s->flags |= SIG_FLAG_PREFILTER;
                SCLogDebug("%u: RetrieveFPForSig set", s->id);
                SCReturn;
            }
            /* fall through, this can happen if the mpm doesn't support the pattern */
        } else {
            s->flags |= SIG_FLAG_PREFILTER;
            SCReturn;
        }
    } else {
        SCLogDebug("%u: RetrieveFPForSig", s->id);
        RetrieveFPForSig(de_ctx, s);
        if (s->init_data->mpm_sm != NULL) {
            s->flags |= SIG_FLAG_PREFILTER;
            SCLogDebug("%u: RetrieveFPForSig set", s->id);
            SCReturn;
        }
    }

    SCLogDebug("s %u: no mpm; prefilter? de_ctx->prefilter_setting %u "
               "s->init_data->has_possible_prefilter %s",
            s->id, de_ctx->prefilter_setting, BOOL2STR(s->init_data->has_possible_prefilter));

    if (!s->init_data->has_possible_prefilter || g_skip_prefilter)
        SCReturn;

    DEBUG_VALIDATE_BUG_ON(s->flags & SIG_FLAG_PREFILTER);
    if (de_ctx->prefilter_setting == DETECT_PREFILTER_AUTO) {
        int prefilter_list = DETECT_TBLSIZE;
        /* get the keyword supporting prefilter with the lowest type */
        for (int i = 0; i < DETECT_SM_LIST_MAX; i++) {
            for (SigMatch *sm = s->init_data->smlists[i]; sm != NULL; sm = sm->next) {
                if (sigmatch_table[sm->type].SupportsPrefilter != NULL) {
                    if (sigmatch_table[sm->type].SupportsPrefilter(s)) {
                        prefilter_list = MIN(prefilter_list, sm->type);
                    }
                }
            }
        }

        /* apply that keyword as prefilter */
        if (prefilter_list != DETECT_TBLSIZE) {
            for (int i = 0; i < DETECT_SM_LIST_MAX; i++) {
                for (SigMatch *sm = s->init_data->smlists[i]; sm != NULL; sm = sm->next) {
                    if (sm->type == prefilter_list) {
                        s->init_data->prefilter_sm = sm;
                        s->flags |= SIG_FLAG_PREFILTER;
                        SCLogConfig("sid %u: prefilter is on \"%s\"", s->id,
                                sigmatch_table[sm->type].name);
                        break;
                    }
                }
            }
        }
    }
    SCReturn;
}

/** \internal
 *  \brief check if signature's table requirement is supported by each of the keywords it uses.
 */
static bool DetectRuleValidateTable(const Signature *s)
{
    if (s->detect_table == 0)
        return true;

    const uint8_t table_as_flag = BIT_U8(s->detect_table);

    for (SigMatch *sm = s->init_data->smlists[DETECT_SM_LIST_MATCH]; sm != NULL; sm = sm->next) {
        const uint8_t kw_tables_supported = sigmatch_table[sm->type].tables;
        if (kw_tables_supported != 0 && (kw_tables_supported & table_as_flag) == 0) {
            SCLogError("rule %u uses hook \"%s\", but keyword \"%s\" doesn't support this hook",
                    s->id, DetectTableToString(s->detect_table), sigmatch_table[sm->type].name);
            return false;
        }
    }
    return true;
}

static bool DetectFirewallRuleValidate(const DetectEngineCtx *de_ctx, const Signature *s)
{
    if (s->init_data->hook.type == SIGNATURE_HOOK_TYPE_NOT_SET) {
        SCLogError("rule %u is loaded as a firewall rule, but does not specify an "
                   "explicit hook",
                s->id);
        return false;
    }
    return true;
}

static void DetectRuleSetTable(Signature *s)
{
    enum DetectTable table;
    if (s->flags & SIG_FLAG_FIREWALL) {
        if (s->type == SIG_TYPE_PKT) {
            if (s->init_data->hook.type == SIGNATURE_HOOK_TYPE_PKT &&
                    s->init_data->hook.t.pkt.ph == SIGNATURE_HOOK_PKT_PRE_STREAM)
                table = DETECT_TABLE_PACKET_PRE_STREAM;
            else if (s->init_data->hook.type == SIGNATURE_HOOK_TYPE_PKT &&
                     s->init_data->hook.t.pkt.ph == SIGNATURE_HOOK_PKT_PRE_FLOW)
                table = DETECT_TABLE_PACKET_PRE_FLOW;
            else
                table = DETECT_TABLE_PACKET_FILTER;
        } else if (s->type == SIG_TYPE_APP_TX) {
            table = DETECT_TABLE_APP_FILTER;
        } else {
            BUG_ON(1);
        }
    } else {
        // TODO pre_flow/pre_stream
        if (s->type != SIG_TYPE_APP_TX) {
            table = DETECT_TABLE_PACKET_TD;
        } else {
            table = DETECT_TABLE_APP_TD;
        }
    }

    s->detect_table = (uint8_t)table;
}

static int SigValidateFirewall(const DetectEngineCtx *de_ctx, const Signature *s)
{
    if (s->init_data->firewall_rule) {
        if (!DetectFirewallRuleValidate(de_ctx, s))
            SCReturnInt(0);
    }
    SCReturnInt(1);
}

static int SigValidateCheckBuffers(
        DetectEngineCtx *de_ctx, const Signature *s, int *ts_excl, int *tc_excl, int *dir_amb)
{
    bool has_frame = false;
    bool has_app = false;
    bool has_pkt = false;
    bool has_pmatch = false;

    int nlists = 0;
    for (uint32_t x = 0; x < s->init_data->buffer_index; x++) {
        nlists = MAX(nlists, (int)s->init_data->buffers[x].id);
    }
    nlists += (nlists > 0);
    SCLogDebug("nlists %d", nlists);

    if (s->init_data->curbuf && s->init_data->curbuf->head == NULL) {
        SCLogError("rule %u setup buffer %s but didn't add matches to it", s->id,
                DetectEngineBufferTypeGetNameById(de_ctx, s->init_data->curbuf->id));
        SCReturnInt(0);
    }

    /* run buffer type validation callbacks if any */
    if (s->init_data->smlists[DETECT_SM_LIST_PMATCH]) {
        if (!DetectContentPMATCHValidateCallback(s))
            SCReturnInt(0);

        has_pmatch = true;
    }

    struct BufferVsDir {
        int ts;
        int tc;
    } bufdir[nlists + 1];
    memset(&bufdir, 0, (nlists + 1) * sizeof(struct BufferVsDir));

    for (uint32_t x = 0; x < s->init_data->buffer_index; x++) {
        SignatureInitDataBuffer *b = &s->init_data->buffers[x];
        const DetectBufferType *bt = DetectEngineBufferTypeGetById(de_ctx, b->id);
        if (bt == NULL) {
            DEBUG_VALIDATE_BUG_ON(1); // should be impossible
            continue;
        }
        SCLogDebug("x %u b->id %u name %s", x, b->id, bt->name);
        for (const SigMatch *sm = b->head; sm != NULL; sm = sm->next) {
            SCLogDebug("sm %u %s", sm->type, sigmatch_table[sm->type].name);
        }

        if (b->head == NULL) {
            SCLogError("no matches in sticky buffer %s", bt->name);
            SCReturnInt(0);
        }

        has_frame |= bt->frame;
        has_app |= (!bt->frame && !bt->packet);
        has_pkt |= bt->packet;

        if ((s->flags & SIG_FLAG_REQUIRE_PACKET) && !bt->packet) {
            SCLogError("Signature combines packet "
                       "specific matches (like dsize, flags, ttl) with stream / "
                       "state matching by matching on app layer proto (like using "
                       "http_* keywords).");
            SCReturnInt(0);
        }

        const DetectEngineAppInspectionEngine *app = de_ctx->app_inspect_engines;
        for (; app != NULL; app = app->next) {
            if (app->sm_list == b->id &&
                    (AppProtoEquals(s->alproto, app->alproto) || s->alproto == 0)) {
                SCLogDebug("engine %s dir %d alproto %d",
                        DetectEngineBufferTypeGetNameById(de_ctx, app->sm_list), app->dir,
                        app->alproto);
                SCLogDebug("b->id %d nlists %d", b->id, nlists);

                if (b->only_tc) {
                    if (app->dir == 1)
                        (*tc_excl)++;
                } else if (b->only_ts) {
                    if (app->dir == 0)
                        (*ts_excl)++;
                } else {
                    bufdir[b->id].ts += (app->dir == 0);
                    bufdir[b->id].tc += (app->dir == 1);
                }

                /* only allow rules to use the hook for engines at that
                 * exact progress for now. */
                if (s->init_data->hook.type == SIGNATURE_HOOK_TYPE_APP) {
                    if ((s->flags & SIG_FLAG_TOSERVER) && (app->dir == 0) &&
                            app->progress != s->init_data->hook.t.app.app_progress) {
                        SCLogError("engine progress value %d doesn't match hook %u", app->progress,
                                s->init_data->hook.t.app.app_progress);
                        SCReturnInt(0);
                    }
                    if ((s->flags & SIG_FLAG_TOCLIENT) && (app->dir == 1) &&
                            app->progress != s->init_data->hook.t.app.app_progress) {
                        SCLogError("engine progress value doesn't match hook");
                        SCReturnInt(0);
                    }
                }
            }
        }

        if (!DetectEngineBufferRunValidateCallback(de_ctx, b->id, s, &de_ctx->sigerror)) {
            SCReturnInt(0);
        }

        if (!DetectBsizeValidateContentCallback(s, b)) {
            SCReturnInt(0);
        }
        if (!DetectAbsentValidateContentCallback(s, b)) {
            SCReturnInt(0);
        }
    }

    if (has_pmatch && has_frame) {
        SCLogError("can't mix pure content and frame inspection");
        SCReturnInt(0);
    }
    if (has_app && has_frame) {
        SCLogError("can't mix app-layer buffer and frame inspection");
        SCReturnInt(0);
    }
    if (has_pkt && has_frame) {
        SCLogError("can't mix pkt buffer and frame inspection");
        SCReturnInt(0);
    }

    for (int x = 0; x < nlists; x++) {
        if (bufdir[x].ts == 0 && bufdir[x].tc == 0)
            continue;
        (*ts_excl) += (bufdir[x].ts > 0 && bufdir[x].tc == 0);
        (*tc_excl) += (bufdir[x].ts == 0 && bufdir[x].tc > 0);
        (*dir_amb) += (bufdir[x].ts > 0 && bufdir[x].tc > 0);

        SCLogDebug("%s/%d: %d/%d", DetectEngineBufferTypeGetNameById(de_ctx, x), x, bufdir[x].ts,
                bufdir[x].tc);
    }

    SCReturnInt(1);
}

static int SigValidatePacketStream(const Signature *s)
{
    if ((s->flags & SIG_FLAG_REQUIRE_PACKET) && (s->flags & SIG_FLAG_REQUIRE_STREAM)) {
        SCLogError("can't mix packet keywords with "
                   "tcp-stream or flow:only_stream.  Invalidating signature.");
        SCReturnInt(0);
    }
    SCReturnInt(1);
}

static int SigConsolidateDirection(
        Signature *s, const int ts_excl, const int tc_excl, const int dir_amb)
{
    if (s->flags & SIG_FLAG_TXBOTHDIR) {
        if (!ts_excl || !tc_excl) {
            SCLogError("rule %u should use both directions, but does not", s->id);
            SCReturnInt(0);
        }
        if (dir_amb) {
            SCLogError("rule %u means to use both directions, cannot have keywords ambiguous about "
                       "directions",
                    s->id);
            SCReturnInt(0);
        }
    } else if (ts_excl && tc_excl) {
        SCLogError(
                "rule %u mixes keywords with conflicting directions, a transactional rule with => "
                "should be used",
                s->id);
        SCReturnInt(0);
    } else if (ts_excl) {
        SCLogDebug("%u: implied rule direction is toserver", s->id);
        if (DetectFlowSetupImplicit(s, SIG_FLAG_TOSERVER) < 0) {
            SCLogError("rule %u mixes keywords with conflicting directions", s->id);
            SCReturnInt(0);
        }
    } else if (tc_excl) {
        SCLogDebug("%u: implied rule direction is toclient", s->id);
        if (DetectFlowSetupImplicit(s, SIG_FLAG_TOCLIENT) < 0) {
            SCLogError("rule %u mixes keywords with conflicting directions", s->id);
            SCReturnInt(0);
        }
    } else if (dir_amb) {
        SCLogDebug("%u: rule direction cannot be deduced from keywords", s->id);
    }
    SCReturnInt(1);
}

static void SigConsolidateTcpBuffer(Signature *s)
{
    /* TCP: corner cases:
     * - pkt vs stream vs depth/offset
     * - pkt vs stream vs stream_size
     */
    if (s->proto.proto[IPPROTO_TCP / 8] & (1 << (IPPROTO_TCP % 8))) {
        if (s->init_data->smlists[DETECT_SM_LIST_PMATCH]) {
            if (!(s->flags & (SIG_FLAG_REQUIRE_PACKET | SIG_FLAG_REQUIRE_STREAM))) {
                s->flags |= SIG_FLAG_REQUIRE_STREAM;
                for (const SigMatch *sm = s->init_data->smlists[DETECT_SM_LIST_PMATCH]; sm != NULL;
                        sm = sm->next) {
                    if (sm->type == DETECT_CONTENT &&
                            (((DetectContentData *)(sm->ctx))->flags &
                             (DETECT_CONTENT_DEPTH | DETECT_CONTENT_OFFSET))) {
                        s->flags |= SIG_FLAG_REQUIRE_PACKET;
                        break;
                    }
                }
                /* if stream_size is in use, also inspect packets */
                for (const SigMatch *sm = s->init_data->smlists[DETECT_SM_LIST_MATCH]; sm != NULL;
                        sm = sm->next) {
                    if (sm->type == DETECT_STREAM_SIZE) {
                        s->flags |= SIG_FLAG_REQUIRE_PACKET;
                        break;
                    }
                }
            }
        }
    }
}

static bool SigInspectsFiles(const Signature *s)
{
    return ((s->flags & SIG_FLAG_FILESTORE) || s->file_flags != 0 ||
            (s->init_data->init_flags & SIG_FLAG_INIT_FILEDATA));
}

/** \internal
 *  \brief validate file handling
 *  \retval 1 good signature
 *  \retval 0 bad signature
 */
static int SigValidateFileHandling(const Signature *s)
{
    if (!SigInspectsFiles(s)) {
        SCReturnInt(1);
    }

    if (s->alproto != ALPROTO_UNKNOWN && !AppLayerParserSupportsFiles(IPPROTO_TCP, s->alproto)) {
        SCLogError("protocol %s doesn't "
                   "support file matching",
                AppProtoToString(s->alproto));
        SCReturnInt(0);
    }
    if (s->init_data->alprotos[0] != ALPROTO_UNKNOWN) {
        bool found = false;
        for (AppProto i = 0; i < SIG_ALPROTO_MAX; i++) {
            if (s->init_data->alprotos[i] == ALPROTO_UNKNOWN) {
                break;
            }
            if (AppLayerParserSupportsFiles(IPPROTO_TCP, s->init_data->alprotos[i])) {
                found = true;
                break;
            }
        }
        if (!found) {
            SCLogError("No protocol support file matching");
            SCReturnInt(0);
        }
    }
    if (s->alproto == ALPROTO_HTTP2 && (s->file_flags & FILE_SIG_NEED_FILENAME)) {
        SCLogError("protocol HTTP2 doesn't support file name matching");
        SCReturnInt(0);
    }
    SCReturnInt(1);
}

/**
 *  \internal
 *  \brief validate and consolidate parsed signature
 *
 *  \param de_ctx detect engine
 *  \param s signature to validate and consolidate
 *
 *  \retval 0 invalid
 *  \retval 1 valid
 */
static int SigValidateConsolidate(
        DetectEngineCtx *de_ctx, Signature *s, const SignatureParser *parser, const uint8_t dir)
{
    SCEnter();

    if (SigValidateFirewall(de_ctx, s) == 0)
        SCReturnInt(0);

    if (SigValidatePacketStream(s) == 0) {
        SCReturnInt(0);
    }

    int ts_excl = 0;
    int tc_excl = 0;
    int dir_amb = 0;

    if (SigValidateCheckBuffers(de_ctx, s, &ts_excl, &tc_excl, &dir_amb) == 0) {
        SCReturnInt(0);
    }

    if (SigConsolidateDirection(s, ts_excl, tc_excl, dir_amb) == 0) {
        SCReturnInt(0);
    }

    SigConsolidateTcpBuffer(s);

    SignatureSetType(de_ctx, s);
    DetectRuleSetTable(s);

    int r = SigValidateFileHandling(s);
    if (r == 0) {
        SCReturnInt(0);
    }
    if (SigInspectsFiles(s)) {
        if (s->alproto == ALPROTO_HTTP1 || s->alproto == ALPROTO_HTTP) {
            AppLayerHtpNeedFileInspection();
        }
    }
    if (DetectRuleValidateTable(s) == false) {
        SCReturnInt(0);
    }

    if (s->type == SIG_TYPE_IPONLY) {
        /* For IPOnly */
        if (IPOnlySigParseAddress(de_ctx, s, parser->src, SIG_DIREC_SRC ^ dir) < 0)
            SCReturnInt(0);

        if (IPOnlySigParseAddress(de_ctx, s, parser->dst, SIG_DIREC_DST ^ dir) < 0)
            SCReturnInt(0);
    }
    SCReturnInt(1);
}

/**
 * \internal
 * \brief Helper function for SigInit().
 */
static Signature *SigInitHelper(
        DetectEngineCtx *de_ctx, const char *sigstr, uint8_t dir, const bool firewall_rule)
{
    SignatureParser parser;
    memset(&parser, 0x00, sizeof(parser));

    Signature *sig = SigAlloc();
    if (sig == NULL)
        goto error;
    if (firewall_rule) {
        sig->init_data->firewall_rule = true;
        sig->flags |= SIG_FLAG_FIREWALL;
    }

    sig->sig_str = SCStrdup(sigstr);
    if (unlikely(sig->sig_str == NULL)) {
        goto error;
    }

    /* default gid to 1 */
    sig->gid = 1;

    /* We do a first parse of the rule in a requires, or scan-only
     * mode. Syntactic errors will be picked up here, but the only
     * part of the rule that is validated completely is the "requires"
     * keyword. */
    int ret = SigParse(de_ctx, sig, sigstr, dir, &parser, true);
    if (ret == -4) {
        /* Rule requirements not met. */
        de_ctx->sigerror_silent = true;
        de_ctx->sigerror_ok = true;
        de_ctx->sigerror_requires = true;
        goto error;
    } else if (ret < 0) {
        goto error;
    }

    /* Check for a SID before continuuing. */
    if (sig->id == 0) {
        SCLogError("Signature missing required value \"sid\".");
        goto error;
    }

    /* Now completely parse the rule. */
    ret = SigParse(de_ctx, sig, sigstr, dir, &parser, false);
    BUG_ON(ret == -4);
    if (ret == -3) {
        de_ctx->sigerror_silent = true;
        de_ctx->sigerror_ok = true;
        goto error;
    } else if (ret == -2) {
        de_ctx->sigerror_silent = true;
        goto error;
    } else if (ret < 0) {
        goto error;
    }

    /* signature priority hasn't been overwritten.  Using default priority */
    if (sig->prio == -1)
        sig->prio = DETECT_DEFAULT_PRIO;

    sig->iid = de_ctx->signum;
    de_ctx->signum++;

    if (sig->alproto != ALPROTO_UNKNOWN) {
        int override_needed = 0;
        if (sig->proto.flags & DETECT_PROTO_ANY) {
            sig->proto.flags &= ~DETECT_PROTO_ANY;
            memset(sig->proto.proto, 0x00, sizeof(sig->proto.proto));
            override_needed = 1;
        } else {
            override_needed = 1;
            size_t s = 0;
            for (s = 0; s < sizeof(sig->proto.proto); s++) {
                if (sig->proto.proto[s] != 0x00) {
                    override_needed = 0;
                    break;
                }
            }
        }

        /* at this point if we had alert ip and the ip proto was not
         * overridden, we use the ip proto that has been configured
         * against the app proto in use. */
        if (override_needed)
            AppLayerProtoDetectSupportedIpprotos(sig->alproto, sig->proto.proto);
    }

    /* set the packet and app layer flags, but only if the
     * app layer flag wasn't already set in which case we
     * only consider the app layer */
    if (!(sig->flags & SIG_FLAG_APPLAYER)) {
        if (sig->init_data->smlists[DETECT_SM_LIST_MATCH] != NULL) {
            SigMatch *sm = sig->init_data->smlists[DETECT_SM_LIST_MATCH];
            for ( ; sm != NULL; sm = sm->next) {
                if (sigmatch_table[sm->type].Match != NULL)
                    sig->init_data->init_flags |= SIG_FLAG_INIT_PACKET;
            }
        } else {
            sig->init_data->init_flags |= SIG_FLAG_INIT_PACKET;
        }
    }

    if (sig->init_data->hook.type == SIGNATURE_HOOK_TYPE_PKT) {
        if (sig->init_data->hook.t.pkt.ph == SIGNATURE_HOOK_PKT_FLOW_START) {
            if ((sig->flags & SIG_FLAG_TOSERVER) != 0) {
                sig->init_data->init_flags |= SIG_FLAG_INIT_FLOW;
            }
        }
    }

    if (!(sig->init_data->init_flags & SIG_FLAG_INIT_FLOW)) {
        if ((sig->flags & (SIG_FLAG_TOSERVER|SIG_FLAG_TOCLIENT)) == 0) {
            sig->flags |= SIG_FLAG_TOSERVER;
            sig->flags |= SIG_FLAG_TOCLIENT;
        }
    }

    SCLogDebug("sig %"PRIu32" SIG_FLAG_APPLAYER: %s, SIG_FLAG_PACKET: %s",
        sig->id, sig->flags & SIG_FLAG_APPLAYER ? "set" : "not set",
        sig->init_data->init_flags & SIG_FLAG_INIT_PACKET ? "set" : "not set");

    SigBuildAddressMatchArray(sig);

    /* run buffer type callbacks if any */
    for (uint32_t x = 0; x < DETECT_SM_LIST_MAX; x++) {
        if (sig->init_data->smlists[x])
            DetectEngineBufferRunSetupCallback(de_ctx, x, sig);
    }
    for (uint32_t x = 0; x < sig->init_data->buffer_index; x++) {
        DetectEngineBufferRunSetupCallback(de_ctx, sig->init_data->buffers[x].id, sig);
    }

    SigSetupPrefilter(de_ctx, sig);

    /* validate signature, SigValidate will report the error reason */
    if (SigValidateConsolidate(de_ctx, sig, &parser, dir) == 0) {
        goto error;
    }

    return sig;

error:
    if (sig != NULL) {
        SigFree(de_ctx, sig);
    }
    return NULL;
}

/**
 * \brief Checks if a signature has the same source and destination
 * \param s parsed signature
 *
 *  \retval true if source and destination are the same, false otherwise
 */
static bool SigHasSameSourceAndDestination(const Signature *s)
{
    if (!(s->flags & SIG_FLAG_SP_ANY) || !(s->flags & SIG_FLAG_DP_ANY)) {
        if (!DetectPortListsAreEqual(s->sp, s->dp)) {
            return false;
        }
    }

    if (!(s->flags & SIG_FLAG_SRC_ANY) || !(s->flags & SIG_FLAG_DST_ANY)) {
        DetectAddress *src = s->init_data->src->ipv4_head;
        DetectAddress *dst = s->init_data->dst->ipv4_head;

        if (!DetectAddressListsAreEqual(src, dst)) {
            return false;
        }

        src = s->init_data->src->ipv6_head;
        dst = s->init_data->dst->ipv6_head;

        if (!DetectAddressListsAreEqual(src, dst)) {
            return false;
        }
    }

    return true;
}

static Signature *SigInitDo(DetectEngineCtx *de_ctx, const char *sigstr, const bool firewall_rule)
{
    SCEnter();

    uint32_t oldsignum = de_ctx->signum;
    de_ctx->sigerror_ok = false;
    de_ctx->sigerror_silent = false;
    de_ctx->sigerror_requires = false;

    Signature *sig = SigInitHelper(de_ctx, sigstr, SIG_DIREC_NORMAL, firewall_rule);
    if (sig == NULL) {
        goto error;
    }

    if (sig->init_data->init_flags & SIG_FLAG_INIT_BIDIREC) {
        if (SigHasSameSourceAndDestination(sig)) {
            SCLogInfo("Rule with ID %u is bidirectional, but source and destination are the same, "
                "treating the rule as unidirectional", sig->id);

            sig->init_data->init_flags &= ~SIG_FLAG_INIT_BIDIREC;
        } else {
            sig->next = SigInitHelper(de_ctx, sigstr, SIG_DIREC_SWITCHED, firewall_rule);
            if (sig->next == NULL) {
                goto error;
            }
        }
    }

    SCReturnPtr(sig, "Signature");

error:
    if (sig != NULL) {
        SigFree(de_ctx, sig);
    }
    /* if something failed, restore the old signum count
     * since we didn't install it */
    de_ctx->signum = oldsignum;

    SCReturnPtr(NULL, "Signature");
}

/**
 * \brief Parses a signature and adds it to the Detection Engine Context.
 *
 * \param de_ctx Pointer to the Detection Engine Context.
 * \param sigstr Pointer to a character string containing the signature to be
 *               parsed.
 *
 * \retval Pointer to the Signature instance on success; NULL on failure.
 */
Signature *SigInit(DetectEngineCtx *de_ctx, const char *sigstr)
{
    return SigInitDo(de_ctx, sigstr, false);
}

static Signature *DetectFirewallRuleNew(DetectEngineCtx *de_ctx, const char *sigstr)
{
    return SigInitDo(de_ctx, sigstr, true);
}

/**
 * \brief The hash free function to be the used by the hash table -
 *        DetectEngineCtx->dup_sig_hash_table.
 *
 * \param data    Pointer to the data, in our case SigDuplWrapper to be freed.
 */
static void DetectParseDupSigFreeFunc(void *data)
{
    if (data != NULL)
        SCFree(data);
}

/**
 * \brief The hash function to be the used by the hash table -
 *        DetectEngineCtx->dup_sig_hash_table.
 *
 * \param ht      Pointer to the hash table.
 * \param data    Pointer to the data, in our case SigDuplWrapper.
 * \param datalen Not used in our case.
 *
 * \retval sw->s->id The generated hash value.
 */
static uint32_t DetectParseDupSigHashFunc(HashListTable *ht, void *data, uint16_t datalen)
{
    SigDuplWrapper *sw = (SigDuplWrapper *)data;

    return (sw->s->id % ht->array_size);
}

/**
 * \brief The Compare function to be used by the  hash table -
 *        DetectEngineCtx->dup_sig_hash_table.
 *
 * \param data1 Pointer to the first SigDuplWrapper.
 * \param len1  Not used.
 * \param data2 Pointer to the second SigDuplWrapper.
 * \param len2  Not used.
 *
 * \retval 1 If the 2 SigDuplWrappers sent as args match.
 * \retval 0 If the 2 SigDuplWrappers sent as args do not match.
 */
static char DetectParseDupSigCompareFunc(void *data1, uint16_t len1, void *data2,
                                  uint16_t len2)
{
    SigDuplWrapper *sw1 = (SigDuplWrapper *)data1;
    SigDuplWrapper *sw2 = (SigDuplWrapper *)data2;

    if (sw1 == NULL || sw2 == NULL ||
        sw1->s == NULL || sw2->s == NULL)
        return 0;

    /* sid and gid match required */
    if (sw1->s->id == sw2->s->id && sw1->s->gid == sw2->s->gid) return 1;

    return 0;
}

/**
 * \brief Initializes the hash table that is used to cull duplicate sigs.
 *
 * \param de_ctx Pointer to the detection engine context.
 *
 * \retval  0 On success.
 * \retval -1 On failure.
 */
int DetectParseDupSigHashInit(DetectEngineCtx *de_ctx)
{
    de_ctx->dup_sig_hash_table = HashListTableInit(15000,
                                                   DetectParseDupSigHashFunc,
                                                   DetectParseDupSigCompareFunc,
                                                   DetectParseDupSigFreeFunc);
    if (de_ctx->dup_sig_hash_table == NULL)
        return -1;

    return 0;
}

/**
 * \brief Frees the hash table that is used to cull duplicate sigs.
 *
 * \param de_ctx Pointer to the detection engine context that holds this table.
 */
void DetectParseDupSigHashFree(DetectEngineCtx *de_ctx)
{
    if (de_ctx->dup_sig_hash_table != NULL)
        HashListTableFree(de_ctx->dup_sig_hash_table);

    de_ctx->dup_sig_hash_table = NULL;
}

/**
 * \brief Check if a signature is a duplicate.
 *
 *        There are 3 types of return values for this function.
 *
 *        - 0, which indicates that the Signature is not a duplicate
 *          and has to be added to the detection engine list.
 *        - 1, Signature is duplicate, and the existing signature in
 *          the list shouldn't be replaced with this duplicate.
 *        - 2, Signature is duplicate, and the existing signature in
 *          the list should be replaced with this duplicate.
 *
 * \param de_ctx Pointer to the detection engine context.
 * \param sig    Pointer to the Signature that has to be checked.
 *
 * \retval 2 If Signature is duplicate and the existing signature in
 *           the list should be chucked out and replaced with this.
 * \retval 1 If Signature is duplicate, and should be chucked out.
 * \retval 0 If Signature is not a duplicate.
 */
static inline int DetectEngineSignatureIsDuplicate(DetectEngineCtx *de_ctx,
                                                   Signature *sig)
{
    /* we won't do any NULL checks on the args */

    /* return value */
    int ret = 0;

    SigDuplWrapper *sw_dup = NULL;
    SigDuplWrapper *sw = NULL;

    /* used for making a duplicate_sig_hash_table entry */
    sw = SCCalloc(1, sizeof(SigDuplWrapper));
    if (unlikely(sw == NULL)) {
        exit(EXIT_FAILURE);
    }
    sw->s = sig;

    /* check if we have a duplicate entry for this signature */
    sw_dup = HashListTableLookup(de_ctx->dup_sig_hash_table, (void *)sw, 0);
    /* we don't have a duplicate entry for this sig */
    if (sw_dup == NULL) {
        /* add it to the hash table */
        HashListTableAdd(de_ctx->dup_sig_hash_table, (void *)sw, 0);

        /* add the s_prev entry for the previously loaded sw in the hash_table */
        if (de_ctx->sig_list != NULL) {
            SigDuplWrapper *sw_old = NULL;
            SigDuplWrapper sw_tmp;
            memset(&sw_tmp, 0, sizeof(SigDuplWrapper));

            /* the topmost sig would be the last loaded sig */
            sw_tmp.s = de_ctx->sig_list;
            sw_old = HashListTableLookup(de_ctx->dup_sig_hash_table,
                                         (void *)&sw_tmp, 0);
            /* sw_old == NULL case is impossible */
            sw_old->s_prev = sig;
        }

        ret = 0;
        goto end;
    }

    /* if we have reached here we have a duplicate entry for this signature.
     * Check the signature revision.  Store the signature with the latest rev
     * and discard the other one */
    if (sw->s->rev <= sw_dup->s->rev) {
        ret = 1;
        SCFree(sw);
        sw = NULL;
        goto end;
    }

    /* the new sig is of a newer revision than the one that is already in the
     * list.  Remove the old sig from the list */
    if (sw_dup->s_prev == NULL) {
        SigDuplWrapper sw_temp;
        memset(&sw_temp, 0, sizeof(SigDuplWrapper));
        if (sw_dup->s->init_data->init_flags & SIG_FLAG_INIT_BIDIREC) {
            sw_temp.s = sw_dup->s->next->next;
            de_ctx->sig_list = sw_dup->s->next->next;
            SigFree(de_ctx, sw_dup->s->next);
        } else {
            sw_temp.s = sw_dup->s->next;
            de_ctx->sig_list = sw_dup->s->next;
        }
        SigDuplWrapper *sw_next = NULL;
        if (sw_temp.s != NULL) {
            sw_next = HashListTableLookup(de_ctx->dup_sig_hash_table,
                                          (void *)&sw_temp, 0);
            sw_next->s_prev = sw_dup->s_prev;
        }
        SigFree(de_ctx, sw_dup->s);
    } else {
        SigDuplWrapper sw_temp;
        memset(&sw_temp, 0, sizeof(SigDuplWrapper));
        if (sw_dup->s->init_data->init_flags & SIG_FLAG_INIT_BIDIREC) {
            sw_temp.s = sw_dup->s->next->next;
            /* If previous signature is bidirectional,
             * it has 2 items in the linked list.
             * So we need to change next->next instead of next
             */
            if (sw_dup->s_prev->init_data->init_flags & SIG_FLAG_INIT_BIDIREC) {
                sw_dup->s_prev->next->next = sw_dup->s->next->next;
            } else {
                sw_dup->s_prev->next = sw_dup->s->next->next;
            }
            SigFree(de_ctx, sw_dup->s->next);
        } else {
            sw_temp.s = sw_dup->s->next;
            if (sw_dup->s_prev->init_data->init_flags & SIG_FLAG_INIT_BIDIREC) {
                sw_dup->s_prev->next->next = sw_dup->s->next;
            } else {
                sw_dup->s_prev->next = sw_dup->s->next;
            }
        }
        SigDuplWrapper *sw_next = NULL;
        if (sw_temp.s != NULL) {
            sw_next = HashListTableLookup(de_ctx->dup_sig_hash_table,
                                          (void *)&sw_temp, 0);
            sw_next->s_prev = sw_dup->s_prev;
        }
        SigFree(de_ctx, sw_dup->s);
    }

    /* make changes to the entry to reflect the presence of the new sig */
    sw_dup->s = sig;
    sw_dup->s_prev = NULL;

    if (de_ctx->sig_list != NULL) {
        SigDuplWrapper sw_tmp;
        memset(&sw_tmp, 0, sizeof(SigDuplWrapper));
        sw_tmp.s = de_ctx->sig_list;
        SigDuplWrapper *sw_old = HashListTableLookup(de_ctx->dup_sig_hash_table,
                                                     (void *)&sw_tmp, 0);
        if (sw_old->s != sw_dup->s) {
            // Link on top of the list if there was another element
            sw_old->s_prev = sig;
        }
    }

    /* this is duplicate, but a duplicate that replaced the existing sig entry */
    ret = 2;

    SCFree(sw);

end:
    return ret;
}

/**
 * \brief Parse and append a Signature into the Detection Engine Context
 *        signature list.
 *
 *        If the signature is bidirectional it should append two signatures
 *        (with the addresses switched) into the list.  Also handle duplicate
 *        signatures.  In case of duplicate sigs, use the ones that have the
 *        latest revision.  We use the sid and the msg to identify duplicate
 *        sigs.  If 2 sigs have the same sid and gid, they are duplicates.
 *
 * \param de_ctx Pointer to the Detection Engine Context.
 * \param sigstr Pointer to a character string containing the signature to be
 *               parsed.
 * \param sig_file Pointer to a character string containing the filename from
 *                 which signature is read
 * \param lineno Line number from where signature is read
 *
 * \retval Pointer to the head Signature in the detection engine ctx sig_list
 *         on success; NULL on failure.
 */
Signature *DetectFirewallRuleAppendNew(DetectEngineCtx *de_ctx, const char *sigstr)
{
    Signature *sig = DetectFirewallRuleNew(de_ctx, sigstr);
    if (sig == NULL) {
        return NULL;
    }

    /* checking for the status of duplicate signature */
    int dup_sig = DetectEngineSignatureIsDuplicate(de_ctx, sig);
    /* a duplicate signature that should be chucked out.  Check the previously
     * called function details to understand the different return values */
    if (dup_sig == 1) {
        SCLogError("Duplicate signature \"%s\"", sigstr);
        goto error;
    } else if (dup_sig == 2) {
        SCLogWarning("Signature with newer revision,"
                     " so the older sig replaced by this new signature \"%s\"",
                sigstr);
    }

    if (sig->init_data->init_flags & SIG_FLAG_INIT_BIDIREC) {
        if (sig->next != NULL) {
            sig->next->next = de_ctx->sig_list;
        } else {
            goto error;
        }
    } else {
        /* if this sig is the first one, sig_list should be null */
        sig->next = de_ctx->sig_list;
    }

    de_ctx->sig_list = sig;

    /**
     * In DetectEngineAppendSig(), the signatures are prepended and we always return the first one
     * so if the signature is bidirectional, the returned sig will point through "next" ptr
     * to the cloned signatures with the switched addresses
     */
    return (dup_sig == 0 || dup_sig == 2) ? sig : NULL;

error:
    /* free the 2nd sig bidir may have set up */
    if (sig != NULL && sig->next != NULL) {
        SigFree(de_ctx, sig->next);
        sig->next = NULL;
    }
    if (sig != NULL) {
        SigFree(de_ctx, sig);
    }
    return NULL;
}

/**
 * \brief Parse and append a Signature into the Detection Engine Context
 *        signature list.
 *
 *        If the signature is bidirectional it should append two signatures
 *        (with the addresses switched) into the list.  Also handle duplicate
 *        signatures.  In case of duplicate sigs, use the ones that have the
 *        latest revision.  We use the sid and the msg to identify duplicate
 *        sigs.  If 2 sigs have the same sid and gid, they are duplicates.
 *
 * \param de_ctx Pointer to the Detection Engine Context.
 * \param sigstr Pointer to a character string containing the signature to be
 *               parsed.
 * \param sig_file Pointer to a character string containing the filename from
 *                 which signature is read
 * \param lineno Line number from where signature is read
 *
 * \retval Pointer to the head Signature in the detection engine ctx sig_list
 *         on success; NULL on failure.
 */
Signature *DetectEngineAppendSig(DetectEngineCtx *de_ctx, const char *sigstr)
{
    Signature *sig = SigInit(de_ctx, sigstr);
    if (sig == NULL) {
        return NULL;
    }

    /* checking for the status of duplicate signature */
    int dup_sig = DetectEngineSignatureIsDuplicate(de_ctx, sig);
    /* a duplicate signature that should be chucked out.  Check the previously
     * called function details to understand the different return values */
    if (dup_sig == 1) {
        SCLogError("Duplicate signature \"%s\"", sigstr);
        goto error;
    } else if (dup_sig == 2) {
        SCLogWarning("Signature with newer revision,"
                     " so the older sig replaced by this new signature \"%s\"",
                sigstr);
    }

    if (sig->init_data->init_flags & SIG_FLAG_INIT_BIDIREC) {
        if (sig->next != NULL) {
            sig->next->next = de_ctx->sig_list;
        } else {
            goto error;
        }
    } else {
        /* if this sig is the first one, sig_list should be null */
        sig->next = de_ctx->sig_list;
    }

    de_ctx->sig_list = sig;

    /**
     * In DetectEngineAppendSig(), the signatures are prepended and we always return the first one
     * so if the signature is bidirectional, the returned sig will point through "next" ptr
     * to the cloned signatures with the switched addresses
     */
    return (dup_sig == 0 || dup_sig == 2) ? sig : NULL;

error:
    /* free the 2nd sig bidir may have set up */
    if (sig != NULL && sig->next != NULL) {
        SigFree(de_ctx, sig->next);
        sig->next = NULL;
    }
    if (sig != NULL) {
        SigFree(de_ctx, sig);
    }
    return NULL;
}

static DetectParseRegex *g_detect_parse_regex_list = NULL;

int DetectParsePcreExec(DetectParseRegex *parse_regex, pcre2_match_data **match, const char *str,
        int start_offset, int options)
{
    *match = pcre2_match_data_create_from_pattern(parse_regex->regex, NULL);
    if (*match)
        return pcre2_match(parse_regex->regex, (PCRE2_SPTR8)str, strlen(str), options, start_offset,
                *match, parse_regex->context);
    return -1;
}

void DetectParseFreeRegex(DetectParseRegex *r)
{
    if (r->regex) {
        pcre2_code_free(r->regex);
    }
    if (r->context) {
        pcre2_match_context_free(r->context);
    }
}

void DetectParseFreeRegexes(void)
{
    DetectParseRegex *r = g_detect_parse_regex_list;
    while (r) {
        DetectParseRegex *next = r->next;

        DetectParseFreeRegex(r);

        SCFree(r);
        r = next;
    }
    g_detect_parse_regex_list = NULL;
}

/** \brief add regex and/or study to at exit free list
 */
void DetectParseRegexAddToFreeList(DetectParseRegex *detect_parse)
{
    DetectParseRegex *r = SCCalloc(1, sizeof(*r));
    if (r == NULL) {
        FatalError("failed to alloc memory for pcre free list");
    }
    r->regex = detect_parse->regex;
    r->next = g_detect_parse_regex_list;
    g_detect_parse_regex_list = r;
}

bool DetectSetupParseRegexesOpts(const char *parse_str, DetectParseRegex *detect_parse, int opts)
{
    int en;
    PCRE2_SIZE eo;

    detect_parse->regex =
            pcre2_compile((PCRE2_SPTR8)parse_str, PCRE2_ZERO_TERMINATED, opts, &en, &eo, NULL);
    if (detect_parse->regex == NULL) {
        PCRE2_UCHAR errbuffer[256];
        pcre2_get_error_message(en, errbuffer, sizeof(errbuffer));
        SCLogError("pcre compile of \"%s\" failed at "
                   "offset %d: %s",
                parse_str, en, errbuffer);
        return false;
    }
    detect_parse->context = pcre2_match_context_create(NULL);
    if (detect_parse->context == NULL) {
        SCLogError("pcre2 could not create match context");
        pcre2_code_free(detect_parse->regex);
        detect_parse->regex = NULL;
        return false;
    }
    pcre2_set_match_limit(detect_parse->context, SC_MATCH_LIMIT_DEFAULT);
    pcre2_set_recursion_limit(detect_parse->context, SC_MATCH_LIMIT_RECURSION_DEFAULT);
    DetectParseRegexAddToFreeList(detect_parse);

    return true;
}

DetectParseRegex *DetectSetupPCRE2(const char *parse_str, int opts)
{
    int en;
    PCRE2_SIZE eo;
    DetectParseRegex *detect_parse = SCCalloc(1, sizeof(DetectParseRegex));
    if (detect_parse == NULL) {
        return NULL;
    }

    detect_parse->regex =
            pcre2_compile((PCRE2_SPTR8)parse_str, PCRE2_ZERO_TERMINATED, opts, &en, &eo, NULL);
    if (detect_parse->regex == NULL) {
        PCRE2_UCHAR errbuffer[256];
        pcre2_get_error_message(en, errbuffer, sizeof(errbuffer));
        SCLogError("pcre2 compile of \"%s\" failed at "
                   "offset %d: %s",
                parse_str, (int)eo, errbuffer);
        SCFree(detect_parse);
        return NULL;
    }

    detect_parse->next = g_detect_parse_regex_list;
    g_detect_parse_regex_list = detect_parse;
    return detect_parse;
}

int SC_Pcre2SubstringCopy(
        pcre2_match_data *match_data, uint32_t number, PCRE2_UCHAR *buffer, PCRE2_SIZE *bufflen)
{
    int r = pcre2_substring_copy_bynumber(match_data, number, buffer, bufflen);
    if (r == PCRE2_ERROR_UNSET) {
        buffer[0] = 0;
        *bufflen = 0;
        return 0;
    }
    return r;
}

int SC_Pcre2SubstringGet(
        pcre2_match_data *match_data, uint32_t number, PCRE2_UCHAR **bufferptr, PCRE2_SIZE *bufflen)
{
    int r = pcre2_substring_get_bynumber(match_data, number, bufferptr, bufflen);
    if (r == PCRE2_ERROR_UNSET) {
        *bufferptr = NULL;
        *bufflen = 0;
        return 0;
    }
    return r;
}

void DetectSetupParseRegexes(const char *parse_str, DetectParseRegex *detect_parse)
{
    if (!DetectSetupParseRegexesOpts(parse_str, detect_parse, 0)) {
        FatalError("pcre compile and study failed");
    }
}

/*
 * TESTS
 */

#ifdef UNITTESTS
#include "detect-engine-alert.h"
#include "packet.h"

static int SigParseTest01 (void)
{
    int result = 1;
    Signature *sig = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    sig = SigInit(de_ctx, "alert tcp 1.2.3.4 any -> !1.2.3.4 any (msg:\"SigParseTest01\"; sid:1;)");
    if (sig == NULL)
        result = 0;

end:
    if (sig != NULL) SigFree(de_ctx, sig);
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);
    return result;
}

static int SigParseTest02 (void)
{
    int result = 0;
    Signature *sig = NULL;
    DetectPort *port = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();

    if (de_ctx == NULL)
        goto end;

    FILE *fd = SCClassConfGenerateValidDummyClassConfigFD01();
    SCClassConfLoadClassificationConfigFile(de_ctx, fd);

    sig = SigInit(de_ctx, "alert tcp any !21:902 -> any any (msg:\"ET MALWARE Suspicious 220 Banner on Local Port\"; content:\"220\"; offset:0; depth:4; pcre:\"/220[- ]/\"; sid:2003055; rev:4;)");
    if (sig == NULL) {
        goto end;
    }

    int r = DetectPortParse(de_ctx, &port, "0:20");
    if (r < 0)
        goto end;

    if (DetectPortCmp(sig->sp, port) == PORT_EQ) {
        result = 1;
    } else {
        DetectPortPrint(port); printf(" != "); DetectPortPrint(sig->sp); printf(": ");
    }

end:
    if (port != NULL)
        DetectPortCleanupList(de_ctx, port);
    if (sig != NULL)
        SigFree(de_ctx, sig);
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test SigParseTest03 test for invalid direction operator in rule
 */
static int SigParseTest03 (void)
{
    int result = 1;
    Signature *sig = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    sig = SigInit(de_ctx, "alert tcp 1.2.3.4 any <- !1.2.3.4 any (msg:\"SigParseTest03\"; sid:1;)");
    if (sig != NULL) {
        result = 0;
        printf("expected NULL got sig ptr %p: ",sig);
    }

end:
    if (sig != NULL) SigFree(de_ctx, sig);
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);
    return result;
}

static int SigParseTest04 (void)
{
    int result = 1;
    Signature *sig = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    sig = SigInit(de_ctx, "alert tcp 1.2.3.4 1024: -> !1.2.3.4 1024: (msg:\"SigParseTest04\"; sid:1;)");
    if (sig == NULL)
        result = 0;

end:
    if (sig != NULL) SigFree(de_ctx, sig);
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);
    return result;
}

/** \test Port validation */
static int SigParseTest05 (void)
{
    int result = 0;
    Signature *sig = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    sig = SigInit(de_ctx, "alert tcp 1.2.3.4 1024:65536 -> !1.2.3.4 any (msg:\"SigParseTest05\"; sid:1;)");
    if (sig == NULL) {
        result = 1;
    } else {
        printf("signature didn't fail to parse as we expected: ");
    }

end:
    if (sig != NULL) SigFree(de_ctx, sig);
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);
    return result;
}

/** \test Parsing bug debugging at 2010-03-18 */
static int SigParseTest06 (void)
{
    int result = 0;
    Signature *sig = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    sig = SigInit(de_ctx, "alert tcp any any -> any any (flow:to_server; content:\"GET\"; nocase; http_method; uricontent:\"/uri/\"; nocase; content:\"Host|3A| abc\"; nocase; sid:1; rev:1;)");
    if (sig != NULL) {
        result = 1;
    } else {
        printf("signature failed to parse: ");
    }

end:
    if (sig != NULL)
        SigFree(de_ctx, sig);
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Parsing duplicate sigs.
 */
static int SigParseTest07(void)
{
    int result = 0;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (msg:\"boo\"; sid:1; rev:1;)");
    DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (msg:\"boo\"; sid:1; rev:1;)");

    result = (de_ctx->sig_list != NULL && de_ctx->sig_list->next == NULL);

end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Parsing duplicate sigs.
 */
static int SigParseTest08(void)
{
    int result = 0;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (msg:\"boo\"; sid:1; rev:1;)");
    DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (msg:\"boo\"; sid:1; rev:2;)");

    result = (de_ctx->sig_list != NULL && de_ctx->sig_list->next == NULL &&
              de_ctx->sig_list->rev == 2);

end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Parsing duplicate sigs.
 */
static int SigParseTest09(void)
{
    int result = 1;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (msg:\"boo\"; sid:1; rev:1;)");
    DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (msg:\"boo\"; sid:1; rev:2;)");
    DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (msg:\"boo\"; sid:1; rev:6;)");
    DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (msg:\"boo\"; sid:1; rev:4;)");
    DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (msg:\"boo\"; sid:2; rev:2;)");
    result &= (de_ctx->sig_list != NULL && de_ctx->sig_list->id == 2 &&
               de_ctx->sig_list->rev == 2);
    if (result == 0)
        goto end;
    result &= (de_ctx->sig_list->next != NULL && de_ctx->sig_list->next->id == 1 &&
               de_ctx->sig_list->next->rev == 6);
    if (result == 0)
        goto end;

    DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (msg:\"boo\"; sid:2; rev:1;)");
    result &= (de_ctx->sig_list != NULL && de_ctx->sig_list->id == 2 &&
               de_ctx->sig_list->rev == 2);
    if (result == 0)
        goto end;
    result &= (de_ctx->sig_list->next != NULL && de_ctx->sig_list->next->id == 1 &&
               de_ctx->sig_list->next->rev == 6);
    if (result == 0)
        goto end;

    DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (msg:\"boo\"; sid:2; rev:4;)");
    result &= (de_ctx->sig_list != NULL && de_ctx->sig_list->id == 2 &&
               de_ctx->sig_list->rev == 4);
    if (result == 0)
        goto end;
    result &= (de_ctx->sig_list->next != NULL && de_ctx->sig_list->next->id == 1 &&
               de_ctx->sig_list->next->rev == 6);
    if (result == 0)
        goto end;

end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Parsing duplicate sigs.
 */
static int SigParseTest10(void)
{
    int result = 1;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (msg:\"boo\"; sid:1; rev:1;)");
    DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (msg:\"boo\"; sid:2; rev:1;)");
    DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (msg:\"boo\"; sid:3; rev:1;)");
    DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (msg:\"boo\"; sid:4; rev:1;)");
    DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (msg:\"boo\"; sid:5; rev:1;)");
    DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (msg:\"boo\"; sid:3; rev:2;)");
    DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (msg:\"boo\"; sid:2; rev:2;)");

    result &= ((de_ctx->sig_list->id == 2) &&
               (de_ctx->sig_list->next->id == 3) &&
               (de_ctx->sig_list->next->next->id == 5) &&
               (de_ctx->sig_list->next->next->next->id == 4) &&
               (de_ctx->sig_list->next->next->next->next->id == 1));

end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test Parsing sig with trailing space(s) as reported by
 *       Morgan Cox on oisf-users.
 */
static int SigParseTest11(void)
{
    int result = 0;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    Signature *s = NULL;

    s = DetectEngineAppendSig(de_ctx,
            "drop tcp any any -> any 80 (msg:\"Snort_Inline is blocking the http link\"; sid:1;) ");
    if (s == NULL) {
        printf("sig 1 didn't parse: ");
        goto end;
    }

    s = DetectEngineAppendSig(de_ctx, "drop tcp any any -> any 80 (msg:\"Snort_Inline is blocking "
                                      "the http link\"; sid:2;)            ");
    if (s == NULL) {
        printf("sig 2 didn't parse: ");
        goto end;
    }

    result = 1;
end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test file_data with rawbytes
 */
static int SigParseTest12(void)
{
    int result = 0;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    Signature *s = NULL;

    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (file_data; content:\"abc\"; rawbytes; sid:1;)");
    if (s != NULL) {
        printf("sig 1 should have given an error: ");
        goto end;
    }

    result = 1;
end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test packet/stream sig
 */
static int SigParseTest13(void)
{
    int result = 0;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    Signature *s = NULL;

    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (content:\"abc\"; sid:1;)");
    if (s == NULL) {
        printf("sig 1 invalidated: failure");
        goto end;
    }

    if (!(s->flags & SIG_FLAG_REQUIRE_STREAM)) {
        printf("sig doesn't have stream flag set\n");
        goto end;
    }

    if (s->flags & SIG_FLAG_REQUIRE_PACKET) {
        printf("sig has packet flag set\n");
        goto end;
    }

    result = 1;

end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test packet/stream sig
 */
static int SigParseTest14(void)
{
    int result = 0;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    Signature *s = NULL;

    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (content:\"abc\"; dsize:>0; sid:1;)");
    if (s == NULL) {
        printf("sig 1 invalidated: failure");
        goto end;
    }

    if (!(s->flags & SIG_FLAG_REQUIRE_PACKET)) {
        printf("sig doesn't have packet flag set\n");
        goto end;
    }

    if (s->flags & SIG_FLAG_REQUIRE_STREAM) {
        printf("sig has stream flag set\n");
        goto end;
    }

    result = 1;

end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test packet/stream sig
 */
static int SigParseTest15(void)
{
    int result = 0;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    Signature *s = NULL;

    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (content:\"abc\"; offset:5; sid:1;)");
    if (s == NULL) {
        printf("sig 1 invalidated: failure");
        goto end;
    }

    if (!(s->flags & SIG_FLAG_REQUIRE_PACKET)) {
        printf("sig doesn't have packet flag set\n");
        goto end;
    }

    if (!(s->flags & SIG_FLAG_REQUIRE_STREAM)) {
        printf("sig doesn't have stream flag set\n");
        goto end;
    }

    result = 1;

end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test packet/stream sig
 */
static int SigParseTest16(void)
{
    int result = 0;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    Signature *s = NULL;

    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (content:\"abc\"; depth:5; sid:1;)");
    if (s == NULL) {
        printf("sig 1 invalidated: failure");
        goto end;
    }

    if (!(s->flags & SIG_FLAG_REQUIRE_PACKET)) {
        printf("sig doesn't have packet flag set\n");
        goto end;
    }

    if (!(s->flags & SIG_FLAG_REQUIRE_STREAM)) {
        printf("sig doesn't have stream flag set\n");
        goto end;
    }

    result = 1;

end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test packet/stream sig
 */
static int SigParseTest17(void)
{
    int result = 0;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    Signature *s = NULL;

    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (content:\"abc\"; offset:1; depth:5; sid:1;)");
    if (s == NULL) {
        printf("sig 1 invalidated: failure");
        goto end;
    }

    if (!(s->flags & SIG_FLAG_REQUIRE_PACKET)) {
        printf("sig doesn't have packet flag set\n");
        goto end;
    }

    if (!(s->flags & SIG_FLAG_REQUIRE_STREAM)) {
        printf("sig doesn't have stream flag set\n");
        goto end;
    }

    result = 1;

end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/** \test sid value too large. Bug #779 */
static int SigParseTest18 (void)
{
    int result = 0;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    if (DetectEngineAppendSig(de_ctx, "alert tcp 1.2.3.4 any -> !1.2.3.4 any (msg:\"SigParseTest01\"; sid:99999999999999999999;)") != NULL)
        goto end;

    result = 1;
end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/** \test gid value too large. Related to bug #779 */
static int SigParseTest19 (void)
{
    int result = 0;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    if (DetectEngineAppendSig(de_ctx, "alert tcp 1.2.3.4 any -> !1.2.3.4 any (msg:\"SigParseTest01\"; sid:1; gid:99999999999999999999;)") != NULL)
        goto end;

    result = 1;
end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/** \test rev value too large. Related to bug #779 */
static int SigParseTest20 (void)
{
    int result = 0;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    if (DetectEngineAppendSig(de_ctx, "alert tcp 1.2.3.4 any -> !1.2.3.4 any (msg:\"SigParseTest01\"; sid:1; rev:99999999999999999999;)") != NULL)
        goto end;

    result = 1;
end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/** \test address parsing */
static int SigParseTest21 (void)
{
    int result = 0;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    if (DetectEngineAppendSig(de_ctx, "alert tcp [1.2.3.4, 1.2.3.5] any -> !1.2.3.4 any (sid:1;)") == NULL)
        goto end;

    result = 1;
end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/** \test address parsing */
static int SigParseTest22 (void)
{
    int result = 0;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    if (DetectEngineAppendSig(de_ctx, "alert tcp [10.10.10.0/24, !10.10.10.247] any -> [10.10.10.0/24, !10.10.10.247] any (sid:1;)") == NULL)
        goto end;

    result = 1;
end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test rule ending in carriage return
 */
static int SigParseTest23(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    Signature *s = NULL;

    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any (content:\"abc\"; offset:1; depth:5; sid:1;)\r");
    FAIL_IF_NULL(s);

    DetectEngineCtxFree(de_ctx);
    PASS;
}

/** \test Direction operator validation (invalid) */
static int SigParseBidirecTest06 (void)
{
    int result = 1;
    Signature *sig = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    sig = DetectEngineAppendSig(de_ctx, "alert tcp 192.168.1.1 any - 192.168.1.5 any (msg:\"SigParseBidirecTest05\"; sid:1;)");
    if (sig == NULL)
        result = 1;

end:
    if (sig != NULL) SigFree(de_ctx, sig);
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);
    return result;
}

/** \test Direction operator validation (invalid) */
static int SigParseBidirecTest07 (void)
{
    int result = 1;
    Signature *sig = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    sig = DetectEngineAppendSig(de_ctx, "alert tcp 192.168.1.1 any <- 192.168.1.5 any (msg:\"SigParseBidirecTest05\"; sid:1;)");
    if (sig == NULL)
        result = 1;

end:
    if (sig != NULL) SigFree(de_ctx, sig);
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);
    return result;
}

/** \test Direction operator validation (invalid) */
static int SigParseBidirecTest08 (void)
{
    int result = 1;
    Signature *sig = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    sig = DetectEngineAppendSig(de_ctx, "alert tcp 192.168.1.1 any < 192.168.1.5 any (msg:\"SigParseBidirecTest05\"; sid:1;)");
    if (sig == NULL)
        result = 1;

end:
    if (sig != NULL) SigFree(de_ctx, sig);
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);
    return result;
}

/** \test Direction operator validation (invalid) */
static int SigParseBidirecTest09 (void)
{
    int result = 1;
    Signature *sig = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    sig = DetectEngineAppendSig(de_ctx, "alert tcp 192.168.1.1 any > 192.168.1.5 any (msg:\"SigParseBidirecTest05\"; sid:1;)");
    if (sig == NULL)
        result = 1;

end:
    if (sig != NULL) SigFree(de_ctx, sig);
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);
    return result;
}

/** \test Direction operator validation (invalid) */
static int SigParseBidirecTest10 (void)
{
    int result = 1;
    Signature *sig = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    sig = DetectEngineAppendSig(de_ctx, "alert tcp 192.168.1.1 any -< 192.168.1.5 any (msg:\"SigParseBidirecTest05\"; sid:1;)");
    if (sig == NULL)
        result = 1;

end:
    if (sig != NULL) SigFree(de_ctx, sig);
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);
    return result;
}

/** \test Direction operator validation (invalid) */
static int SigParseBidirecTest11 (void)
{
    int result = 1;
    Signature *sig = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    sig = DetectEngineAppendSig(de_ctx, "alert tcp 192.168.1.1 any >- 192.168.1.5 any (msg:\"SigParseBidirecTest05\"; sid:1;)");
    if (sig == NULL)
        result = 1;

end:
    if (sig != NULL) SigFree(de_ctx, sig);
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);
    return result;
}

/** \test Direction operator validation (invalid) */
static int SigParseBidirecTest12 (void)
{
    int result = 1;
    Signature *sig = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    sig = DetectEngineAppendSig(de_ctx, "alert tcp 192.168.1.1 any >< 192.168.1.5 any (msg:\"SigParseBidirecTest05\"; sid:1;)");
    if (sig == NULL)
        result = 1;

end:
    if (sig != NULL) SigFree(de_ctx, sig);
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);
    return result;
}

/** \test Direction operator validation (valid) */
static int SigParseBidirecTest13 (void)
{
    int result = 1;
    Signature *sig = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    sig = DetectEngineAppendSig(de_ctx, "alert tcp 192.168.1.1 any <> 192.168.1.5 any (msg:\"SigParseBidirecTest05\"; sid:1;)");
    if (sig != NULL)
        result = 1;

end:
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);
    return result;
}

/** \test Direction operator validation (valid) */
static int SigParseBidirecTest14 (void)
{
    int result = 1;
    Signature *sig = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    sig = DetectEngineAppendSig(de_ctx, "alert tcp 192.168.1.1 any -> 192.168.1.5 any (msg:\"SigParseBidirecTest05\"; sid:1;)");
    if (sig != NULL)
        result = 1;

end:
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);
    return result;
}

/** \test Ensure that we don't set bidirectional in a
 *         normal (one direction) Signature
 */
static int SigTestBidirec01 (void)
{
    Signature *sig = NULL;
    int result = 0;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    sig = DetectEngineAppendSig(de_ctx, "alert tcp 1.2.3.4 1024:65535 -> !1.2.3.4 any (msg:\"SigTestBidirec01\"; sid:1;)");
    if (sig == NULL)
        goto end;
    if (sig->next != NULL)
        goto end;
    if (sig->init_data->init_flags & SIG_FLAG_INIT_BIDIREC)
        goto end;
    if (de_ctx->signum != 1)
        goto end;

    result = 1;

end:
    if (de_ctx != NULL) {
        SigCleanSignatures(de_ctx);
        SigGroupCleanup(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }
    return result;
}

/** \test Ensure that we set a bidirectional Signature correctly */
static int SigTestBidirec02 (void)
{
    int result = 0;
    Signature *sig = NULL;
    Signature *copy = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;

    sig = DetectEngineAppendSig(de_ctx, "alert tcp 1.2.3.4 1024:65535 <> !1.2.3.4 any (msg:\"SigTestBidirec02\"; sid:1;)");
    if (sig == NULL)
        goto end;
    if (de_ctx->sig_list != sig)
        goto end;
    if (!(sig->init_data->init_flags & SIG_FLAG_INIT_BIDIREC))
        goto end;
    if (sig->next == NULL)
        goto end;
    if (de_ctx->signum != 2)
        goto end;
    copy = sig->next;
    if (copy->next != NULL)
        goto end;
    if (!(copy->init_data->init_flags & SIG_FLAG_INIT_BIDIREC))
        goto end;

    result = 1;

end:
    if (de_ctx != NULL) {
        SigCleanSignatures(de_ctx);
        SigGroupCleanup(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }

    return result;
}

/** \test Ensure that we set a bidirectional Signature correctly
*         and we install it with the rest of the signatures, checking
*         also that it match with the correct addr directions
*/
static int SigTestBidirec03 (void)
{
    int result = 0;
    Signature *sig = NULL;
    Packet *p = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;

    const char *sigs[3];
    sigs[0] = "alert tcp any any -> 192.168.1.1 any (msg:\"SigTestBidirec03 sid 1\"; sid:1;)";
    sigs[1] = "alert tcp any any <> 192.168.1.1 any (msg:\"SigTestBidirec03 sid 2 bidirectional\"; sid:2;)";
    sigs[2] = "alert tcp any any -> 192.168.1.1 any (msg:\"SigTestBidirec03 sid 3\"; sid:3;)";
    UTHAppendSigs(de_ctx, sigs, 3);

    /* Checking that bidirectional rules are set correctly */
    sig = de_ctx->sig_list;
    if (sig == NULL)
        goto end;
    if (sig->next == NULL)
        goto end;
    if (sig->next->next == NULL)
        goto end;
    if (sig->next->next->next == NULL)
        goto end;
    if (sig->next->next->next->next != NULL)
        goto end;
    if (de_ctx->signum != 4)
        goto end;

    uint8_t rawpkt1_ether[] = {
        0x00,0x50,0x56,0xea,0x00,0xbd,0x00,0x0c,
        0x29,0x40,0xc8,0xb5,0x08,0x00,0x45,0x00,
        0x01,0xa8,0xb9,0xbb,0x40,0x00,0x40,0x06,
        0xe0,0xbf,0xc0,0xa8,0x1c,0x83,0xc0,0xa8,
        0x01,0x01,0xb9,0x0a,0x00,0x50,0x6f,0xa2,
        0x92,0xed,0x7b,0xc1,0xd3,0x4d,0x50,0x18,
        0x16,0xd0,0xa0,0x6f,0x00,0x00,0x47,0x45,
        0x54,0x20,0x2f,0x20,0x48,0x54,0x54,0x50,
        0x2f,0x31,0x2e,0x31,0x0d,0x0a,0x48,0x6f,
        0x73,0x74,0x3a,0x20,0x31,0x39,0x32,0x2e,
        0x31,0x36,0x38,0x2e,0x31,0x2e,0x31,0x0d,
        0x0a,0x55,0x73,0x65,0x72,0x2d,0x41,0x67,
        0x65,0x6e,0x74,0x3a,0x20,0x4d,0x6f,0x7a,
        0x69,0x6c,0x6c,0x61,0x2f,0x35,0x2e,0x30,
        0x20,0x28,0x58,0x31,0x31,0x3b,0x20,0x55,
        0x3b,0x20,0x4c,0x69,0x6e,0x75,0x78,0x20,
        0x78,0x38,0x36,0x5f,0x36,0x34,0x3b,0x20,
        0x65,0x6e,0x2d,0x55,0x53,0x3b,0x20,0x72,
        0x76,0x3a,0x31,0x2e,0x39,0x2e,0x30,0x2e,
        0x31,0x34,0x29,0x20,0x47,0x65,0x63,0x6b,
        0x6f,0x2f,0x32,0x30,0x30,0x39,0x30,0x39,
        0x30,0x32,0x31,0x37,0x20,0x55,0x62,0x75,
        0x6e,0x74,0x75,0x2f,0x39,0x2e,0x30,0x34,
        0x20,0x28,0x6a,0x61,0x75,0x6e,0x74,0x79,
        0x29,0x20,0x46,0x69,0x72,0x65,0x66,0x6f,
        0x78,0x2f,0x33,0x2e,0x30,0x2e,0x31,0x34,
        0x0d,0x0a,0x41,0x63,0x63,0x65,0x70,0x74,
        0x3a,0x20,0x74,0x65,0x78,0x74,0x2f,0x68,
        0x74,0x6d,0x6c,0x2c,0x61,0x70,0x70,0x6c,
        0x69,0x63,0x61,0x74,0x69,0x6f,0x6e,0x2f,
        0x78,0x68,0x74,0x6d,0x6c,0x2b,0x78,0x6d,
        0x6c,0x2c,0x61,0x70,0x70,0x6c,0x69,0x63,
        0x61,0x74,0x69,0x6f,0x6e,0x2f,0x78,0x6d,
        0x6c,0x3b,0x71,0x3d,0x30,0x2e,0x39,0x2c,
        0x2a,0x2f,0x2a,0x3b,0x71,0x3d,0x30,0x2e,
        0x38,0x0d,0x0a,0x41,0x63,0x63,0x65,0x70,
        0x74,0x2d,0x4c,0x61,0x6e,0x67,0x75,0x61,
        0x67,0x65,0x3a,0x20,0x65,0x6e,0x2d,0x75,
        0x73,0x2c,0x65,0x6e,0x3b,0x71,0x3d,0x30,
        0x2e,0x35,0x0d,0x0a,0x41,0x63,0x63,0x65,
        0x70,0x74,0x2d,0x45,0x6e,0x63,0x6f,0x64,
        0x69,0x6e,0x67,0x3a,0x20,0x67,0x7a,0x69,
        0x70,0x2c,0x64,0x65,0x66,0x6c,0x61,0x74,
        0x65,0x0d,0x0a,0x41,0x63,0x63,0x65,0x70,
        0x74,0x2d,0x43,0x68,0x61,0x72,0x73,0x65,
        0x74,0x3a,0x20,0x49,0x53,0x4f,0x2d,0x38,
        0x38,0x35,0x39,0x2d,0x31,0x2c,0x75,0x74,
        0x66,0x2d,0x38,0x3b,0x71,0x3d,0x30,0x2e,
        0x37,0x2c,0x2a,0x3b,0x71,0x3d,0x30,0x2e,
        0x37,0x0d,0x0a,0x4b,0x65,0x65,0x70,0x2d,
        0x41,0x6c,0x69,0x76,0x65,0x3a,0x20,0x33,
        0x30,0x30,0x0d,0x0a,0x43,0x6f,0x6e,0x6e,
        0x65,0x63,0x74,0x69,0x6f,0x6e,0x3a,0x20,
        0x6b,0x65,0x65,0x70,0x2d,0x61,0x6c,0x69,
        0x76,0x65,0x0d,0x0a,0x0d,0x0a }; /* end rawpkt1_ether */

    FlowInitConfig(FLOW_QUIET);
    p = UTHBuildPacketFromEth(rawpkt1_ether, sizeof(rawpkt1_ether));
    if (p == NULL) {
        SCLogDebug("Error building packet");
        goto end;
    }
    UTHMatchPackets(de_ctx, &p, 1);

    uint32_t sids[3] = {1, 2, 3};
    uint32_t results[3] = {1, 1, 1};
    result = UTHCheckPacketMatchResults(p, sids, results, 1);

end:
    if (p != NULL) {
        PacketRecycle(p);
        SCFree(p);
    }
    FlowShutdown();
    return result;
}

/** \test Ensure that we set a bidirectional Signature correctly
*         and we install it with the rest of the signatures, checking
*         also that it match with the correct addr directions
*/
static int SigTestBidirec04 (void)
{
    int result = 0;
    Signature *sig = NULL;
    Packet *p = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;

    sig = DetectEngineAppendSig(de_ctx, "alert tcp 192.168.1.1 any -> any any (msg:\"SigTestBidirec03 sid 1\"; sid:1;)");
    if (sig == NULL)
        goto end;
    sig = DetectEngineAppendSig(de_ctx, "alert tcp 192.168.1.1 any <> any any (msg:\"SigTestBidirec03 sid 2 bidirectional\"; sid:2;)");
    if (sig == NULL)
        goto end;
    if ( !(sig->init_data->init_flags & SIG_FLAG_INIT_BIDIREC))
        goto end;
    if (sig->next == NULL)
        goto end;
    if (sig->next->next == NULL)
        goto end;
    if (sig->next->next->next != NULL)
        goto end;
    if (de_ctx->signum != 3)
        goto end;

    sig = DetectEngineAppendSig(de_ctx, "alert tcp 192.168.1.1 any -> any any (msg:\"SigTestBidirec03 sid 3\"; sid:3;)");
    if (sig == NULL)
        goto end;
    if (sig->next == NULL)
        goto end;
    if (sig->next->next == NULL)
        goto end;
    if (sig->next->next->next == NULL)
        goto end;
    if (sig->next->next->next->next != NULL)
        goto end;
    if (de_ctx->signum != 4)
        goto end;

    uint8_t rawpkt1_ether[] = {
        0x00,0x50,0x56,0xea,0x00,0xbd,0x00,0x0c,
        0x29,0x40,0xc8,0xb5,0x08,0x00,0x45,0x00,
        0x01,0xa8,0xb9,0xbb,0x40,0x00,0x40,0x06,
        0xe0,0xbf,0xc0,0xa8,0x1c,0x83,0xc0,0xa8,
        0x01,0x01,0xb9,0x0a,0x00,0x50,0x6f,0xa2,
        0x92,0xed,0x7b,0xc1,0xd3,0x4d,0x50,0x18,
        0x16,0xd0,0xa0,0x6f,0x00,0x00,0x47,0x45,
        0x54,0x20,0x2f,0x20,0x48,0x54,0x54,0x50,
        0x2f,0x31,0x2e,0x31,0x0d,0x0a,0x48,0x6f,
        0x73,0x74,0x3a,0x20,0x31,0x39,0x32,0x2e,
        0x31,0x36,0x38,0x2e,0x31,0x2e,0x31,0x0d,
        0x0a,0x55,0x73,0x65,0x72,0x2d,0x41,0x67,
        0x65,0x6e,0x74,0x3a,0x20,0x4d,0x6f,0x7a,
        0x69,0x6c,0x6c,0x61,0x2f,0x35,0x2e,0x30,
        0x20,0x28,0x58,0x31,0x31,0x3b,0x20,0x55,
        0x3b,0x20,0x4c,0x69,0x6e,0x75,0x78,0x20,
        0x78,0x38,0x36,0x5f,0x36,0x34,0x3b,0x20,
        0x65,0x6e,0x2d,0x55,0x53,0x3b,0x20,0x72,
        0x76,0x3a,0x31,0x2e,0x39,0x2e,0x30,0x2e,
        0x31,0x34,0x29,0x20,0x47,0x65,0x63,0x6b,
        0x6f,0x2f,0x32,0x30,0x30,0x39,0x30,0x39,
        0x30,0x32,0x31,0x37,0x20,0x55,0x62,0x75,
        0x6e,0x74,0x75,0x2f,0x39,0x2e,0x30,0x34,
        0x20,0x28,0x6a,0x61,0x75,0x6e,0x74,0x79,
        0x29,0x20,0x46,0x69,0x72,0x65,0x66,0x6f,
        0x78,0x2f,0x33,0x2e,0x30,0x2e,0x31,0x34,
        0x0d,0x0a,0x41,0x63,0x63,0x65,0x70,0x74,
        0x3a,0x20,0x74,0x65,0x78,0x74,0x2f,0x68,
        0x74,0x6d,0x6c,0x2c,0x61,0x70,0x70,0x6c,
        0x69,0x63,0x61,0x74,0x69,0x6f,0x6e,0x2f,
        0x78,0x68,0x74,0x6d,0x6c,0x2b,0x78,0x6d,
        0x6c,0x2c,0x61,0x70,0x70,0x6c,0x69,0x63,
        0x61,0x74,0x69,0x6f,0x6e,0x2f,0x78,0x6d,
        0x6c,0x3b,0x71,0x3d,0x30,0x2e,0x39,0x2c,
        0x2a,0x2f,0x2a,0x3b,0x71,0x3d,0x30,0x2e,
        0x38,0x0d,0x0a,0x41,0x63,0x63,0x65,0x70,
        0x74,0x2d,0x4c,0x61,0x6e,0x67,0x75,0x61,
        0x67,0x65,0x3a,0x20,0x65,0x6e,0x2d,0x75,
        0x73,0x2c,0x65,0x6e,0x3b,0x71,0x3d,0x30,
        0x2e,0x35,0x0d,0x0a,0x41,0x63,0x63,0x65,
        0x70,0x74,0x2d,0x45,0x6e,0x63,0x6f,0x64,
        0x69,0x6e,0x67,0x3a,0x20,0x67,0x7a,0x69,
        0x70,0x2c,0x64,0x65,0x66,0x6c,0x61,0x74,
        0x65,0x0d,0x0a,0x41,0x63,0x63,0x65,0x70,
        0x74,0x2d,0x43,0x68,0x61,0x72,0x73,0x65,
        0x74,0x3a,0x20,0x49,0x53,0x4f,0x2d,0x38,
        0x38,0x35,0x39,0x2d,0x31,0x2c,0x75,0x74,
        0x66,0x2d,0x38,0x3b,0x71,0x3d,0x30,0x2e,
        0x37,0x2c,0x2a,0x3b,0x71,0x3d,0x30,0x2e,
        0x37,0x0d,0x0a,0x4b,0x65,0x65,0x70,0x2d,
        0x41,0x6c,0x69,0x76,0x65,0x3a,0x20,0x33,
        0x30,0x30,0x0d,0x0a,0x43,0x6f,0x6e,0x6e,
        0x65,0x63,0x74,0x69,0x6f,0x6e,0x3a,0x20,
        0x6b,0x65,0x65,0x70,0x2d,0x61,0x6c,0x69,
        0x76,0x65,0x0d,0x0a,0x0d,0x0a }; /* end rawpkt1_ether */

    p = PacketGetFromAlloc();
    if (unlikely(p == NULL))
        return 0;
    DecodeThreadVars dtv;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;

    memset(&th_v, 0, sizeof(th_v));

    FlowInitConfig(FLOW_QUIET);
    DecodeEthernet(&th_v, &dtv, p, rawpkt1_ether, sizeof(rawpkt1_ether));
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    /* At this point we have a list of 4 signatures. The last one
       is a copy of the second one. If we receive a packet
       with source 192.168.1.1 80, all the sids should match */

    SigGroupBuild(de_ctx);
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    /* only sid 2 should match with a packet going to 192.168.1.1 port 80 */
    if (PacketAlertCheck(p, 1) <= 0 && PacketAlertCheck(p, 3) <= 0 &&
        PacketAlertCheck(p, 2) == 1) {
        result = 1;
    }

    if (p != NULL) {
        PacketRecycle(p);
    }
    FlowShutdown();
    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);

end:
    if (de_ctx != NULL) {
        SigCleanSignatures(de_ctx);
        SigGroupCleanup(de_ctx);
        DetectEngineCtxFree(de_ctx);
    }

    if (p != NULL)
        SCFree(p);
    return result;
}

/**
 * \test check that we don't allow invalid negation options
 */
static int SigParseTestNegation01 (void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);
    de_ctx->flags |= DE_QUIET;
    Signature *s = DetectEngineAppendSig(de_ctx, "alert tcp !any any -> any any (sid:1;)");
    FAIL_IF_NOT_NULL(s);
    DetectEngineCtxFree(de_ctx);
    PASS;
}

/**
 * \test check that we don't allow invalid negation options
 */
static int SigParseTestNegation02 (void)
{
    int result = 0;
    DetectEngineCtx *de_ctx;
    Signature *s=NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;
    de_ctx->flags |= DE_QUIET;

    s = SigInit(de_ctx,"alert tcp any !any -> any any (msg:\"SigTest41-02 src ip is !any \"; classtype:misc-activity; sid:410002; rev:1;)");
    if (s != NULL) {
        SigFree(de_ctx, s);
        goto end;
    }

    result = 1;
end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}
/**
 * \test check that we don't allow invalid negation options
 */
static int SigParseTestNegation03 (void)
{
    int result = 0;
    DetectEngineCtx *de_ctx;
    Signature *s=NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;
    de_ctx->flags |= DE_QUIET;

    s = SigInit(de_ctx,"alert tcp any any -> any [80:!80] (msg:\"SigTest41-03 dst port [80:!80] \"; classtype:misc-activity; sid:410003; rev:1;)");
    if (s != NULL) {
        SigFree(de_ctx, s);
        goto end;
    }

    result = 1;
end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}
/**
 * \test check that we don't allow invalid negation options
 */
static int SigParseTestNegation04 (void)
{
    int result = 0;
    DetectEngineCtx *de_ctx;
    Signature *s=NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;
    de_ctx->flags |= DE_QUIET;

    s = SigInit(de_ctx,"alert tcp any any -> any [80,!80] (msg:\"SigTest41-03 dst port [80:!80] \"; classtype:misc-activity; sid:410003; rev:1;)");
    if (s != NULL) {
        SigFree(de_ctx, s);
        goto end;
    }

    result = 1;
end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}
/**
 * \test check that we don't allow invalid negation options
 */
static int SigParseTestNegation05 (void)
{
    int result = 0;
    DetectEngineCtx *de_ctx;
    Signature *s=NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;
    de_ctx->flags |= DE_QUIET;

    s = SigInit(de_ctx,"alert tcp any any -> [192.168.0.2,!192.168.0.2] any (msg:\"SigTest41-04 dst ip [192.168.0.2,!192.168.0.2] \"; classtype:misc-activity; sid:410004; rev:1;)");
    if (s != NULL) {
        SigFree(de_ctx, s);
        goto end;
    }

    result = 1;
end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}
/**
 * \test check that we don't allow invalid negation options
 */
static int SigParseTestNegation06 (void)
{
    int result = 0;
    DetectEngineCtx *de_ctx;
    Signature *s=NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;
    de_ctx->flags |= DE_QUIET;

    s = SigInit(de_ctx,"alert tcp any any -> any [100:1000,!1:20000] (msg:\"SigTest41-05 dst port [100:1000,!1:20000] \"; classtype:misc-activity; sid:410005; rev:1;)");
    if (s != NULL) {
        SigFree(de_ctx, s);
        goto end;
    }

    result = 1;
end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test check that we don't allow invalid negation options
 */
static int SigParseTestNegation07 (void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);
    de_ctx->flags |= DE_QUIET;
    Signature *s = DetectEngineAppendSig(
            de_ctx, "alert tcp any any -> [192.168.0.2,!192.168.0.0/24] any (sid:410006;)");
    FAIL_IF_NOT_NULL(s);
    DetectEngineCtxFree(de_ctx);
    PASS;
}

/**
 * \test check valid negation bug 1079
 */
static int SigParseTestNegation08 (void)
{
    int result = 0;
    DetectEngineCtx *de_ctx;
    Signature *s=NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;
    de_ctx->flags |= DE_QUIET;

    s = SigInit(de_ctx,"alert tcp any any -> [192.168.0.0/16,!192.168.0.0/24] any (sid:410006; rev:1;)");
    if (s == NULL) {
        goto end;
    }

    result = 1;
end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test mpm
 */
static int SigParseTestMpm01 (void)
{
    int result = 0;
    Signature *sig = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    sig = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"mpm test\"; content:\"abcd\"; sid:1;)");
    if (sig == NULL) {
        printf("sig failed to init: ");
        goto end;
    }

    if (sig->init_data->smlists[DETECT_SM_LIST_PMATCH] == NULL) {
        printf("sig doesn't have content list: ");
        goto end;
    }

    result = 1;
end:
    if (sig != NULL)
        SigFree(de_ctx, sig);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test mpm
 */
static int SigParseTestMpm02 (void)
{
    int result = 0;
    Signature *sig = NULL;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    sig = SigInit(de_ctx, "alert tcp any any -> any any (msg:\"mpm test\"; content:\"abcd\"; content:\"abcdef\"; sid:1;)");
    if (sig == NULL) {
        printf("sig failed to init: ");
        goto end;
    }

    if (sig->init_data->smlists[DETECT_SM_LIST_PMATCH] == NULL) {
        printf("sig doesn't have content list: ");
        goto end;
    }

    result = 1;
end:
    if (sig != NULL)
        SigFree(de_ctx, sig);
    DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test test tls (app layer) rule
 */
static int SigParseTestAppLayerTLS01(void)
{
    int result = 0;
    DetectEngineCtx *de_ctx;
    Signature *s=NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;
    de_ctx->flags |= DE_QUIET;

    s = SigInit(de_ctx,"alert tls any any -> any any (msg:\"SigParseTestAppLayerTLS01 \"; sid:410006; rev:1;)");
    if (s == NULL) {
        printf("parsing sig failed: ");
        goto end;
    }

    if (s->alproto == 0) {
        printf("alproto not set: ");
        goto end;
    }

    result = 1;
end:
    if (s != NULL)
        SigFree(de_ctx, s);
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test test tls (app layer) rule
 */
static int SigParseTestAppLayerTLS02(void)
{
    int result = 0;
    DetectEngineCtx *de_ctx;
    Signature *s=NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;
    de_ctx->flags |= DE_QUIET;

    s = SigInit(de_ctx,"alert tls any any -> any any (msg:\"SigParseTestAppLayerTLS02 \"; tls.version:1.0; sid:410006; rev:1;)");
    if (s == NULL) {
        printf("parsing sig failed: ");
        goto end;
    }

    if (s->alproto == 0) {
        printf("alproto not set: ");
        goto end;
    }

    result = 1;
end:
    if (s != NULL)
        SigFree(de_ctx, s);
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

/**
 * \test test tls (app layer) rule
 */
static int SigParseTestAppLayerTLS03(void)
{
    int result = 0;
    DetectEngineCtx *de_ctx;
    Signature *s=NULL;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;
    de_ctx->flags |= DE_QUIET;

    s = SigInit(de_ctx,"alert tls any any -> any any (msg:\"SigParseTestAppLayerTLS03 \"; tls.version:2.5; sid:410006; rev:1;)");
    if (s != NULL) {
        SigFree(de_ctx, s);
        goto end;
    }

    result = 1;
end:
    if (de_ctx != NULL)
        DetectEngineCtxFree(de_ctx);
    return result;
}

static int SigParseTestUnbalancedQuotes01(void)
{
    DetectEngineCtx *de_ctx;
    Signature *s;

    de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);
    de_ctx->flags |= DE_QUIET;

    s = SigInit(de_ctx,
            "alert http any any -> any any (msg:\"SigParseTestUnbalancedQuotes01\"; "
            "pcre:\"/\\/[a-z]+\\.php\\?[a-z]+?=\\d{7}&[a-z]+?=\\d{7,8}$/U\" "
            "flowbits:set,et.exploitkitlanding; classtype:trojan-activity; sid:2017078; rev:5;)");
    FAIL_IF_NOT_NULL(s);

    PASS;
}

static int SigParseTestContentGtDsize01(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);
    de_ctx->flags |= DE_QUIET;

    Signature *s = SigInit(de_ctx,
            "alert http any any -> any any ("
            "dsize:21; content:\"0123456789001234567890|00 00|\"; "
            "sid:1; rev:1;)");
    FAIL_IF_NOT_NULL(s);

    PASS;
}

static int SigParseTestContentGtDsize02(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);
    de_ctx->flags |= DE_QUIET;

    Signature *s = SigInit(de_ctx,
            "alert http any any -> any any ("
            "dsize:21; content:\"0123456789|00 00|\"; offset:10; "
            "sid:1; rev:1;)");
    FAIL_IF_NOT_NULL(s);

    PASS;
}

static int CountSigsWithSid(const DetectEngineCtx *de_ctx, const uint32_t sid)
{
    int cnt = 0;
    for (Signature *s = de_ctx->sig_list; s != NULL; s = s->next) {
        if (sid == s->id)
            cnt++;
    }
    return cnt;
}

static int SigParseBidirWithSameSrcAndDest01(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);
    de_ctx->flags |= DE_QUIET;

    Signature *s = DetectEngineAppendSig(de_ctx, "alert tcp any any <> any any (sid:1;)");
    FAIL_IF_NULL(s);
    FAIL_IF_NOT(CountSigsWithSid(de_ctx, 1) == 1);
    FAIL_IF(s->init_data->init_flags & SIG_FLAG_INIT_BIDIREC);

    s = DetectEngineAppendSig(de_ctx, "alert tcp any [80, 81] <> any [81, 80] (sid:2;)");
    FAIL_IF_NULL(s);
    FAIL_IF_NOT(CountSigsWithSid(de_ctx, 2) == 1);
    FAIL_IF(s->init_data->init_flags & SIG_FLAG_INIT_BIDIREC);

    s = DetectEngineAppendSig(de_ctx,
            "alert tcp [1.2.3.4, 5.6.7.8] [80, 81] <> [5.6.7.8, 1.2.3.4] [81, 80] (sid:3;)");
    FAIL_IF_NULL(s);
    FAIL_IF_NOT(CountSigsWithSid(de_ctx, 3) == 1);
    FAIL_IF(s->init_data->init_flags & SIG_FLAG_INIT_BIDIREC);

    DetectEngineCtxFree(de_ctx);
    PASS;
}

static int SigParseBidirWithSameSrcAndDest02(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);
    de_ctx->flags |= DE_QUIET;

    // Source is a subset of destination
    Signature *s = DetectEngineAppendSig(
            de_ctx, "alert tcp 1.2.3.4 any <> [1.2.3.4, 5.6.7.8, ::1] any (sid:1;)");
    FAIL_IF_NULL(s);
    FAIL_IF_NOT(CountSigsWithSid(de_ctx, 1) == 2);
    FAIL_IF_NOT(s->init_data->init_flags & SIG_FLAG_INIT_BIDIREC);

    // Source is a subset of destination
    s = DetectEngineAppendSig(
            de_ctx, "alert tcp [1.2.3.4, ::1] [80, 81, 82] <> [1.2.3.4, ::1] [80, 81] (sid:2;)");
    FAIL_IF_NULL(s);
    FAIL_IF_NOT(CountSigsWithSid(de_ctx, 2) == 2);
    FAIL_IF_NOT(s->init_data->init_flags & SIG_FLAG_INIT_BIDIREC);

    // Source intersects with destination
    s = DetectEngineAppendSig(de_ctx,
            "alert tcp [1.2.3.4, ::1, ABCD:AAAA::1] [80] <> [1.2.3.4, ::1] [80, 81] (sid:3;)");
    FAIL_IF_NULL(s);
    FAIL_IF_NOT(CountSigsWithSid(de_ctx, 3) == 2);
    FAIL_IF_NOT(s->init_data->init_flags & SIG_FLAG_INIT_BIDIREC);

    // mix in negation, these are the same
    s = DetectEngineAppendSig(
            de_ctx, "alert tcp [!1.2.3.4, 1.2.3.0/24] any <> [1.2.3.0/24, !1.2.3.4] any (sid:4;)");
    FAIL_IF_NULL(s);
    FAIL_IF_NOT(CountSigsWithSid(de_ctx, 4) == 1);
    FAIL_IF(s->init_data->init_flags & SIG_FLAG_INIT_BIDIREC);

    // mix in negation, these are not the same
    s = DetectEngineAppendSig(
            de_ctx, "alert tcp [1.2.3.4, 1.2.3.0/24] any <> [1.2.3.0/24, !1.2.3.4] any (sid:5;)");
    FAIL_IF_NULL(s);
    FAIL_IF_NOT(CountSigsWithSid(de_ctx, 5) == 2);
    FAIL_IF_NOT(s->init_data->init_flags & SIG_FLAG_INIT_BIDIREC);

    DetectEngineCtxFree(de_ctx);
    PASS;
}

static int SigParseTestActionReject(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    Signature *sig = DetectEngineAppendSig(
            de_ctx, "reject tcp 1.2.3.4 any -> !1.2.3.4 any (msg:\"SigParseTest01\"; sid:1;)");
#ifdef HAVE_LIBNET11
    FAIL_IF_NULL(sig);
    FAIL_IF_NOT((sig->action & (ACTION_DROP | ACTION_REJECT)) == (ACTION_DROP | ACTION_REJECT));
#else
    FAIL_IF_NOT_NULL(sig);
#endif

    DetectEngineCtxFree(de_ctx);
    PASS;
}

static int SigParseTestActionDrop(void)
{
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);

    Signature *sig = DetectEngineAppendSig(
            de_ctx, "drop tcp 1.2.3.4 any -> !1.2.3.4 any (msg:\"SigParseTest01\"; sid:1;)");
    FAIL_IF_NULL(sig);
    FAIL_IF_NOT(sig->action & ACTION_DROP);

    DetectEngineCtxFree(de_ctx);
    PASS;
}

static int SigSetMultiAppProto(void)
{
    Signature *s = SigAlloc();
    FAIL_IF_NULL(s);

    AppProto alprotos[] = { 1, 2, 3, ALPROTO_UNKNOWN };
    FAIL_IF(DetectSignatureSetMultiAppProto(s, alprotos) < 0);

    // check intersection gives multiple entries
    alprotos[0] = 3;
    alprotos[1] = 2;
    alprotos[2] = ALPROTO_UNKNOWN;
    FAIL_IF(DetectSignatureSetMultiAppProto(s, alprotos) < 0);
    FAIL_IF(s->init_data->alprotos[0] != 3);
    FAIL_IF(s->init_data->alprotos[1] != 2);
    FAIL_IF(s->init_data->alprotos[2] != ALPROTO_UNKNOWN);

    // check single after multiple
    FAIL_IF(SCDetectSignatureSetAppProto(s, 3) < 0);
    FAIL_IF(s->init_data->alprotos[0] != ALPROTO_UNKNOWN);
    FAIL_IF(s->alproto != 3);
    alprotos[0] = 4;
    alprotos[1] = 3;
    alprotos[2] = ALPROTO_UNKNOWN;
    // check multiple containing singleton
    FAIL_IF(DetectSignatureSetMultiAppProto(s, alprotos) < 0);
    FAIL_IF(s->alproto != 3);

    // reset
    s->alproto = ALPROTO_UNKNOWN;
    alprotos[0] = 1;
    alprotos[1] = 2;
    alprotos[2] = 3;
    alprotos[3] = ALPROTO_UNKNOWN;
    FAIL_IF(DetectSignatureSetMultiAppProto(s, alprotos) < 0);
    // fail if set single not in multiple
    FAIL_IF(SCDetectSignatureSetAppProto(s, 4) >= 0);

    s->init_data->alprotos[0] = ALPROTO_UNKNOWN;
    s->alproto = ALPROTO_UNKNOWN;
    alprotos[0] = 1;
    alprotos[1] = 2;
    alprotos[2] = 3;
    alprotos[3] = ALPROTO_UNKNOWN;
    FAIL_IF(DetectSignatureSetMultiAppProto(s, alprotos) < 0);
    alprotos[0] = 4;
    alprotos[1] = 5;
    alprotos[2] = ALPROTO_UNKNOWN;
    // fail if multiple do not have intersection
    FAIL_IF(DetectSignatureSetMultiAppProto(s, alprotos) >= 0);

    s->init_data->alprotos[0] = ALPROTO_UNKNOWN;
    s->alproto = ALPROTO_UNKNOWN;
    alprotos[0] = 1;
    alprotos[1] = 2;
    alprotos[2] = 3;
    alprotos[3] = ALPROTO_UNKNOWN;
    FAIL_IF(DetectSignatureSetMultiAppProto(s, alprotos) < 0);
    alprotos[0] = 3;
    alprotos[1] = 4;
    alprotos[2] = 5;
    alprotos[3] = ALPROTO_UNKNOWN;
    // check multiple intersect to singleton
    FAIL_IF(DetectSignatureSetMultiAppProto(s, alprotos) < 0);
    FAIL_IF(s->alproto != 3);
    alprotos[0] = 5;
    alprotos[1] = 4;
    alprotos[2] = ALPROTO_UNKNOWN;
    // fail if multiple do not belong to singleton
    FAIL_IF(DetectSignatureSetMultiAppProto(s, alprotos) >= 0);

    SigFree(NULL, s);
    PASS;
}

static int DetectSetupDirection01(void)
{
    Signature *s = SigAlloc();
    FAIL_IF_NULL(s);
    // Basic case : ok
    char *str = (char *)"to_client";
    FAIL_IF(DetectSetupDirection(s, &str, true) < 0);
    SigFree(NULL, s);
    PASS;
}

static int DetectSetupDirection02(void)
{
    Signature *s = SigAlloc();
    FAIL_IF_NULL(s);
    char *str = (char *)"to_server";
    FAIL_IF(DetectSetupDirection(s, &str, true) < 0);
    // ok so far
    str = (char *)"to_client";
    FAIL_IF(DetectSetupDirection(s, &str, true) >= 0);
    // fails because we cannot have both to_client and to_server for same signature
    SigFree(NULL, s);
    PASS;
}

static int DetectSetupDirection03(void)
{
    Signature *s = SigAlloc();
    FAIL_IF_NULL(s);
    char *str = (char *)"to_client , something";
    FAIL_IF(DetectSetupDirection(s, &str, false) < 0);
    FAIL_IF(strcmp(str, "something") != 0);
    str = (char *)"to_client,something";
    FAIL_IF(DetectSetupDirection(s, &str, false) < 0);
    FAIL_IF(strcmp(str, "something") != 0);
    SigFree(NULL, s);
    PASS;
}

static int DetectSetupDirection04(void)
{
    Signature *s = SigAlloc();
    FAIL_IF_NULL(s);
    // invalid case
    char *str = (char *)"to_client_toto";
    FAIL_IF(DetectSetupDirection(s, &str, true) >= 0);
    // test we do not change the string pointer if only_dir is false
    str = (char *)"to_client_toto";
    FAIL_IF(DetectSetupDirection(s, &str, false) < 0);
    FAIL_IF(strcmp(str, "to_client_toto") != 0);
    str = (char *)"to_client,something";
    // fails because we call with only_dir=true
    FAIL_IF(DetectSetupDirection(s, &str, true) >= 0);
    SigFree(NULL, s);
    PASS;
}

#endif /* UNITTESTS */

#ifdef UNITTESTS
void DetectParseRegisterTests (void);
#include "tests/detect-parse.c"
#endif

void SigParseRegisterTests(void)
{
#ifdef UNITTESTS
    DetectParseRegisterTests();

    UtRegisterTest("SigParseTest01", SigParseTest01);
    UtRegisterTest("SigParseTest02", SigParseTest02);
    UtRegisterTest("SigParseTest03", SigParseTest03);
    UtRegisterTest("SigParseTest04", SigParseTest04);
    UtRegisterTest("SigParseTest05", SigParseTest05);
    UtRegisterTest("SigParseTest06", SigParseTest06);
    UtRegisterTest("SigParseTest07", SigParseTest07);
    UtRegisterTest("SigParseTest08", SigParseTest08);
    UtRegisterTest("SigParseTest09", SigParseTest09);
    UtRegisterTest("SigParseTest10", SigParseTest10);
    UtRegisterTest("SigParseTest11", SigParseTest11);
    UtRegisterTest("SigParseTest12", SigParseTest12);
    UtRegisterTest("SigParseTest13", SigParseTest13);
    UtRegisterTest("SigParseTest14", SigParseTest14);
    UtRegisterTest("SigParseTest15", SigParseTest15);
    UtRegisterTest("SigParseTest16", SigParseTest16);
    UtRegisterTest("SigParseTest17", SigParseTest17);
    UtRegisterTest("SigParseTest18", SigParseTest18);
    UtRegisterTest("SigParseTest19", SigParseTest19);
    UtRegisterTest("SigParseTest20", SigParseTest20);
    UtRegisterTest("SigParseTest21 -- address with space", SigParseTest21);
    UtRegisterTest("SigParseTest22 -- address with space", SigParseTest22);
    UtRegisterTest("SigParseTest23 -- carriage return", SigParseTest23);

    UtRegisterTest("SigParseBidirecTest06", SigParseBidirecTest06);
    UtRegisterTest("SigParseBidirecTest07", SigParseBidirecTest07);
    UtRegisterTest("SigParseBidirecTest08", SigParseBidirecTest08);
    UtRegisterTest("SigParseBidirecTest09", SigParseBidirecTest09);
    UtRegisterTest("SigParseBidirecTest10", SigParseBidirecTest10);
    UtRegisterTest("SigParseBidirecTest11", SigParseBidirecTest11);
    UtRegisterTest("SigParseBidirecTest12", SigParseBidirecTest12);
    UtRegisterTest("SigParseBidirecTest13", SigParseBidirecTest13);
    UtRegisterTest("SigParseBidirecTest14", SigParseBidirecTest14);
    UtRegisterTest("SigTestBidirec01", SigTestBidirec01);
    UtRegisterTest("SigTestBidirec02", SigTestBidirec02);
    UtRegisterTest("SigTestBidirec03", SigTestBidirec03);
    UtRegisterTest("SigTestBidirec04", SigTestBidirec04);
    UtRegisterTest("SigParseTestNegation01", SigParseTestNegation01);
    UtRegisterTest("SigParseTestNegation02", SigParseTestNegation02);
    UtRegisterTest("SigParseTestNegation03", SigParseTestNegation03);
    UtRegisterTest("SigParseTestNegation04", SigParseTestNegation04);
    UtRegisterTest("SigParseTestNegation05", SigParseTestNegation05);
    UtRegisterTest("SigParseTestNegation06", SigParseTestNegation06);
    UtRegisterTest("SigParseTestNegation07", SigParseTestNegation07);
    UtRegisterTest("SigParseTestNegation08", SigParseTestNegation08);
    UtRegisterTest("SigParseTestMpm01", SigParseTestMpm01);
    UtRegisterTest("SigParseTestMpm02", SigParseTestMpm02);
    UtRegisterTest("SigParseTestAppLayerTLS01", SigParseTestAppLayerTLS01);
    UtRegisterTest("SigParseTestAppLayerTLS02", SigParseTestAppLayerTLS02);
    UtRegisterTest("SigParseTestAppLayerTLS03", SigParseTestAppLayerTLS03);
    UtRegisterTest("SigParseTestUnbalancedQuotes01", SigParseTestUnbalancedQuotes01);

    UtRegisterTest("SigParseTestContentGtDsize01",
            SigParseTestContentGtDsize01);
    UtRegisterTest("SigParseTestContentGtDsize02",
            SigParseTestContentGtDsize02);

    UtRegisterTest("SigParseBidirWithSameSrcAndDest01",
            SigParseBidirWithSameSrcAndDest01);
    UtRegisterTest("SigParseBidirWithSameSrcAndDest02",
            SigParseBidirWithSameSrcAndDest02);
    UtRegisterTest("SigParseTestActionReject", SigParseTestActionReject);
    UtRegisterTest("SigParseTestActionDrop", SigParseTestActionDrop);

    UtRegisterTest("SigSetMultiAppProto", SigSetMultiAppProto);

    UtRegisterTest("DetectSetupDirection01", DetectSetupDirection01);
    UtRegisterTest("DetectSetupDirection02", DetectSetupDirection02);
    UtRegisterTest("DetectSetupDirection03", DetectSetupDirection03);
    UtRegisterTest("DetectSetupDirection04", DetectSetupDirection04);

#endif /* UNITTESTS */
}
