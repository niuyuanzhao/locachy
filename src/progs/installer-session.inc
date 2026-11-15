/* A refreshed executable receives the choices through a sealed anonymous fd,
 * never a disk file, environment variable, command argument or log. Names,
 * not array indices, survive a module/catalog reorder between versions. */
#define SESSION_INTS(X) \
    X(lang) X(disk_mode) X(swap_gib) X(encrypt) X(boot_lock) X(ntp) X(ime) X(machine) X(level) X(rank) \
    X(copy_network) X(hibernate) X(secure_boot) X(secure_boot_microsoft) X(lang_touched) \
    X(modules_touched) X(containers_touched) X(ime_touched)
#define SESSION_TEXT(X) \
    X(disk) X(esp) X(root) X(swap) X(passphrase) X(password) X(user) X(full_name) X(hostname) \
    X(keymap) X(timezone) X(cpu) X(update_repo) X(update_branch) X(apps_repo) X(apps_branch)

static struct json_object *selected_names(const struct named *items, const unsigned char *selected, int count)
{
    struct json_object *array = json_object_new_array();
    for (int i = 0; i < count; i++) {
        if (selected[i]) json_object_array_add(array, json_object_new_string(items[i].name));
    }
    return array;
}

/* json-c owns both the parsed strings and, after serialization, its output
 * buffer. Erase both before releasing a document that may contain secrets. */
static void clear_session_json(struct json_object *value)
{
    if (!value) return;
    if (json_object_is_type(value, json_type_string)) {
        explicit_bzero((void *)json_object_get_string(value), (size_t)json_object_get_string_len(value));
    } else if (json_object_is_type(value, json_type_array)) {
        for (int i = 0; i < jlen(value); i++) clear_session_json(jat(value, i));
    } else if (json_object_is_type(value, json_type_object)) {
        json_object_object_foreach(value, key, child) {
            (void)key;
            clear_session_json(child);
        }
    }
}

