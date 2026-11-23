/* Appearance: the two choices edit only their own rc.conf key. */
static const char *const scheme_names[] = { N_("LOCachy Dark"), N_("LOCachy Light") };
static const char *const scheme_values[] = { "dark", "light" };
static const struct {
    const char *name;
    uint32_t rgb;
} accent_presets[] = {
    { N_("Blue"),   0x3478D4 },
    { N_("Teal"),   0x008577 },
    { N_("Green"),  0x438A46 },
    { N_("Amber"),  0xD98519 },
    { N_("Violet"), 0x8C60C7 },
    { N_("Rose"),   0xCB557A },
};
#define CUSTOM_ACCENT ((int)(sizeof accent_presets / sizeof *accent_presets) + 1)
#define THEME_LABEL_CELLS 32

static const char *accent_name(int choice)
{
    if (choice == 0) return _("Scheme default");
    if (choice == CUSTOM_ACCENT) return _("Custom");
    return _(accent_presets[choice - 1].name);
}

static int accent_choice(const struct theme_config *config)
{
    if (!config->custom_accent) return 0;
    for (int i = 1; i < CUSTOM_ACCENT; i++) {
        if (accent_presets[i - 1].rgb == config->accent) return i;
    }
    return CUSTOM_ACCENT;
}

static int theme_row(int idx)
{
    return idx == scheme_index() || idx == accent_index();
}

static void theme_row_text(int idx, char *line, size_t size)
{
    const struct theme_config fallback = { .scheme = THEME_DARK, .accent = ACCENT };
    const struct theme_config *config = theme_get ? &theme_get()->config : &fallback;
    const char *name = idx == scheme_index() ? _("Color scheme") : _("Accent color");
    char label[96];
    if (idx == scheme_index()) {
        snprintf(line, size, "%s     %s", ui_pad(label, sizeof label, name, THEME_LABEL_CELLS),
                 _(scheme_names[config->scheme]));
    } else {
        snprintf(line, size, "%s     %s #%06X", ui_pad(label, sizeof label, name, THEME_LABEL_CELLS),
                 accent_name(accent_choice(config)), (unsigned)config->accent);
    }
}

/* The outline stays readable even if the swatch equals its surroundings. */
static void theme_swatch(int x, int row, uint32_t first, uint32_t second, uint32_t fg, uint32_t bg)
{
    if (x + 4 > screen_cols()) return;
    screen_text(x, row, "[", fg, bg);
    screen_fill(x + 1, row, 1, 1, first);
    screen_fill(x + 2, row, 1, 1, second);
    screen_text(x + 3, row, "]", fg, bg);
}

static int theme_apply(int notify_desktop)
{
    int status = theme_apply_desktop();
    if (status == 0 && notify_desktop) signal_programs("labwc", SIGHUP);
    if (theme_publish_appearance() < 0) status = -1;
    return status;
}

static void save_theme_value(const char *key, const char *value)
{
    if (rc_set(key, value) < 0) {
        snprintf(notice, sizeof notice, _("Cannot write %s."), RC_CONF);
        return;
    }
    int changed = theme_reload();
    full_redraw = 1;
    int status = theme_apply(1);
    if (changed > 0) signal_programs("locachy-bar", SIGHUP);
    snprintf(notice, sizeof notice, "%s", status < 0 ?
             _("Colors saved, but not all desktop settings could be applied.") :
             _("Colors saved and applied. Reopen foot windows for the new colors."));
}

static void choose_colors(int idx)
{
    int scheme = idx == scheme_index();
    const struct theme_config config = theme_get()->config;
    struct theme_palette schemes[2];
    for (int i = 0; i < 2; i++) {
        const struct theme_config sample = { .scheme = i };
        theme_resolve(&sample, &schemes[i]);
    }
    uint32_t default_accent = schemes[config.scheme].config.accent;
    int n = scheme ? 2 : CUSTOM_ACCENT + 1;
    int sel = scheme ? (int)config.scheme : accent_choice(&config), top = 0;
    for (;;) {
        int cols = screen_cols(), rows = screen_rows();
        int visible = rows > 6 ? rows - 6 : 1;
        if (visible > n) visible = n;
        if (top > n - visible) top = n - visible;
        if (sel < top) top = sel;
        if (sel >= top + visible) top = sel - visible + 1;
        ui_hits_clear();
        screen_fill(0, 0, cols, rows, BG);
        screen_bar(scheme ? _("Color scheme") : _("Accent color"), NULL);
        /* TRANSLATORS: Up/Down, Enter and Esc are key names; keep them. */
        screen_text(2, 2, _("Up/Down: choose   Enter: select   Esc: back"), ACCENT_TEXT, BG);
        for (int r = 0; r < visible; r++) {
            int i = top + r, row = 3 + r;
            uint32_t bg = i == sel ? ACCENT : BG, fg = i == sel ? ON_ACCENT : FG;
            uint32_t first, second;
            char label[128];
            if (scheme) {
                first = schemes[i].rgb[THEME_BG];
                second = schemes[i].rgb[THEME_FG];
                snprintf(label, sizeof label, "%s", _(scheme_names[i]));
            } else {
                first = second = i == 0 ? default_accent : i == CUSTOM_ACCENT ? config.accent : accent_presets[i - 1].rgb;
                if (i == CUSTOM_ACCENT) snprintf(label, sizeof label, "%s (#RRGGBB)", accent_name(i));
                else snprintf(label, sizeof label, "%s #%06X", accent_name(i), (unsigned)first);
            }
            screen_fill(0, row, cols, 1, bg);
            theme_swatch(2, row, first, second, fg, bg);
            screen_text(7, row, label, fg, bg);
            ui_hit(0, row, cols, 1, i);
        }
        settings_back_button(rows - 2);
        screen_flush();
        int key = key_get(), mods = mods_now(), act = key_list_action(key, mods);
        if (close_read(key) || act == ACT_CANCEL) return;
        if (key == K_POINTER) {
            const struct pointer_event *e = pointer_last();
            if (e->wheel) {
                sel -= e->wheel;
                if (sel < 0) sel = 0;
                if (sel >= n) sel = n - 1;
            } else if (e->button == BTN_LEFT && e->pressed) {
                int hit = ui_hit_at(e->col, e->row);
                if (hit == SETTINGS_HIT_BACK) return;
                if (hit >= 0 && hit < n) { sel = hit; break; }
            }
        } else if (act == ACT_NONE) {
            if (key == '\n') break;
            if (key == K_UP) sel = (sel + n - 1) % n;
            if (key == K_DOWN) sel = (sel + 1) % n;
            if (key == K_BOL) sel = 0;
            if (key == K_EOL) sel = n - 1;
        }
    }
    if (scheme) {
        save_theme_value("color_scheme", scheme_values[sel]);
    } else if (sel == 0) {
        save_theme_value("accent_color", "default");
    } else {
        uint32_t rgb;
        char rgb_text[64];
        if (sel == CUSTOM_ACCENT) {
            snprintf(rgb_text, sizeof rgb_text, "#%06X", (unsigned)config.accent);
            if (!prompt(_("Custom accent (#RRGGBB): "), rgb_text, sizeof rgb_text)) return;
            if (theme_parse_accent(rgb_text, &rgb) < 0) {
                snprintf(notice, sizeof notice, "%s", _("Enter a color as #RRGGBB, for example #3478D4. Nothing was changed."));
                return;
            }
        } else {
            rgb = accent_presets[sel - 1].rgb;
        }
        snprintf(rgb_text, sizeof rgb_text, "#%06X", (unsigned)rgb);
        save_theme_value("accent_color", rgb_text);
    }
}
