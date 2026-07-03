/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <pauln@niova.io> 2019
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <regex.h>
#include <stdio.h>
#include <linux/limits.h>

#include "common.h"
#include "config_token.h"
#include "ctl_interface.h"
#include "ctl_interface_cmd.h"
#include "file_util.h"
#include "io.h"
#include "log.h"
#include "random.h"
#include "registry.h"
#include "util_thread.h"

REGISTRY_ENTRY_FILE_GENERATE;

#define CTLIC_BUFFER_SIZE        4096
#define CTLIC_MAX_TOKENS_PER_REQ 8
#define CTLIC_MAX_VALUE_SIZE     (NAME_MAX + 1)
#define CTLIC_MAX_VALUE_DEPTH    32 // Max 'tree' depth which can be queried
#define CTLIC_MAX_TAB_DEPTH      (CTLIC_MAX_VALUE_DEPTH * 2)
#define CTLIC_MAX_SIBLING_CNT    16384

enum ctlic_cmd_input_output_files
{
    CTLIC_INPUT_FILE = 0,
    CTLIC_OUTPUT_FILE,
    CTLIC_NUM_FILES,
};

/* The entire ctl interface is single threaded, executed by the util_thread.
 * This means that only a single set of buffers are needed
 */
static util_thread_ctx_ctli_char_t
    ctlicBuffer[CTLIC_NUM_FILES][CTLIC_BUFFER_SIZE];

struct ctlic_path_regex_segment
{
    bool        cprs_regex_is_allocated;
    const char *cprs_str;
    regex_t     cprs_regex;
};

enum ctlic_depth_segment_type
{
    CTLIC_DEPTH_SEGMENT_TYPE__MIN = 0,
    CTLIC_DEPTH_SEGMENT_TYPE_KEY  = 0,
    CTLIC_DEPTH_SEGMENT_TYPE_VAL  = 1,
    CTLIC_DEPTH_SEGMENT_TYPE__MAX = 2,
};

struct ctlic_depth_segment
{
    unsigned int                    cds_tab_depth : 6;
    struct ctlic_path_regex_segment cds_cprs[CTLIC_DEPTH_SEGMENT_TYPE__MAX];
};

struct ctlic_matched_token
{
    const struct conf_token   *cmt_token;
    size_t                     cmt_value_len;
    char                       cmt_value[CTLIC_MAX_VALUE_SIZE + 1];
    size_t                     cmt_current_depth;
    size_t                     cmt_num_depth_segments;
    struct ctlic_depth_segment cmt_depth_segments[CTLIC_MAX_VALUE_DEPTH];
};

struct ctlic_file
{
    const char *cf_file_name;
    char       *cf_buffer;
    int         cf_fd;
    ssize_t     cf_nbytes_written;
};

struct ctlic_request
{
    enum lreg_user_types         cr_reg_user_type;
    size_t                       cr_num_matched_tokens;
    size_t                       cr_current_token;
    size_t                       cr_output_byte_cnt;
    size_t                       cr_current_tab_depth;
    struct stat                  cr_stb;
    char                         cr_value_buf[CTLIC_MAX_VALUE_SIZE + 1];
    struct ctlic_matched_token   cr_matched_token[CTLIC_MAX_TOKENS_PER_REQ];
    struct ctlic_file            cr_file[CTLIC_NUM_FILES];
    struct conf_token_set_parser cr_ctsp;
};

struct ctlic_iterator
{
    struct ctlic_request        *citer_cr;
    const struct ctlic_iterator *citer_parent;
    struct lreg_value            citer_lv;
    size_t                       citer_starting_byte_cnt;
    size_t                       citer_tab_depth;
    size_t                       citer_sibling_num;
    size_t                       citer_sibling_printed_cnt;
    bool                         citer_open_stanza;
    uint32_t                     citer_rand_id;
    uint32_t                     citer_error_cnt;
};