static int session_fd(const char *action)
{
    struct json_object *state = json_object_new_object();
    json_object_object_add(state, "format", json_object_new_int(1));
    json_object_object_add(state, "action", json_object_new_string(action));
    json_object_object_add(state, "ui_lang", json_object_new_int(ui_lang));
    json_object_object_add(state, "recovered", json_object_new_boolean(recovered_installation));
#define SAVE_INT(k) json_object_object_add(state, #k, json_object_new_int(c.k));
    SESSION_INTS(SAVE_INT)
#undef SAVE_INT
#define SAVE_TEXT(k) json_object_object_add(state, #k, json_object_new_string(c.k));
    SESSION_TEXT(SAVE_TEXT)
#undef SAVE_TEXT
    json_object_object_add(state, "modules", selected_names(modules, c.module_on, nmodules));
    json_object_object_add(state, "containers", selected_names(containers, c.container_on, ncontainers));
    struct json_object *repos = json_object_new_array();
    for (int i = 0; i < nrepos; i++) {
        if (c.repo_on[i]) json_object_array_add(repos, json_object_new_string(repo_names[i]));
    }
    json_object_object_add(state, "repos", repos);
    json_object_object_add(state, "ime_name", json_object_new_string(nimes ? imes[c.ime].name : "off"));
    json_object_object_add(state, "machine_name", json_object_new_string(c.machine > 0 ? machines[c.machine].name : ""));
    json_object_object_add(state, "level_name", json_object_new_string(c.level > 0 ? levels[c.level - 1] : "auto"));
    const char *text = json_object_to_json_string_ext(state, JSON_C_TO_STRING_PLAIN);
    size_t size = strlen(text), done = 0;
    int fd = memfd_create("locachy-installer-session", MFD_ALLOW_SEALING | MFD_CLOEXEC);
    while (fd >= 0 && done < size) {
        ssize_t n = write(fd, text + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += (size_t)n;
    }
    /* Keep CLOEXEC until the immediate exec boundary; the next frontend
     * restores it before starting any helper. */
    if (fd >= 0 && (done != size || fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0 ||
                    lseek(fd, 0, SEEK_SET) < 0)) {
        close(fd);
        fd = -1;
    }
    /* json-c owns copies of the secrets; erase the serialized copy before
     * freeing it. The sealed fd is the only copy crossing exec. */
    if (text) explicit_bzero((void *)text, size);
    clear_session_json(state);
    json_object_put(state);
    return fd;
}

static int restore_names(struct json_object *names, const struct named *items, unsigned char *on, int count)
{
    if (!json_object_is_type(names, json_type_array)) return -1;
    memset(on, 0, (size_t)count);
    for (int i = 0; i < jlen(names); i++) {
        struct json_object *name = jat(names, i);
        if (!json_object_is_type(name, json_type_string) ||
            (size_t)json_object_get_string_len(name) != strlen(jtext(names, i))) return -1;
        int found = 0;
        for (int k = 0; k < count; k++) {
            if (!strcmp(jtext(names, i), items[k].name)) on[k] = found = 1;
        }
        if (!found) return -1; /* never silently drop a chosen module or app */
    }
    return 0;
}

static int session_string(struct json_object *state, const char *key)
{
    struct json_object *value = jget(state, key);
    return json_object_is_type(value, json_type_string) &&
           (size_t)json_object_get_string_len(value) == strlen(json_object_get_string(value));
}

static int restore_session(int fd, char *action, size_t action_size)
{
    int seals = fcntl(fd, F_GET_SEALS), required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    struct stat st;
    if (seals < 0 || (seals & required) != required || fstat(fd, &st) < 0 || st.st_size <= 0 || st.st_size > 65536) {
        close(fd);
        return -1;
    }
    char *text = calloc(1, (size_t)st.st_size + 1);
    if (!text) { close(fd); return -1; }
    ssize_t got;
    do { got = pread(fd, text, (size_t)st.st_size, 0); } while (got < 0 && errno == EINTR);
    close(fd);
    struct json_tokener *tok = json_tokener_new();
    struct json_object *state = NULL;
    if (tok && got == st.st_size) {
        json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
        state = json_tokener_parse_ex(tok, text, (int)st.st_size);
    }
    int parsed = tok && json_tokener_get_error(tok) == json_tokener_success;
    if (tok) json_tokener_free(tok);
    explicit_bzero(text, (size_t)st.st_size);
    free(text);
    int ok = parsed && json_object_is_type(state, json_type_object) &&
             json_object_is_type(jget(state, "format"), json_type_int) && jint(state, "format") == 1;
    if (!ok) {
        clear_session_json(state);
        if (state) json_object_put(state);
        return -1;
    }
    struct choices next = {0};
    /* Decode into a separate value. An incompatible catalog or malformed
     * field must never leave partial choices (especially secrets) in c. */
#define LOAD_INT(k) do { struct json_object *v = jget(state, #k); long long n = jint(state, #k); \
    if (!json_object_is_type(v, json_type_int) || n < INT_MIN || n > INT_MAX) ok = 0; \
    else next.k = (int)n; } while (0);
    SESSION_INTS(LOAD_INT)
#undef LOAD_INT
#define LOAD_TEXT(k) do { struct json_object *v = jget(state, #k); const char *s = jstr(state, #k); \
    if (!json_object_is_type(v, json_type_string) || (size_t)json_object_get_string_len(v) != strlen(s) || \
        strlen(s) >= sizeof next.k) ok = 0; else copy(next.k, sizeof next.k, s); } while (0);
    SESSION_TEXT(LOAD_TEXT)
#undef LOAD_TEXT
    ok &= restore_names(jget(state, "modules"), modules, next.module_on, nmodules) == 0;
    ok &= restore_names(jget(state, "containers"), containers, next.container_on, ncontainers) == 0;
    struct json_object *repos = jget(state, "repos");
    ok &= json_object_is_type(repos, json_type_array);
    for (int i = 0; i < jlen(repos); i++) {
        int found = 0;
        for (int k = 0; k < nrepos; k++) {
            if (json_object_is_type(jat(repos, i), json_type_string) && !strcmp(repo_names[k], jtext(repos, i))) {
                next.repo_on[k] = found = 1;
            }
        }
        ok &= found;
    }
    next.ime = next.machine = next.level = 0;
    const char *ime = jstr(state, "ime_name"), *machine = jstr(state, "machine_name"), *level = jstr(state, "level_name");
    int have_ime = !nimes && !strcmp(ime, "off"), have_machine = !machine[0], have_level = !strcmp(level, "auto");
    for (int i = 0; i < nimes; i++) if (!strcmp(imes[i].name, ime)) { next.ime = i; have_ime = 1; }
    for (int i = 0; i < nmachines; i++) if (!strcmp(machines[i].name, machine)) { next.machine = i; have_machine = 1; }
    for (int i = 0; i < nlevels; i++) if (!strcmp(levels[i], level)) { next.level = i + 1; have_level = 1; }
    ok &= have_ime && have_machine && have_level && session_string(state, "ime_name") &&
          session_string(state, "machine_name") && session_string(state, "level_name");
    int next_recovered = json_object_is_type(jget(state, "recovered"), json_type_boolean) ?
                         json_object_get_boolean(jget(state, "recovered")) : 0;
    long long next_lang = jint(state, "ui_lang");
    ok &= json_object_is_type(jget(state, "recovered"), json_type_boolean) &&
          json_object_is_type(jget(state, "ui_lang"), json_type_int) && next_lang >= 0 && next_lang <= 1 &&
          next.lang >= 0 && next.lang <= 1 && next.swap_gib >= 0 && next.swap_gib <= 1024 && next.hibernate >= -1 &&
          next.disk_mode >= WHOLE && next.disk_mode <= MOUNTED && next.encrypt >= ENC_OFF && next.encrypt <= ENC_TPM;
    const char *operation = jstr(state, "action");
    ok &= session_string(state, "action") && (!strcmp(operation, "begin") || !strcmp(operation, "continue")) && strlen(operation) < action_size;
    if (ok) {
        c = next;
        recovered_installation = next_recovered;
        ui_lang = (int)next_lang;
        copy(action, action_size, operation);
    }
    explicit_bzero(&next, sizeof next);
    clear_session_json(state);
    if (state) json_object_put(state);
    return ok ? 0 : -1;
}

