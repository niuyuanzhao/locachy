/* The part of librime's rime_api.h that base/ime.c uses, with the same
 * names, for device-free tests without librime (test-ime-keys.c gives the
 * functions: a double of Rime's session). The real header is librime's. */
#ifndef RIME_API_DOUBLE_H
#define RIME_API_DOUBLE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define Bool int
#define False 0
#define True 1

typedef uintptr_t RimeSessionId;

#define RIME_STRUCT_INIT(Type, var) ((var).data_size = sizeof(Type) - sizeof((var).data_size))
#define RIME_STRUCT(Type, var) \
    Type var = { 0 };          \
    RIME_STRUCT_INIT(Type, var);

typedef struct rime_traits_t {
    int data_size;
    const char *shared_data_dir;
    const char *user_data_dir;
    const char *distribution_name;
    const char *distribution_code_name;
    const char *distribution_version;
    const char *app_name;
    const char **modules;
    int min_log_level;
} RimeTraits;

typedef struct {
    int length;
    int cursor_pos;
    int sel_start;
    int sel_end;
    char *preedit;
} RimeComposition;

typedef struct rime_candidate_t {
    char *text;
    char *comment;
    void *reserved;
} RimeCandidate;

typedef struct {
    int page_size;
    int page_no;
    Bool is_last_page;
    int highlighted_candidate_index;
    int num_candidates;
    RimeCandidate *candidates;
    char *select_keys;
} RimeMenu;

typedef struct rime_commit_t {
    int data_size;
    char *text;
} RimeCommit;

typedef struct rime_context_t {
    int data_size;
    RimeComposition composition;
    RimeMenu menu;
    char *commit_text_preview;
    char **select_labels;
} RimeContext;

typedef struct rime_api_t {
    int data_size;
    void (*setup)(RimeTraits *traits);
    void (*initialize)(RimeTraits *traits);
    void (*finalize)(void);
    Bool (*start_maintenance)(Bool full_check);
    void (*join_maintenance_thread)(void);
    RimeSessionId (*create_session)(void);
    Bool (*destroy_session)(RimeSessionId session_id);
    Bool (*process_key)(RimeSessionId session_id, int keycode, int mask);
    void (*clear_composition)(RimeSessionId session_id);
    Bool (*get_commit)(RimeSessionId session_id, RimeCommit *commit);
    Bool (*free_commit)(RimeCommit *commit);
    Bool (*get_context)(RimeSessionId session_id, RimeContext *context);
    Bool (*free_context)(RimeContext *ctx);
    void (*set_option)(RimeSessionId session_id, const char *option, Bool value);
    Bool (*get_option)(RimeSessionId session_id, const char *option);
    Bool (*select_schema)(RimeSessionId session_id, const char *schema_id);
    const char *(*get_input)(RimeSessionId session_id);
    Bool (*select_candidate_on_current_page)(RimeSessionId session_id, size_t index);
} RimeApi;

RimeApi *rime_get_api(void);
#endif