#define DBG_CITER(log_level, citer, fmt, ...)                                                              \
{                                                                                                          \
    LOG_MSG(                                                                                               \
        log_level,                                                                                         \
        "citer@%p-%08x p@%p-%08x lv=%s depth=%zu sib-num:nprint=%zu:%zu p-nprint=%zu pdepth=%zu o=%d "fmt, \
        citer, (citer)->citer_rand_id, (citer)->citer_parent,                                              \
        (citer)->citer_parent ? (citer)->citer_parent->citer_rand_id : 0,                                  \
        LREG_VALUE_TO_KEY_STR(&(citer)->citer_lv),                                                         \
        (citer)->citer_tab_depth, (citer)->citer_sibling_num,                                              \
        (citer)->citer_sibling_printed_cnt,                                                                \
        (citer)->citer_parent ?                                                                            \
            (citer)->citer_parent->citer_sibling_printed_cnt : 0,                                          \
        (citer)->citer_parent ?                                                                            \
            (citer)->citer_parent->citer_tab_depth : 0,                                                    \
        (citer)->citer_open_stanza,                                                                        \
        ##__VA_ARGS__);                                                                                    \
}

static void
ctlic_matched_token_init(struct ctlic_matched_token *cmt)
{
    if (cmt)
    {
        cmt->cmt_token = NULL;
        cmt->cmt_value_len = 0;
        memset(cmt->cmt_value, 0, CTLIC_MAX_VALUE_SIZE + 1);
    }
}

static void
ctlic_request_prepare(struct ctlic_request *cr)
{
    if (cr)
    {
        cr->cr_num_matched_tokens = 0;

        for (int i = 0; i < CTLIC_MAX_TOKENS_PER_REQ; i++)
            ctlic_matched_token_init(&cr->cr_matched_token[i]);

        for (int i = 0; i < CTLIC_NUM_FILES; i++)
        {
            cr->cr_file[i].cf_nbytes_written = 0;
            cr->cr_file[i].cf_file_name = NULL;
            cr->cr_file[i].cf_fd = -1;
            cr->cr_file[i].cf_buffer = ctlicBuffer[i];

            memset(cr->cr_file[i].cf_buffer, 0, CTLIC_BUFFER_SIZE);
        }
    }
}

static void
ctlic_regex_free(struct ctlic_path_regex_segment *cprs)
{
    if (cprs && cprs->cprs_regex_is_allocated)
        regfree(&cprs->cprs_regex);
}

static void
ctlic_request_done(struct ctlic_request *cr)
{
    if (!cr)
        return;

    for (int i = 0; i < CTLIC_NUM_FILES; i++)
    {
        if (cr->cr_file[i].cf_fd >= 0)
        {
            close(cr->cr_file[i].cf_fd);
            cr->cr_file[i].cf_fd = -1;
        }
    }

    for (size_t i = 0; i < cr->cr_num_matched_tokens; i++)
    {
        struct ctlic_matched_token *cmt = &cr->cr_matched_token[i];

        for (size_t j = 0; j < cmt->cmt_num_depth_segments; j++)
        {
            struct ctlic_depth_segment *cds = &cmt->cmt_depth_segments[j];

            for (int k = 0; k < CTLIC_DEPTH_SEGMENT_TYPE__MAX; k++)
                ctlic_regex_free(&cds->cds_cprs[k]);
        }
    }
}

#define CTLIC_OUTPUT_TMP_FILE(tmp_str, file_name)                \
    const ssize_t CTLIC_OUTPUT_TMP_FILE_file_name_len =           \
        NUM_HEX_CHARS(pid_t) + strnlen((file_name), PATH_MAX);   \
    if (CTLIC_OUTPUT_TMP_FILE_file_name_len >= PATH_MAX - 2)     \
        return -ENAMETOOLONG;                                    \
    DECL_AND_FMT_STRING((tmp_str),                               \
                        CTLIC_OUTPUT_TMP_FILE_file_name_len + 2, \
                        ".%s.%x", file_name, getpid());

static int
ctlic_rename_output_file(int out_dirfd, struct ctlic_request *cr)
{
    if (out_dirfd < 0 || !cr || !cr->cr_num_matched_tokens)
        return -EINVAL;

    struct ctlic_file *cf = &cr->cr_file[CTLIC_OUTPUT_FILE];
    if (cf->cf_fd < 0 || !cf->cf_file_name)
        return -EINVAL;

    CTLIC_OUTPUT_TMP_FILE(tmp_name, cf->cf_file_name);

    return renameat(out_dirfd, tmp_name, out_dirfd, cf->cf_file_name);
}

static int
ctlic_open_output_file(int out_dirfd, struct ctlic_request *cr)
{
    if (out_dirfd < 0 || !cr || !cr->cr_num_matched_tokens)
        return -EINVAL;

    struct ctlic_file *cf = &cr->cr_file[CTLIC_OUTPUT_FILE];
    if (cf->cf_fd >= 0 || cf->cf_file_name)
        return -EINVAL;

    bool found = false;
    const struct ctlic_matched_token *cmt = NULL;

    /* Set the output file name.
     */
    for (size_t i = 0; i < cr->cr_num_matched_tokens; i++)
    {
        cmt = &cr->cr_matched_token[i];
        if (!cmt->cmt_token) // Something went badly wrong here..
            return -EINVAL;

        if (cmt->cmt_token->ct_id == CT_ID_OUTFILE)
        {
            found = true;
            break;
        }
    }

    if (!found || cmt->cmt_num_depth_segments != 1 ||
        !cmt->cmt_depth_segments[0].cds_cprs[0].cprs_str)
        return -EBADMSG;

    cf->cf_file_name = cmt->cmt_depth_segments[0].cds_cprs[0].cprs_str;

    /* See macro definition above (the function may return from here).
     */
    CTLIC_OUTPUT_TMP_FILE(tmp_name, cf->cf_file_name);

    cf->cf_fd = openat(out_dirfd, tmp_name,
                       O_WRONLY | O_CREAT | O_TRUNC, 0644);

    return cf->cf_fd < 0 ? -errno : 0;
}

static int
ctlic_open_and_read_input_file(const struct ctli_cmd_handle *cch,
                               struct ctlic_request *cr)
{
    const char *input_cmd_file = cch ? cch->ctlih_input_file_name : NULL;
    if (!input_cmd_file || !cr)
        return -EINVAL;

    /* Init and assign buffers.
     */
    ctlic_request_prepare(cr);

    struct ctlic_file *cf_in = &cr->cr_file[CTLIC_INPUT_FILE];

    cf_in->cf_file_name = input_cmd_file;

    cf_in->cf_nbytes_written =
        file_util_open_and_read(cch->ctlih_input_dirfd, input_cmd_file,
                                cf_in->cf_buffer, CTLIC_BUFFER_SIZE, NULL,
                                &cr->cr_stb);

    if (cf_in->cf_nbytes_written < 0)
    {
        ctlic_request_done(cr);
        return (int)cf_in->cf_nbytes_written;
    }

    return 0;
}

static int
ctlic_regex_compile(struct ctlic_path_regex_segment *cprs)
{
    if (!cprs)
        return -EINVAL;

    else if (!cprs->cprs_str)
        return 0;

    int rc = regcomp(&cprs->cprs_regex, cprs->cprs_str, REG_NOSUB);
    if (rc)
    {
        char err_str[64] = {0};
        regerror(rc, &cprs->cprs_regex, err_str, 63);

        LOG_MSG(LL_DEBUG, "regcomp(`%s'): %s", cprs->cprs_str, err_str);

        return -EBADMSG;
    }

    LOG_MSG(LL_DEBUG, "%s regcomp():  OK", cprs->cprs_str);
    cprs->cprs_regex_is_allocated = true;

    return 0;
}

static int
ctlic_prepare_token_values(struct ctlic_matched_token *cmt)
{
    if (!cmt)
        return -EINVAL;

    for (size_t i = 0; i < cmt->cmt_num_depth_segments; i++)
    {
        struct ctlic_depth_segment *cds = &cmt->cmt_depth_segments[i];

        for (int j = 0; j < CTLIC_DEPTH_SEGMENT_TYPE__MAX; j++)
        {
            struct ctlic_path_regex_segment *cprs = &cds->cds_cprs[j];

            int rc = ctlic_regex_compile(cprs);
            if (rc)
                return rc;
        }
    }

    return 0;
}

static int
ctlic_parse_token_value_GET(struct ctlic_matched_token *cmt)
{
    if (!cmt ||
        !cmt->cmt_value_len ||
        !cmt->cmt_token ||
        (cmt->cmt_token->ct_id != CT_ID_GET &&
         cmt->cmt_token->ct_id != CT_ID_OUTFILE &&
         cmt->cmt_token->ct_id != CT_ID_WHERE) ||
        cmt->cmt_num_depth_segments ||
        cmt->cmt_value_len > CTLIC_MAX_VALUE_SIZE ||
        cmt->cmt_value[0] != '/')
        return -EINVAL;

    bool escape_char = false;
    bool prev_char_was_separator = false;
    bool prev_char_was_at_sign = false;

    for (size_t i = 0; i < cmt->cmt_value_len; i++)
    {
        const bool segment_separator =
            cmt->cmt_value[i] == '/' ||
            (cmt->cmt_token->ct_id == CT_ID_WHERE &&
             cmt->cmt_value[i] == '&');

        if (cmt->cmt_value[i] == '\\')
        {
            escape_char = true;
            continue;
        }
        else if (segment_separator && !escape_char)
        {
            if (prev_char_was_at_sign)
                return -EBADMSG;

            cmt->cmt_value[i] = '\0';
            prev_char_was_separator = true;
            escape_char = false;
            continue;
        }
        else if (cmt->cmt_value[i] == '@' && !escape_char)
        {
            cmt->cmt_value[i] = '\0';
            prev_char_was_at_sign = true;
            escape_char = false;
            continue;
        }
        else if (prev_char_was_separator)
        {
            if (cmt->cmt_num_depth_segments == CTLIC_MAX_VALUE_DEPTH)
                return -E2BIG;

            struct ctlic_depth_segment *cds =
                &cmt->cmt_depth_segments[cmt->cmt_num_depth_segments++];

            cds->cds_cprs[CTLIC_DEPTH_SEGMENT_TYPE_KEY].cprs_str =
                &cmt->cmt_value[i];
        }
        else if (prev_char_was_at_sign)
        {
            if (prev_char_was_separator ||
                cmt->cmt_num_depth_segments == 0 ||
                cmt->cmt_num_depth_segments >= CTLIC_MAX_VALUE_DEPTH)
                return -EBADMSG;

            struct ctlic_depth_segment *cds =
                &cmt->cmt_depth_segments[cmt->cmt_num_depth_segments - 1];

            if (!cds->cds_cprs[CTLIC_DEPTH_SEGMENT_TYPE_KEY].cprs_str)
                return -EBADMSG;

            cds->cds_cprs[CTLIC_DEPTH_SEGMENT_TYPE_VAL].cprs_str =
                &cmt->cmt_value[i];
        }

        prev_char_was_at_sign = false;
        prev_char_was_separator = false;
        escape_char = false;
    }

    return 0;
}

static int
ctlic_parse_token_value_APPLY(struct ctlic_matched_token *cmt)
{
    if (!cmt ||
        !cmt->cmt_value_len ||
        !cmt->cmt_token ||
        cmt->cmt_token->ct_id != CT_ID_APPLY ||
        cmt->cmt_num_depth_segments ||
        cmt->cmt_value_len > CTLIC_MAX_VALUE_SIZE)
        return -EINVAL;

    bool escape_char = false;
    bool prev_char_was_at_sign = false;
    bool at_sign_was_found = false;

    struct ctlic_depth_segment *cds = &cmt->cmt_depth_segments[0];
    cmt->cmt_num_depth_segments = 1; // There's only ever one segment

    struct ctlic_path_regex_segment *cprs = &cds->cds_cprs[0];
    cprs->cprs_str = &cmt->cmt_value[0];

    for (size_t i = 0; i < cmt->cmt_value_len; i++)
    {
        if (cmt->cmt_value[i] == '\\')
        {
            escape_char = true;
            continue;
        }
        else if (cmt->cmt_value[i] == '@' && !escape_char)
        {
            cmt->cmt_value[i] = '\0';

            at_sign_was_found = true;
            prev_char_was_at_sign = true;
            escape_char = false;

            continue;
        }
        else if (prev_char_was_at_sign)
        {
            cds->cds_cprs[CTLIC_DEPTH_SEGMENT_TYPE_VAL].cprs_str =
                &cmt->cmt_value[i];

            break;
        }

        prev_char_was_at_sign = false;
        escape_char = false;
    }

    return at_sign_was_found ? 0 : -EBADMSG;
}

static int
ctlic_parse_token_value(struct ctlic_matched_token *cmt)
{
    if (!cmt || !cmt->cmt_token)
        return -EINVAL;

    int rc = -EINVAL;

    switch (cmt->cmt_token->ct_id)
    {
    case CT_ID_GET:     // fall through
    case CT_ID_OUTFILE:
    case CT_ID_WHERE:
        rc = ctlic_parse_token_value_GET(cmt);
        break;
    case CT_ID_APPLY:
        rc = ctlic_parse_token_value_APPLY(cmt);
        break;
    default:
        break;
    };

    return rc ? rc : ctlic_prepare_token_values(cmt);
}

static void
ctlic_dump_request_items(const struct ctlic_request *cr)
{
    if (!cr)
        return;

    for (size_t i = 0; i < cr->cr_num_matched_tokens; i++)
    {
        if (cr->cr_matched_token[i].cmt_token)
        {
            LOG_MSG(LL_DEBUG, "(%s) %s -> `%s'",
                    cr->cr_file[CTLIC_INPUT_FILE].cf_file_name,
                    cr->cr_matched_token[i].cmt_token->ct_name,
                    cr->cr_matched_token[i].cmt_value);
        }
    }
}

static int
ctlic_parse_request_values(struct ctlic_request *cr)
{
    if (!cr || cr->cr_num_matched_tokens > CTLIC_MAX_TOKENS_PER_REQ)
        return -EINVAL;

    for (size_t i = 0; i < cr->cr_num_matched_tokens; i++)
    {
        int rc = ctlic_parse_token_value(&cr->cr_matched_token[i]);

        if (rc)
            return rc;
    }

    return 0;
}

/**
 * ctlic_ctsp_cb - config token parsing callback for the ctl_interface_cmd
 *   handler subsystem.  This callback doesn't do much other than
 */
static util_thread_ctx_ctli_int_t
ctlic_ctsp_cb(const struct conf_token *ct, const char *val_buf,
              size_t val_buf_sz, void *cb_arg, int error)
{
    if (!ct || !val_buf || !val_buf_sz || !cb_arg)
        return -EINVAL;

    if (error)
        return error;

    struct ctlic_request *cr = cb_arg;

    if (cr->cr_num_matched_tokens >= CTLIC_MAX_TOKENS_PER_REQ)
        return -E2BIG;
    else if (val_buf_sz >= CTLIC_MAX_VALUE_SIZE)
        return -EOVERFLOW;

    struct ctlic_matched_token *cmt =
        &cr->cr_matched_token[cr->cr_num_matched_tokens++];

    cmt->cmt_token = ct;
    cmt->cmt_value_len = val_buf_sz;

    strncpy(cmt->cmt_value, val_buf, val_buf_sz);

    LOG_MSG(LL_DEBUG, "token-name %10s val_sz %02zu err %01d %s",
            ct->ct_name, val_buf_sz, error, val_buf);

    return 0;
}

static int
ctlic_parse_request(struct ctlic_request *cr)
{
    if (!cr)
        return -EINVAL;

    const struct ctlic_file *cf_in = &cr->cr_file[CTLIC_INPUT_FILE];

    if (!cf_in->cf_buffer)
        return -EINVAL;

    /* File contents have been read into 'file_buf'.
     */
    memset(&cr->cr_ctsp, 0, sizeof(struct conf_token_set_parser));

    conf_token_set_init(&cr->cr_ctsp.ctsp_cts);
    conf_token_set_enable(&cr->cr_ctsp.ctsp_cts, CT_ID_GET);
    conf_token_set_enable(&cr->cr_ctsp.ctsp_cts, CT_ID_APPLY);
    conf_token_set_enable(&cr->cr_ctsp.ctsp_cts, CT_ID_WHERE);
    conf_token_set_enable(&cr->cr_ctsp.ctsp_cts, CT_ID_OUTFILE);

    conf_token_set_parser_init(&cr->cr_ctsp, cf_in->cf_buffer,
                               cf_in->cf_nbytes_written, cr->cr_value_buf,
                               CTLIC_MAX_VALUE_SIZE, ctlic_ctsp_cb, cr);

    int rc = conf_token_set_parse(&cr->cr_ctsp);
    if (rc)
        return rc;

    ctlic_dump_request_items(cr);

    rc = ctlic_parse_request_values(cr);
    if (rc)
        return rc;

    return 0;
}

static struct ctlic_matched_token *
ctlic_get_current_matched_token(struct ctlic_request *cr)
{
    NIOVA_ASSERT(cr && cr->cr_current_token < CTLIC_MAX_TOKENS_PER_REQ);

    struct ctlic_matched_token *cmt =
        &cr->cr_matched_token[cr->cr_current_token];

    NIOVA_ASSERT(cmt->cmt_token);
    NIOVA_ASSERT(cmt->cmt_num_depth_segments < CTLIC_MAX_VALUE_DEPTH);

    return cmt;
}

static const char *
ctlic_scan_registry_sibling_helper(const struct ctlic_iterator *citer)
{
    const struct lreg_value *lv = &citer->citer_lv;

    const char *ret = "";

    /* If our sibling number is greater than 0 then always apply a comma to the
     * outgoing JSON stream.  Otherwise, if our parent's sibling count is
     * positive and we're object or array, then print a comma.
     */
    if (citer->citer_sibling_printed_cnt ||
        (citer->citer_parent &&
         citer->citer_parent->citer_sibling_printed_cnt > 0 &&
         (LREG_VALUE_TO_REQ_TYPE(lv) == LREG_VAL_TYPE_OBJECT ||
          LREG_VALUE_TO_REQ_TYPE(lv) == LREG_VAL_TYPE_ARRAY ||
          LREG_VALUE_TO_REQ_TYPE(lv) == LREG_VAL_TYPE_VARRAY ||
          LREG_VALUE_TO_REQ_TYPE(lv) == LREG_VAL_TYPE_VOBJECT ||
          LREG_VALUE_TO_REQ_TYPE(lv) == LREG_VAL_TYPE_ANON_OBJECT)))
        ret = ",";

    DBG_CITER(LL_DEBUG, citer, "ret=%s", ret);

    return ret;
}

static const char *
ctlic_citer_2_value_string(const struct ctlic_iterator *citer)
{
    if (!citer)
        return NULL;

    const char *value_string;

    switch (LREG_VALUE_TO_REQ_TYPE(&citer->citer_lv))
    {
    case LREG_VAL_TYPE_OBJECT:
    case LREG_VAL_TYPE_VOBJECT:
    case LREG_VAL_TYPE_ANON_OBJECT:
        value_string = citer->citer_open_stanza ? "{" : "}";
        break;
    case LREG_VAL_TYPE_ARRAY:
    case LREG_VAL_TYPE_VARRAY:
        value_string = citer->citer_open_stanza ? "[" : "]";
        break;
    default:
        value_string = citer->citer_open_stanza ?
            LREG_VALUE_TO_OUT_STR(&citer->citer_lv) : NULL;
        break;
    }

    return value_string;
}

static int
ctlic_scan_registry_cb_output_writer(struct ctlic_iterator *citer)
{
    if (!citer || !citer->citer_cr)
        return -EINVAL;

    else if (citer->citer_tab_depth > CTLIC_MAX_TAB_DEPTH ||
             citer->citer_sibling_num > CTLIC_MAX_SIBLING_CNT)
        return -E2BIG;

    int rc = 0;

    struct ctlic_request *cr = citer->citer_cr;
    const bool open_stanza = citer->citer_open_stanza;
    const struct lreg_value *lv = &citer->citer_lv;
    const ssize_t tab_depth = citer->citer_tab_depth;
    const size_t sibling_number = citer->citer_sibling_num;
    const size_t starting_byte_cnt = citer->citer_starting_byte_cnt;
    const char *value_string = ctlic_citer_2_value_string(citer);

    LOG_MSG(LL_DEBUG, "key=`%s' depth=%zu sib-num=%zu open=%d",
            lv->lrv_key_string, tab_depth, sibling_number, open_stanza);

    DECL_AND_INIT_STRING(tab_array, CTLIC_MAX_TAB_DEPTH, '\t', tab_depth);

    if (open_stanza)
    {
        if (!tab_depth)
        {
            rc = dprintf(cr->cr_file[CTLIC_OUTPUT_FILE].cf_fd, "{");
        }
        else
        {
            switch (LREG_VALUE_TO_REQ_TYPE(lv))
            {
            case LREG_VAL_TYPE_ANON_OBJECT:
                rc = dprintf(cr->cr_file[CTLIC_OUTPUT_FILE].cf_fd,
                             "%s\n%s%s",
                             ctlic_scan_registry_sibling_helper(citer),
                             tab_array,
                             value_string);
                break;
            case LREG_VAL_TYPE_ARRAY:
            case LREG_VAL_TYPE_VARRAY:
            case LREG_VAL_TYPE_VOBJECT:
            case LREG_VAL_TYPE_OBJECT:
                rc = dprintf(cr->cr_file[CTLIC_OUTPUT_FILE].cf_fd,
                             "%s\n%s\"%s\" : %s",
                             ctlic_scan_registry_sibling_helper(citer),
                             tab_array,
                             lv->lrv_key_string,
                             value_string);
                break;
            case LREG_VAL_TYPE_STRING:
                rc = dprintf(cr->cr_file[CTLIC_OUTPUT_FILE].cf_fd,
                             "%s\n%s\"%s\" : \"%s\"",
                             ctlic_scan_registry_sibling_helper(citer),
                             tab_array,
                             lv->lrv_key_string,
                             value_string);
                break;
            case LREG_VAL_TYPE_BOOL:
                rc = dprintf(cr->cr_file[CTLIC_OUTPUT_FILE].cf_fd,
                             "%s\n%s\"%s\" : %s",
                             ctlic_scan_registry_sibling_helper(citer),
                             tab_array,
                             lv->lrv_key_string,
                             LREG_VALUE_TO_BOOL(lv) ?
                             "true" : "false");
                break;
            case LREG_VAL_TYPE_SIGNED_VAL:
                rc = dprintf(cr->cr_file[CTLIC_OUTPUT_FILE].cf_fd,
                             "%s\n%s\"%s\" : %ld",
                             ctlic_scan_registry_sibling_helper(citer),
                             tab_array,
                             lv->lrv_key_string,
                             LREG_VALUE_TO_OUT_SIGNED_INT(lv));
                break;
            case LREG_VAL_TYPE_SIGNED32_VAL:
                rc = dprintf(cr->cr_file[CTLIC_OUTPUT_FILE].cf_fd,
                             "%s\n%s\"%s\" : %d",
                             ctlic_scan_registry_sibling_helper(citer),
                             tab_array,
                             lv->lrv_key_string,
                             (int)LREG_VALUE_TO_OUT_SIGNED_INT(lv));
                break;
            case LREG_VAL_TYPE_UNSIGNED_VAL:
                rc = dprintf(cr->cr_file[CTLIC_OUTPUT_FILE].cf_fd,
                             "%s\n%s\"%s\" : %lu",
                             ctlic_scan_registry_sibling_helper(citer),
                             tab_array,
                             lv->lrv_key_string,
                             LREG_VALUE_TO_OUT_UNSIGNED_INT(lv));
                break;
            case LREG_VAL_TYPE_FLOAT_VAL:
                rc = dprintf(cr->cr_file[CTLIC_OUTPUT_FILE].cf_fd,
                             "%s\n%s\"%s\" : %f",
                             ctlic_scan_registry_sibling_helper(citer),
                             tab_array,
                             lv->lrv_key_string,
                             LREG_VALUE_TO_OUT_FLOAT(lv));
                break;
            default:
                break;
            }
        }
    }
    else if (value_string) // Close stanza
    {
        rc = dprintf(cr->cr_file[CTLIC_OUTPUT_FILE].cf_fd,
                     "%s%s%s%s",
                     cr->cr_output_byte_cnt > starting_byte_cnt ?
                     "\n" : "",
                     cr->cr_output_byte_cnt > starting_byte_cnt ?
                     tab_array : "",
                     value_string,
                     // Add a newline if this closes the final stanza
                     !tab_depth ? "\n" : "");
    }

    if (rc > 0)
        cr->cr_output_byte_cnt += rc;

    citer->citer_open_stanza = false;
    citer->citer_starting_byte_cnt = cr->cr_output_byte_cnt;

    return rc >= 0 ? 0 : -errno;
}

static void
ctlic_insert_error_kv_lrv_init(struct lreg_value *lv)
{
    NIOVA_ASSERT(lv);

    LREG_VALUE_TO_REQ_TYPE(lv) = LREG_VAL_TYPE_STRING;
    snprintf(lv->lrv_key_string, LREG_VALUE_STRING_MAX, "error");
}

static int
ctlic_insert_error_kv_init_EISDIR(struct ctlic_iterator *err_citer, int depth)
{
    if (!err_citer || depth <= 0)
        return -EINVAL;

    ctlic_insert_error_kv_lrv_init(&err_citer->citer_lv);

    const struct ctlic_matched_token *cmt =
        ctlic_get_current_matched_token(err_citer->citer_cr);

    const struct ctlic_depth_segment *cds =
        &cmt->cmt_depth_segments[depth - 1];

    int rc = snprintf(
        err_citer->citer_lv.get.lrv_value_out.lrv_string,
        LREG_VALUE_STRING_MAX,
        "May not apply value filter `%s' to array or object `%s'",
        cds->cds_cprs[CTLIC_DEPTH_SEGMENT_TYPE_VAL].cprs_str,
        cds->cds_cprs[CTLIC_DEPTH_SEGMENT_TYPE_KEY].cprs_str);

    (void)rc; // For now, err msg truncation is ignored

    return 0;
}

static const struct ctlic_matched_token *
ctlic_get_next_apply_token(const struct ctlic_request *cr);

/**
 * ctlic_insert_error_kv_init_EPERM - called when an APPLY operation has
 *    failed.  It is implied that an 'apply' token is present.
 */
static int
ctlic_insert_error_kv_init_EPERM(struct ctlic_iterator *err_citer, int depth)
{
    if (!err_citer || !err_citer->citer_cr || depth <= 0)
        return -EINVAL;

    const struct ctlic_matched_token *apply_token =
        ctlic_get_next_apply_token(err_citer->citer_cr);

    if (!apply_token || apply_token->cmt_num_depth_segments != 1)
        return -EBADMSG;

    ctlic_insert_error_kv_lrv_init(&err_citer->citer_lv);

    const struct ctlic_depth_segment *cds =
        &apply_token->cmt_depth_segments[0];

    int rc = snprintf(err_citer->citer_lv.get.lrv_value_out.lrv_string,
                      LREG_VALUE_STRING_MAX, "Key `%s' is read-only",
                      cds->cds_cprs[CTLIC_DEPTH_SEGMENT_TYPE_KEY].cprs_str);

    (void)rc; // For now, err msg truncation is ignored

    return 0;
}

static int
ctlic_insert_error_kv_init_generic(struct ctlic_iterator *err_citer, int depth,
                                   const int error)
{
    if (!err_citer || depth <= 0)
        return -EINVAL;

    ctlic_insert_error_kv_lrv_init(&err_citer->citer_lv);

    const struct ctlic_depth_segment *cds = NULL;

    const struct ctlic_matched_token *cmt =
        ctlic_get_next_apply_token(err_citer->citer_cr);

    if (cmt)
    {
        cds = &cmt->cmt_depth_segments[0];
    }
    else
    {
        cmt = ctlic_get_current_matched_token(err_citer->citer_cr);
        if (cmt)
            cds = &cmt->cmt_depth_segments[depth - 1];
    }

    if (!cds)
        return -EINVAL;

    int rc = snprintf(
        err_citer->citer_lv.get.lrv_value_out.lrv_string,
        LREG_VALUE_STRING_MAX,
        "command `%s' with value `%s' failed: %s",
        cds->cds_cprs[CTLIC_DEPTH_SEGMENT_TYPE_KEY].cprs_str,
        cds->cds_cprs[CTLIC_DEPTH_SEGMENT_TYPE_VAL].cprs_str,
        strerror(-error));

    (void)rc; // For now, err msg truncation is ignored

    return 0;
}

static int
ctlic_insert_error_kv_into_outfile(struct ctlic_iterator *in_citer,
                                   const int depth, const int error)
{
    if (!in_citer || depth <= 0 || !error)
        return -EINVAL;

    struct ctlic_iterator tmp_citer = {0};
    bool citer_is_root = false;

    // Create a level-1 citer, don't use the root
    if (in_citer->citer_parent == NULL)
    {
        citer_is_root = true;

        tmp_citer = *in_citer;
        tmp_citer.citer_tab_depth += 1;
        tmp_citer.citer_open_stanza = true;
        tmp_citer.citer_parent = in_citer;
    }

    struct ctlic_iterator *citer = citer_is_root ? &tmp_citer : in_citer;
    int rc;

    switch (error)
    {
    case -EISDIR:
        rc = ctlic_insert_error_kv_init_EISDIR(citer, depth);
        break;
    case -EPERM:
        rc = ctlic_insert_error_kv_init_EPERM(citer, depth);
        break;
    default:
        rc = ctlic_insert_error_kv_init_generic(citer, depth, error);
        break;
    }

    if (!rc)
        ctlic_scan_registry_cb_output_writer(citer);

    return rc;
}

static const char *
ctlic_regex_get_compare_string_from_lv(const struct lreg_value *lv,
                                       enum ctlic_depth_segment_type cds_type,
                                       char *outbuf, size_t outbuf_len,
                                       int *error)
{
    NIOVA_ASSERT(error);

    if (!lv || !outbuf || !outbuf_len ||
        (cds_type != CTLIC_DEPTH_SEGMENT_TYPE_KEY &&
         cds_type != CTLIC_DEPTH_SEGMENT_TYPE_VAL))
    {
        *error = -EINVAL;
        return NULL;
    }

    *error = 0;

    // Keys are always strings
    if (cds_type == CTLIC_DEPTH_SEGMENT_TYPE_KEY)
        return LREG_VALUE_TO_KEY_STR(lv);

    // type == CTLIC_DEPTH_SEGMENT_TYPE_VAL
    switch (LREG_VALUE_TO_REQ_TYPE(lv))
    {
    case LREG_VAL_TYPE_STRING:
        return LREG_VALUE_TO_OUT_STR(lv);

    // The rest need string conversions
    case LREG_VAL_TYPE_BOOL:
        snprintf(outbuf, outbuf_len, "%s",
                 LREG_VALUE_TO_BOOL(lv) ? "true" : "false");
        return outbuf;

    case LREG_VAL_TYPE_SIGNED_VAL:
        snprintf(outbuf, outbuf_len, "%ld", LREG_VALUE_TO_OUT_SIGNED_INT(lv));
        return outbuf;

    case LREG_VAL_TYPE_UNSIGNED_VAL:
        snprintf(outbuf, outbuf_len, "%lu",
                 LREG_VALUE_TO_OUT_UNSIGNED_INT(lv));
        return outbuf;

    case LREG_VAL_TYPE_FLOAT_VAL:
        snprintf(outbuf, outbuf_len, "%f", LREG_VALUE_TO_OUT_FLOAT(lv));
        return outbuf;

    default:
        /* keys types such as LREG_VAL_TYPE_ARRAY and LREG_VAL_TYPE_OBJECT
         * do not hold regex-able string values.
         */
        *error = -EISDIR;
        break;
    }

    return NULL;
}

static int
ctlic_regex_test_depth_segment(struct lreg_node *lrn,
                               struct ctlic_depth_segment *cds,
                               const struct lreg_value *lv, const int depth,
                               bool *regex_matches)
{
    if (!lrn || !cds || !lv || !regex_matches)
        return false;

    *regex_matches = true;

    int rc = 0;

    const bool has_value_regex =
        cds->cds_cprs[CTLIC_DEPTH_SEGMENT_TYPE_VAL].cprs_regex_is_allocated;

    for (enum ctlic_depth_segment_type i = CTLIC_DEPTH_SEGMENT_TYPE__MIN;
         i < CTLIC_DEPTH_SEGMENT_TYPE__MAX; i++)
    {
        struct ctlic_path_regex_segment *cprs = &cds->cds_cprs[i];

        if (!cprs->cprs_str || !cprs->cprs_regex_is_allocated)
        {
            if (i == CTLIC_DEPTH_SEGMENT_TYPE_KEY)
            {
                *regex_matches = false; // this shouldn't happen
                rc = -EINVAL;

                DBG_LREG_NODE(
                    LL_NOTIFY, lrn,
                    "key segment is NULL (depth=%d) (lv-key=%s)",
                    depth, LREG_VALUE_TO_KEY_STR(lv));
            }
            break;
        }

        int regex_rc = 0;
        char outbuf[LREG_VALUE_STRING_MAX + 1] = {0};
        const char *compare_str =
            ctlic_regex_get_compare_string_from_lv(lv, i, outbuf,
                                                   LREG_VALUE_STRING_MAX,
                                                   &rc);
        if (!rc && compare_str)
            regex_rc = regexec(&cprs->cprs_regex, compare_str, 0, NULL, 0);

        DBG_LREG_NODE(
            LL_DEBUG, lrn,
            "rc=%d matched: %s (depth=%d, cds-type=%d) (cds=%s) compare=%s",
            rc, (rc || regex_rc) ? "no" : "yes", depth, i,
            cprs->cprs_str, compare_str);

        if ((rc || regex_rc) ||
            (!has_value_regex &&
             (lrn->lrn_ignore_items_with_value_zero &&
              lreg_value_is_numeric(lv) &&
              LREG_VALUE_TO_OUT_SIGNED_INT(lv) == 0)))
        {
            if (!rc && !LREG_VALUE_TO_OUT_SIGNED_INT(lv))
                DBG_LREG_NODE(LL_DEBUG, lrn, "skip zero-value entry (key=%s)",
                              LREG_VALUE_TO_KEY_STR(lv));
            *regex_matches = false;
            break;
        }
    }

    return rc;
}

static const struct ctlic_matched_token *
ctlic_get_next_apply_token(const struct ctlic_request *cr)
{
    if (!cr)
        return NULL;

    for (size_t i = 0; i < cr->cr_num_matched_tokens; i++)
    {
        const struct conf_token *ct = cr->cr_matched_token[i].cmt_token;
        if (!ct)
            break;

        else if (ct->ct_id == CT_ID_APPLY)
            return &cr->cr_matched_token[i];
    }

    return NULL;
}

/**
 * ctlic_try_apply_here - given a lreg node and ctlic_request, try to apply
 *    the contents of an APPLY directive to the specified key in this node.
 *    This function will perform several checks, most importantly is that the
 *    key specified in the APPLY directive exists in the provided lrn - it is
 *    not the role of the caller to perform this check.
 */
static int
ctlic_try_apply_here(struct lreg_node *lrn, const struct ctlic_request *cr)
{
    struct lreg_value lv = {0};

    const struct ctlic_matched_token *apply_token =
        ctlic_get_next_apply_token(cr);

    if (!apply_token || apply_token->cmt_num_depth_segments != 1)
        return -EBADMSG;

    const struct ctlic_depth_segment *cds =
        &apply_token->cmt_depth_segments[0];

    // Map the key and 'new' value strings
    const struct ctlic_path_regex_segment *cprs_key =
        &cds->cds_cprs[CTLIC_DEPTH_SEGMENT_TYPE_KEY];

    const struct ctlic_path_regex_segment *cprs_val =
        &cds->cds_cprs[CTLIC_DEPTH_SEGMENT_TYPE_VAL];

    if (!cprs_key->cprs_str || !cprs_val->cprs_str)
        return -EBADMSG;

    int rc = lreg_node_key_lookup(lrn, &lv, cprs_key->cprs_str,
                                  CTLIC_MAX_VALUE_SIZE);
    if (rc)
    {
        DBG_LREG_NODE(LL_DEBUG, lrn, "lreg_node_key_lookup(`%s'): %s",
                      cprs_key->cprs_str, strerror(-rc));
    }
    else
    {
        // Make the necessary preparations for the 'write' cmd.
        LREG_VALUE_TO_REQ_TYPE_IN(&lv) = LREG_VAL_TYPE_STRING;
        strncpy(LREG_VALUE_TO_IN_STR(&lv), cprs_val->cprs_str,
                LREG_VALUE_STRING_MAX);

        rc = lreg_node_exec_lrn_cb(LREG_NODE_CB_OP_WRITE_VAL, lrn, &lv);

        DBG_LREG_NODE((rc ? LL_NOTIFY : LL_DEBUG), lrn,
                      "OP_WRITE_VAL lreg_node_exec_lrn_cb(`%s:%s'): %s",
                      cprs_key->cprs_str, cprs_val->cprs_str, strerror(-rc));
    }

    return rc;
}

static bool
ctlic_scan_registry_cb(struct lreg_node *lrn, void *arg, const int depth);

static bool
ctlic_scan_registry_cb_CT_ID_WHERE(struct lreg_node *lrn,
                                   struct ctlic_iterator *parent_citer,
                                   const unsigned int depth);

static int
ctlic_try_apply_to_value(struct lreg_node *lrn,
                         const struct ctlic_request *cr,
                         const struct lreg_value *lv)
{
    if (!lrn || !cr || !lv)
        return -EINVAL;

    if (LREG_VALUE_TO_REQ_TYPE(lv) == LREG_VAL_TYPE_VARRAY ||
        LREG_VALUE_TO_REQ_TYPE(lv) == LREG_VAL_TYPE_VOBJECT)
    {
        struct lreg_node vnode = *lrn;

        lreg_value_vnode_data_to_lreg_node(lv, &vnode);

        return ctlic_try_apply_here(&vnode, cr);
    }

    return ctlic_try_apply_here(lrn, cr);
}

static bool
ctlic_where_descend(struct lreg_node *lrn, struct ctlic_iterator *parent_citer,
                    const struct lreg_value *lv, const unsigned int depth)
{
    if (!lrn || !parent_citer || !lv)
        return false;

    switch (LREG_VALUE_TO_REQ_TYPE(lv))
    {
    case LREG_VAL_TYPE_ARRAY:
    case LREG_VAL_TYPE_OBJECT:
    case LREG_VAL_TYPE_ANON_OBJECT:
        lreg_node_walk(lrn, ctlic_scan_registry_cb, parent_citer, depth,
                       LREG_VALUE_TO_USER_TYPE(lv));
        break;
    case LREG_VAL_TYPE_VARRAY:
    case LREG_VAL_TYPE_VOBJECT:
    {
        struct lreg_node vnode = *lrn;

        lreg_value_vnode_data_to_lreg_node(lv, &vnode);

        lreg_node_walk(&vnode, ctlic_scan_registry_cb, parent_citer, depth,
                       LREG_VALUE_TO_USER_TYPE(lv));
        break;
    }
    default:
        return ctlic_scan_registry_cb_CT_ID_WHERE(lrn, parent_citer, depth);
    }

    return true;
}

static bool
ctlic_scan_registry_cb_CT_ID_WHERE(struct lreg_node *lrn,
                                   struct ctlic_iterator *parent_citer,
                                   const unsigned int depth)
{
    NIOVA_ASSERT(parent_citer && parent_citer->citer_cr && depth > 0);

    struct ctlic_request *cr = parent_citer->citer_cr;
    struct ctlic_matched_token *cmt = ctlic_get_current_matched_token(cr);

    /* Do not exceed the depth specified in the GET request.
     */
    if (depth - 1 >= cmt->cmt_num_depth_segments)
        return false;

    struct lreg_value lv = {0};

    int rc = lreg_node_exec_lrn_cb(LREG_NODE_CB_OP_GET_NODE_INFO, lrn, &lv);
    if (rc)
        return rc;

    const unsigned int nkeys = lv.get.lrv_num_keys_out;
    for (unsigned int i = 0; i < nkeys; i++)
    {
        lv.lrv_value_idx_in = i;
        rc = lreg_node_exec_lrn_cb(LREG_NODE_CB_OP_READ_VAL, lrn, &lv);
        if (rc)
            return rc;

        struct ctlic_depth_segment *cds = &cmt->cmt_depth_segments[depth - 1];
        bool match = false;

        rc = ctlic_regex_test_depth_segment(lrn, cds, &lv, depth, &match);

        LOG_MSG(
            LL_DEBUG,
            "regex_rc=%d match=%d key=%s cds[%d]=%s:%s num_depth_segments=%zu",
            rc, match, LREG_VALUE_TO_KEY_STR(&lv), depth,
            cds->cds_cprs[0].cprs_str, cds->cds_cprs[1].cprs_str,
            cmt->cmt_num_depth_segments);

        if (!rc)
        {
            if (!match)
                continue;

            /* Segment match, descend if max request depth wasn't reached.
             * Scalar matches act as selectors for the current object, so the
             * next depth is evaluated against the same lrn.
             */
            else if (cmt->cmt_num_depth_segments > depth)
            {
                if (!ctlic_where_descend(lrn, parent_citer, &lv, depth + 1))
                    return false;
            }

            // Segment match and depth has been reached
            else if (cmt->cmt_num_depth_segments == depth)
                rc = ctlic_try_apply_to_value(lrn, cr, &lv);
        }

        if (rc)
        {
            // Update the printed count only if an error string was emitted.
            if (!ctlic_insert_error_kv_into_outfile(parent_citer, depth, rc))
                parent_citer->citer_sibling_printed_cnt++;

            return false;
        }
    }

    return true;
}

static bool
ctlic_scan_registry_cb_CT_ID_GET(struct lreg_node *lrn,
                                 struct ctlic_iterator *parent_citer,
                                 const unsigned int depth)
{
    NIOVA_ASSERT(parent_citer && parent_citer->citer_cr && depth > 0);

    struct ctlic_request *cr = parent_citer->citer_cr;
    struct ctlic_matched_token *cmt = ctlic_get_current_matched_token(cr);

    /* Do not exceed the depth specified in the GET request.
     */
    if (depth - 1 >= cmt->cmt_num_depth_segments)
        return false;

    bool parent_is_anon_object = false;
    struct ctlic_iterator anon_object_citer = {
        .citer_cr = cr,
        .citer_starting_byte_cnt = cr->cr_output_byte_cnt,
        .citer_tab_depth = parent_citer->citer_tab_depth + 1,
        .citer_sibling_num = parent_citer->citer_sibling_num,
        .citer_open_stanza = true,
        .citer_sibling_printed_cnt = 0,
        .citer_rand_id = random_get(),
        .citer_parent = parent_citer,
    };

    DBG_CITER(LL_DEBUG, &anon_object_citer, "");

    /* Subtract '1' from depth since depth '0' is the root ('/')
     */
    struct ctlic_depth_segment *cds = &cmt->cmt_depth_segments[depth - 1];

    struct lreg_value *lv = &anon_object_citer.citer_lv;

    int rc = lreg_node_exec_lrn_cb(LREG_NODE_CB_OP_GET_NODE_INFO, lrn, lv);
    if (rc)
        return false;

    if (LREG_VALUE_TO_REQ_TYPE(&parent_citer->citer_lv) ==
        LREG_VAL_TYPE_ARRAY ||
        LREG_VALUE_TO_REQ_TYPE(&parent_citer->citer_lv) ==
        LREG_VAL_TYPE_VARRAY)
    {
        /* If the parent is an array then "I" must be an anonymous
         * object (otherwise lreg_node_walk() would not have been called.
         * NOTE: this likely means that 'arrays of arrays' are not
         *       supported yet.
         */
        LREG_VALUE_TO_REQ_TYPE(lv) = LREG_VAL_TYPE_ANON_OBJECT;
        ctlic_scan_registry_cb_output_writer(&anon_object_citer);
        parent_is_anon_object = true;
    }

    const unsigned int nkeys = lv->get.lrv_num_keys_out;
    size_t sibling_print_cnt = 0;


    for (unsigned int i = 0; i < nkeys; i++)
    {
        const struct ctlic_iterator *my_parent = parent_is_anon_object ?
            &anon_object_citer : parent_citer;

        struct ctlic_iterator kv_citer = {
            .citer_cr = cr,
            .citer_starting_byte_cnt = cr->cr_output_byte_cnt,
            .citer_tab_depth = my_parent->citer_tab_depth + 1,
            .citer_sibling_num = i,
            .citer_open_stanza = true,
            .citer_lv = {.lrv_value_idx_in = i},
            .citer_sibling_printed_cnt = sibling_print_cnt,
            .citer_rand_id = random_get(),
            .citer_parent = my_parent,
        };

        DBG_CITER(LL_DEBUG, &kv_citer, "kv-citer");

        struct lreg_value *kv_lv = &kv_citer.citer_lv;

        rc = lreg_node_exec_lrn_cb(LREG_NODE_CB_OP_READ_VAL, lrn, kv_lv);

        if (rc) // This is an internal error which a user should not see
        {
            DBG_LREG_NODE(LL_DEBUG, lrn, "rc=%d", rc);
            return false;
        }

        bool should_print = false;
        rc = ctlic_regex_test_depth_segment(lrn, cds, kv_lv, depth,
                                            &should_print);

        if (rc) // Attempt to print an error to the user
        {
            DBG_LREG_NODE(LL_DEBUG, lrn,
                          "ctlic_regex_test_depth_segment(): %s",
                          strerror(-rc));

            if (!ctlic_insert_error_kv_into_outfile(&kv_citer, depth, rc))
                sibling_print_cnt++;
        }
        else
        {
            if (!should_print) // successful but unmatched regex
                continue;

            sibling_print_cnt++;
            ctlic_scan_registry_cb_output_writer(&kv_citer);

            if (cmt->cmt_num_depth_segments > depth &&
                (LREG_VALUE_TO_REQ_TYPE(kv_lv) == LREG_VAL_TYPE_OBJECT ||
                 LREG_VALUE_TO_REQ_TYPE(kv_lv) == LREG_VAL_TYPE_ANON_OBJECT ||
                 LREG_VALUE_TO_REQ_TYPE(kv_lv) == LREG_VAL_TYPE_ARRAY ||
                 LREG_VALUE_TO_REQ_TYPE(kv_lv) == LREG_VAL_TYPE_VOBJECT ||
                 LREG_VALUE_TO_REQ_TYPE(kv_lv) == LREG_VAL_TYPE_VARRAY))
            {
                struct ctlic_iterator sub_obj_kv_citer = kv_citer;
                sub_obj_kv_citer.citer_sibling_num = 0;
                sub_obj_kv_citer.citer_sibling_printed_cnt = 0;
                sub_obj_kv_citer.citer_rand_id = random_get();
                sub_obj_kv_citer.citer_parent = &kv_citer;

                DBG_CITER(LL_DEBUG, &sub_obj_kv_citer, "sub-obj");

                if (LREG_VALUE_TO_REQ_TYPE(kv_lv) != LREG_VAL_TYPE_VARRAY &&
                    LREG_VALUE_TO_REQ_TYPE(kv_lv) != LREG_VAL_TYPE_VOBJECT)
                {
                    lreg_node_walk(lrn, ctlic_scan_registry_cb,
                                   (void *)&sub_obj_kv_citer,
                                   depth + 1, LREG_VALUE_TO_USER_TYPE(kv_lv));
                }
                else
                {
                    struct lreg_node varray_child = *lrn;

                    lreg_value_vnode_data_to_lreg_node(kv_lv, &varray_child);

                    lreg_node_walk(&varray_child, ctlic_scan_registry_cb,
                                   (void *)&sub_obj_kv_citer,
                                   depth + 1, LREG_VALUE_TO_USER_TYPE(kv_lv));
                }
            }
        }

        ctlic_scan_registry_cb_output_writer(&kv_citer);

        if (rc)
            break;
    }

    if (parent_is_anon_object)
    {
        // Close the anon object stanza
        LREG_VALUE_TO_REQ_TYPE(lv) = LREG_VAL_TYPE_ANON_OBJECT;
        ctlic_scan_registry_cb_output_writer(&anon_object_citer);
    }

    parent_citer->citer_sibling_printed_cnt += sibling_print_cnt;

    return rc ? false : true;
}

static bool // return 'false' to terminate scan
ctlic_scan_registry_cb(struct lreg_node *lrn, void *arg, const int depth)
{
    if (!lrn)
        return false;

    struct ctlic_iterator *parent_citer = arg;

    NIOVA_ASSERT(parent_citer && parent_citer->citer_cr && depth >= 0);

    const struct ctlic_matched_token *cmt =
        ctlic_get_current_matched_token(parent_citer->citer_cr);

    DBG_CITER(LL_DEBUG, parent_citer,
              "parent-citer cmt_num_depth_segments=%zu depth=%d",
              cmt->cmt_num_depth_segments, depth);

    bool keep_going = true;

    if (cmt->cmt_token->ct_id == CT_ID_GET)
    {
        keep_going =
            ctlic_scan_registry_cb_CT_ID_GET(lrn, parent_citer, depth);

        if (keep_going)
            parent_citer->citer_sibling_num++;
    }
    else if (cmt->cmt_token->ct_id == CT_ID_WHERE)
    {
        keep_going =
            ctlic_scan_registry_cb_CT_ID_WHERE(lrn, parent_citer, depth);
    }
#if 0
    else if (cmt->cmt_token->ct_id == CT_ID_APPLY)
    {
        // Stash the 'apply' value into the cmt / cr
    }
#endif


    return keep_going;
}

static void
ctlic_scan_registry(struct ctlic_request *cr)
{
    if (!cr)
        return;

    struct lreg_node *lrn_root = lreg_root_node_get();
    if (!lrn_root)
        return;

    struct ctlic_iterator citer = {
        .citer_cr = cr,
        .citer_starting_byte_cnt = 0,
        .citer_tab_depth = 0,
        .citer_sibling_num = 0,
        .citer_sibling_printed_cnt = 0,
        .citer_open_stanza = true,
        .citer_rand_id = random_get(),
        .citer_parent = NULL,
    };

    DBG_CITER(LL_DEBUG, &citer, "start");

    int rc = lreg_node_exec_lrn_cb(LREG_NODE_CB_OP_GET_NAME, lrn_root,
                                   &citer.citer_lv);
    if (rc)
        return;

    /* This should just print the opening "{" with no prepended "`key` =" since
     * the root object is anonymous.  By bookending the token loop (just below)
     * we allow the user to place multiple GET calls into the cmd file to
     * create custom JSON outputs.
     */
    rc = ctlic_scan_registry_cb_output_writer(&citer);
    if (rc)
        return;

    for (cr->cr_current_token = 0;
         cr->cr_current_token < cr->cr_num_matched_tokens;
         cr->cr_current_token++)
    {
        const struct ctlic_matched_token *cmt =
            &cr->cr_matched_token[cr->cr_current_token];

        if (cmt->cmt_token->ct_id == CT_ID_GET ||
            cmt->cmt_token->ct_id == CT_ID_WHERE)
            lreg_node_walk(lrn_root, ctlic_scan_registry_cb,
                           (void *)&citer, 1, cr->cr_reg_user_type);
    }

    ctlic_scan_registry_cb_output_writer(&citer);
}

util_thread_ctx_ctli_int_t
ctlic_process_request(const struct ctli_cmd_handle *cch)
{
    if (!cch || !cch->ctlih_input_file_name || cch->ctlih_output_dirfd < 0)
        return -EINVAL;

    struct ctlic_request cr = {.cr_reg_user_type = cch->ctlih_reg_user_type};

    int rc = ctlic_open_and_read_input_file(cch, &cr);
    if (rc)
    {
        LOG_MSG(LL_NOTIFY, "ctlic_open_and_read_input_file(`%s'): %s",
                cch->ctlih_input_file_name, strerror(-rc));
        return rc;
    }

    LOG_MSG(LL_DEBUG, "file=%s\ncontents=\n%s",
            cch->ctlih_input_file_name,
            (const char *)cr.cr_file[CTLIC_INPUT_FILE].cf_buffer);

    rc = ctlic_parse_request(&cr);
    if (rc)
    {
        LOG_MSG(LL_NOTIFY,
                "ctlic_parse_request() %s :: file=%s\ncontents=\n%s",
                strerror(-rc), cch->ctlih_input_file_name,
                (const char *)cr.cr_file[CTLIC_INPUT_FILE].cf_buffer);
        goto done;
    }

    rc = ctlic_open_output_file(cch->ctlih_output_dirfd, &cr);
    if (rc)
    {
        LOG_MSG(LL_NOTIFY, "ctlic_open_output_file(): %s",
                strerror(-rc));
        goto done;
    }

    ctlic_scan_registry(&cr);

    rc = ctlic_rename_output_file(cch->ctlih_output_dirfd, &cr);
done:
    ctlic_request_done(&cr);

    return rc;
}