/* Restore only named choices, never disk operations, from the backend's
 * validated record. Resume itself consumes that record, not these UI values. */
static int recover_installation(void)
{
    if (!stopped_installation) return 0;
    struct json_object *argv = jget(stopped_installation, "argv");
    if (!argv || !jlen(argv)) return -1;
    const char *phase = jstr(stopped_installation, "phase");
    if (!strcmp(phase, "partitioning")) return -1;
    c.encrypt = ENC_OFF;
    explicit_bzero(c.password, sizeof c.password);
    explicit_bzero(c.passphrase, sizeof c.passphrase);
    for (int i = 0; i < jlen(argv); i++) {
        const char *arg = jtext(argv, i), *value = strchr(arg, '=');
        char option[80];
        size_t length = value ? (size_t)(value - arg) : strlen(arg);
        if (length >= sizeof option) return -1;
        memcpy(option, arg, length);
        option[length] = '\0';
        if (value) value++;
        else if (i + 1 < jlen(argv) && strncmp(jtext(argv, i + 1), "--", 2)) value = jtext(argv, ++i);
        else value = "";
#define RECOVER_TEXT(k, flag) if (!strcmp(option, flag)) { if (strlen(value) >= sizeof c.k) return -1; copy(c.k, sizeof c.k, value); }
        RECOVER_TEXT(user, "--user")
        else RECOVER_TEXT(full_name, "--full-name")
        else RECOVER_TEXT(hostname, "--hostname")
        else RECOVER_TEXT(keymap, "--keymap")
        else RECOVER_TEXT(timezone, "--timezone")
        else RECOVER_TEXT(cpu, "--cpu")
        else RECOVER_TEXT(update_repo, "--update-repo")
        else RECOVER_TEXT(update_branch, "--update-branch")
        else RECOVER_TEXT(apps_repo, "--apps-repo")
        else RECOVER_TEXT(apps_branch, "--apps-branch")
#undef RECOVER_TEXT
        else if (!strcmp(option, "--encrypt") && c.encrypt != ENC_TPM) c.encrypt = ENC_PASS;
        else if (!strcmp(option, "--tpm")) c.encrypt = ENC_TPM;
        else if (!strcmp(option, "--language")) c.lang = !strncmp(value, "zh", 2);
        else if (!strcmp(option, "--root")) { c.disk_mode = MOUNTED; copy(c.root, sizeof c.root, value); }
        else if (!strcmp(option, "--esp")) copy(c.esp, sizeof c.esp, value);
    }
    if (!strcmp(phase, "preflight")) {
        tell(0, "%s", _("The package keys were not trusted; c will check them again before any target write."));
    }
    /* Every recovered record omits secrets, including a stop at preflight. */
    recovered_installation = 1;
    resumable = resumed_run = 1;
    mode = M_FAILED;
    started = ended = now();
    log_read = 0;
    log_part[0] = '\0';
    read_progress(); /* reconstruct the key list and copyable command too */
    copy(run_source, sizeof run_source, jstr(jget(stopped_installation, "source"), "commit"));
    return 1;
}

/* Refresh first. The next call to the backend is pinned to this snapshot,
 * so a second clone cannot change the code after its preflight succeeded. */
