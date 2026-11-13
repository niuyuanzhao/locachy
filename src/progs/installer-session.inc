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
    if (fd >= 0 && fcntl(fd, F_SETFD, 0) < 0) {
        close(fd);
        fd = -1;
    }
    if (fd >= 0 && (done != size || fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0 ||
                    lseek(fd, 0, SEEK_SET) < 0)) {
        close(fd);
        fd = -1;
    }
    /* json-c owns copies of the secrets; erase the serialized copy before
     * freeing it. The sealed fd is the only copy crossing exec. */
    if (text) explicit_bzero((void *)text, size);
    json_object_put(state);
    return fd;
}

static int restore_names(struct json_object *names, const struct named *items, unsigned char *on, int count)
{
    memset(on, 0, (size_t)count);
    for (int i = 0; i < jlen(names); i++) {
        int found = 0;
        for (int k = 0; k < count; k++) {
            if (!strcmp(jtext(names, i), items[k].name)) on[k] = found = 1;
        }
        if (!found) return -1; /* never silently drop a chosen module or app */
    }
    return 0;
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
    ssize_t got = pread(fd, text, (size_t)st.st_size, 0);
    close(fd);
    struct json_object *state = got == st.st_size ? json_tokener_parse(text) : NULL;
    explicit_bzero(text, (size_t)st.st_size);
    free(text);
    if (!state || jint(state, "format") != 1) {
        if (state) json_object_put(state);
        return -1;
    }
    int ok = 1;
#define LOAD_INT(k) c.k = (int)jint(state, #k);
    SESSION_INTS(LOAD_INT)
#undef LOAD_INT
#define LOAD_TEXT(k) do { const char *s = jstr(state, #k); if (strlen(s) >= sizeof c.k) ok = 0; \
                         else copy(c.k, sizeof c.k, s); } while (0);
    SESSION_TEXT(LOAD_TEXT)
#undef LOAD_TEXT
    ok &= restore_names(jget(state, "modules"), modules, c.module_on, nmodules) == 0;
    ok &= restore_names(jget(state, "containers"), containers, c.container_on, ncontainers) == 0;
    memset(c.repo_on, 0, sizeof c.repo_on);
    for (int i = 0; i < jlen(jget(state, "repos")); i++) {
        int found = 0;
        for (int k = 0; k < nrepos; k++) {
            if (!strcmp(repo_names[k], jtext(jget(state, "repos"), i))) c.repo_on[k] = found = 1;
        }
        ok &= found;
    }
    c.ime = c.machine = c.level = 0;
    const char *ime = jstr(state, "ime_name"), *machine = jstr(state, "machine_name"), *level = jstr(state, "level_name");
    int have_ime = !nimes && !strcmp(ime, "off"), have_machine = !machine[0], have_level = !strcmp(level, "auto");
    for (int i = 0; i < nimes; i++) if (!strcmp(imes[i].name, ime)) { c.ime = i; have_ime = 1; }
    for (int i = 0; i < nmachines; i++) if (!strcmp(machines[i].name, machine)) { c.machine = i; have_machine = 1; }
    for (int i = 0; i < nlevels; i++) if (!strcmp(levels[i], level)) { c.level = i + 1; have_level = 1; }
    ok &= have_ime && have_machine && have_level;
    recovered_installation = json_object_get_boolean(jget(state, "recovered"));
    ui_lang = (int)jint(state, "ui_lang");
    ok &= ui_lang >= 0 && ui_lang <= 1 && c.lang >= 0 && c.lang <= 1 &&
          c.disk_mode >= WHOLE && c.disk_mode <= MOUNTED && c.encrypt >= ENC_OFF && c.encrypt <= ENC_TPM;
    const char *operation = jstr(state, "action");
    ok &= !strcmp(operation, "begin") || !strcmp(operation, "continue");
    copy(action, action_size, operation);
    /* Erase json-c's secret strings too; their lengths are unchanged. */
    struct json_object *secret = jget(state, "passphrase");
    if (secret) explicit_bzero((void *)json_object_get_string(secret), (size_t)json_object_get_string_len(secret));
    secret = jget(state, "password");
    if (secret) explicit_bzero((void *)json_object_get_string(secret), (size_t)json_object_get_string_len(secret));
    json_object_put(state);
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
        else if (!strcmp(option, "--encrypt")) c.encrypt = ENC_PASS;
        else if (!strcmp(option, "--tpm")) c.encrypt = ENC_TPM;
        else if (!strcmp(option, "--language")) c.lang = !strncmp(value, "zh", 2);
        else if (!strcmp(option, "--root")) { c.disk_mode = MOUNTED; copy(c.root, sizeof c.root, value); }
        else if (!strcmp(option, "--esp")) copy(c.esp, sizeof c.esp, value);
    }
    if (!strcmp(phase, "preflight")) {
        tell(0, "%s", _("The package keys were not trusted; c will check them again before any target write."));
        recovered_installation = 0;
    } else {
        recovered_installation = 1;
    }
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
    setenv("LOCACHY_INSTALL_SNAPSHOT_ROOT", checkout, 1);
    setenv("LOCACHY_INSTALL_SNAPSHOT_COMMIT", revision, 1);
    const char *current = getenv("LOCACHY_INSTALLER_INPUT_HASH");
    if (current && !strcmp(current, hash)) {
        setenv("LOCACHY_INSTALLER_REVISION", revision, 1);
        json_object_put(result);
        return 0;
    }
    int fd = session_fd(action);
    if (fd < 0) {
        json_object_put(result);
        tell(1, "%s", _("Cannot preserve the installer choices for its update."));
        return -1;
    }
    setenv("LOCACHY_INSTALLER_INPUT_HASH", hash, 1);
    setenv("LOCACHY_INSTALLER_REVISION", revision, 1);
    char state_arg[48];
    snprintf(state_arg, sizeof state_arg, "--state-fd=%d", fd);
    char *const next[] = { (char *)frontend, state_arg, NULL };
    pointer_release();
    keys_release();
    screen_release();
    console_restore();
    execv(frontend, next);
    close(fd);
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