static int refresh_installer(const char *action)
{
    char output[4096], repo[300], branch[180], apps_repo[300], apps_branch[180], errors[512];
    mkdir(RUN_DIR, 0700);
    snprintf(errors, sizeof errors, "%s/refresh.log", RUN_DIR);
    snprintf(repo, sizeof repo, "--update-repo=%s", c.update_repo);
    snprintf(branch, sizeof branch, "--update-branch=%s", c.update_branch);
    snprintf(apps_repo, sizeof apps_repo, "--apps-repo=%s", c.apps_repo);
    snprintf(apps_branch, sizeof apps_branch, "--apps-branch=%s", c.apps_branch);
    char *const argv[] = { INSTALL_CMD, "--refresh-live", repo, branch, apps_repo, apps_branch, NULL };
    screen_title(_("Updating the source and the installer..."));
    screen_flush();
    capture_error_log = errors;
    int status = capture(argv, output, sizeof output, 0);
    capture_error_log = NULL;
    struct json_object *result = status == 0 ? json_tokener_parse(output) : NULL;
    const char *frontend = jstr(result, "frontend"), *hash = jstr(result, "input_hash"), *revision = jstr(result, "revision");
    if (status != 0) {
        tell(1, _("The source or installer could not be updated. The terminal shows why."));
        if (result) json_object_put(result);
        return -1;
    }
    if (!result || !frontend[0] || !hash[0]) {
        if (result) json_object_put(result);
        tell(1, "%s", _("The source or installer could not be updated. Nothing was continued; the terminal shows why."));
        return -1;
    }
    const char *source = jstr(result, "source");
    char checkout[4096];
    copy(checkout, sizeof checkout, source);
    char *slash = strrchr(checkout, '/');
    if (slash && !strcmp(slash, "/src")) *slash = '\0';
    if (!strcmp(action, "continue") && stopped_installation) {
        const char *saved = jstr(stopped_installation, "source");
        if (saved[0] && revision[0] && strcmp(saved, revision) != 0) {
            /* A stopped run's target stays the authority for resume. The
             * refreshed binary may continue it only after the person sees
             * that the source revision changed. */
            tell(0, _("The installer was updated; the stopped target will be checked again before continuing."));
        }
    }
    const char *current = getenv("LOCACHY_INSTALLER_INPUT_HASH");
    if (current && !strcmp(current, hash)) {
        if (setenv("LOCACHY_INSTALL_SNAPSHOT_ROOT", checkout, 1) < 0 ||
            setenv("LOCACHY_INSTALL_SNAPSHOT_COMMIT", revision, 1) < 0 ||
            setenv("LOCACHY_INSTALLER_REVISION", revision, 1) < 0) {
            json_object_put(result);
            tell(1, "%s", _("Cannot preserve the installer choices for its update."));
            return -1;
        }
        json_object_put(result);
        return 0;
    }
    int fd = session_fd(action);
    if (fd < 0) {
        json_object_put(result);
        tell(1, "%s", _("Cannot preserve the installer choices for its update."));
        return -1;
    }
    /* Only exec's environment may claim the new executable's input hash.
     * On failure this process must still identify its original binary. */
    const char *names[] = { "LOCACHY_INSTALL_SNAPSHOT_ROOT=", "LOCACHY_INSTALL_SNAPSHOT_COMMIT=",
                            "LOCACHY_INSTALLER_INPUT_HASH=", "LOCACHY_INSTALLER_REVISION=" };
    const char *values[] = { checkout, revision, hash, revision };
    char *updates[4] = {0};
    size_t count = 0, used = 0;
    while (environ[count]) count++;
    char **environment = calloc(count + 5, sizeof *environment);
    int ready = environment != NULL;
    for (size_t i = 0; i < 4 && ready; i++) {
        if (asprintf(&updates[i], "%s%s", names[i], values[i]) < 0) ready = 0;
    }
    if (ready) {
        for (size_t i = 0; i < count; i++) {
            int replace = 0;
            for (size_t k = 0; k < 4; k++) replace |= !strncmp(environ[i], names[k], strlen(names[k]));
            if (!replace) environment[used++] = environ[i];
        }
        for (size_t i = 0; i < 4; i++) environment[used++] = updates[i];
    }
    if (!ready || fcntl(fd, F_SETFD, 0) < 0) {
        for (size_t i = 0; i < 4; i++) free(updates[i]);
        free(environment);
        close(fd);
        json_object_put(result);
        tell(1, "%s", _("Cannot preserve the installer choices for its update."));
        return -1;
    }
    char state_arg[48];
    snprintf(state_arg, sizeof state_arg, "--state-fd=%d", fd);
    char *const next[] = { (char *)frontend, state_arg, NULL };
    pointer_release();
    keys_release();
    screen_release();
    console_restore();
    execve(frontend, next, environment);
    close(fd);
    for (size_t i = 0; i < 4; i++) free(updates[i]);
    free(environment);
    json_object_put(result);
    console_take();
    screen_open(BASE_FONT);
    keys_open();
    take_pointer();
    tell(1, "%s", _("The updated installer could not start. The choices are kept; try again."));
    return -1;
}

#undef SESSION_INTS
#undef SESSION_TEXT
