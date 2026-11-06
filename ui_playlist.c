#include "ui_playlist.h"
#include "audio_core.h"
#include "audio_memory.h"
#include "media_io.h"
#include "ui_globals.h"
#include "ui_transport.h"

static GListStore *track_list_store = NULL;
static GtkMultiSelection *track_selection_model = NULL;
static GtkWidget *track_list_view;
static GtkWidget *btn_rename;
static GtkWidget *btn_delete;
static GtkWidget *preset_btns[22];
static GtkWidget *lbl_playlist_title = NULL;
static int current_preset_slot = -1;
G_DEFINE_TYPE(QJTrack, qj_track, G_TYPE_OBJECT)

static gboolean is_supported_media_file(const char *path);

static void qj_track_finalize(GObject *object) {
    QJTrack *self = QJ_TRACK(object);
    g_free(self->filepath);
    g_free(self->display_name);
    G_OBJECT_CLASS(qj_track_parent_class)->finalize(object);
}

static void qj_track_class_init(QJTrackClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = qj_track_finalize;
}

static void qj_track_init(QJTrack *self) {
    self->filepath = NULL;
    self->display_name = NULL;
    self->is_qjams_native = false;
}

QJTrack *qj_track_new(const char *filepath, const char *display_name, bool is_native) {
    QJTrack *self = g_object_new(QJ_TYPE_TRACK, NULL);
    self->filepath = g_strdup(filepath);
    self->display_name = g_strdup(display_name);
    self->is_qjams_native = is_native;
    return self;
}

static void update_playlist_title_ui(const char *filepath);

static void update_preset_buttons(void) {
    for (int i = 0; i < 22; i++) {
        if (!preset_btns[i])
            continue;

        gtk_widget_remove_css_class(preset_btns[i], "active-preset");
        if (i == current_preset_slot) {
            gtk_widget_add_css_class(preset_btns[i], "active-preset");
        }

        if (strlen(ui_state.config.playlist_slots[i]) > 0) {
            char *base = g_path_get_basename(ui_state.config.playlist_slots[i]);
            char letter[2] = {'?', '\0'};
            for (int j = 0; base[j] != '\0'; j++) {
                if (g_ascii_isalpha(base[j])) {
                    letter[0] = g_ascii_toupper(base[j]);
                    break;
                }
            }
            gtk_button_set_label(GTK_BUTTON(preset_btns[i]), letter);
            gtk_widget_set_tooltip_text(preset_btns[i], ui_state.config.playlist_slots[i]);
            g_free(base);
        } else {
            gtk_button_set_label(GTK_BUTTON(preset_btns[i]), "-");
            gtk_widget_set_tooltip_text(preset_btns[i], "Empty Slot");
        }
    }
}

void activate_playlist_preset(int slot) {
    if (slot < 0 || slot >= 22)
        return;
    gtk_widget_grab_focus(preset_btns[slot]);
    current_preset_slot = slot;

    if (strlen(ui_state.config.playlist_slots[slot]) > 0) {
        strncpy(ui_state.playlist_path, ui_state.config.playlist_slots[slot],
                sizeof(ui_state.playlist_path) - 1);
        ui_state.playlist_path[sizeof(ui_state.playlist_path) - 1] = '\0';

        strncpy(ui_state.config.last_playlist_path, ui_state.config.playlist_slots[slot],
                sizeof(ui_state.config.last_playlist_path) - 1);
        ui_state.config.last_playlist_path[sizeof(ui_state.config.last_playlist_path) - 1] = '\0';

        save_qjams_config(ui_state.config_path, &ui_state.config);
        load_playlist_from_file(ui_state.playlist_path);
    } else {
        ui_state.playlist_path[0] = '\0';
        g_list_store_remove_all(track_list_store);
        update_playlist_title_ui("");
        update_preset_buttons();

        log_and_notify(1, "Status: Empty Preset Selected");
    }
}
// ...

static GdkContentProvider *on_preset_drag_prepare(GtkDragSource *source, double x, double y,
                                                  gpointer user_data) {
    (void)source;
    (void)x;
    (void)y;
    int slot = GPOINTER_TO_INT(user_data);
    GValue value = G_VALUE_INIT;
    g_value_init(&value, G_TYPE_INT);
    g_value_set_int(&value, slot);
    return gdk_content_provider_new_for_value(&value);
}

static gboolean on_preset_drop(GtkDropTarget *target, const GValue *value, double x, double y,
                               gpointer user_data) {
    (void)target;
    (void)x;
    (void)y;
    if (!G_VALUE_HOLDS(value, G_TYPE_INT))
        return FALSE;

    int src_slot = g_value_get_int(value);
    int dest_slot = GPOINTER_TO_INT(user_data);

    if (src_slot == dest_slot || src_slot < 0 || src_slot >= 22 || dest_slot < 0 ||
        dest_slot >= 22) {
        return FALSE;
    }

    // Safely cache the source slot path string
    char temp_slot[1024];
    strncpy(temp_slot, ui_state.config.playlist_slots[src_slot], sizeof(temp_slot) - 1);
    temp_slot[sizeof(temp_slot) - 1] = '\0';

    // Shift elements left or right securely mimicking memmove logic
    if (src_slot < dest_slot) {
        for (int i = src_slot; i < dest_slot; i++) {
            strncpy(ui_state.config.playlist_slots[i], ui_state.config.playlist_slots[i + 1],
                    sizeof(ui_state.config.playlist_slots[0]) - 1);
            ui_state.config.playlist_slots[i][sizeof(ui_state.config.playlist_slots[0]) - 1] = '\0';
        }
    } else {
        for (int i = src_slot; i > dest_slot; i--) {
            strncpy(ui_state.config.playlist_slots[i], ui_state.config.playlist_slots[i - 1],
                    sizeof(ui_state.config.playlist_slots[0]) - 1);
            ui_state.config.playlist_slots[i][sizeof(ui_state.config.playlist_slots[0]) - 1] = '\0';
        }
    }

    // Deposit the cached string into its new home
    strncpy(ui_state.config.playlist_slots[dest_slot], temp_slot,
            sizeof(ui_state.config.playlist_slots[0]) - 1);
    ui_state.config.playlist_slots[dest_slot][sizeof(ui_state.config.playlist_slots[0]) - 1] = '\0';

    // Maintain focus tracking
    if (current_preset_slot == src_slot) {
        current_preset_slot = dest_slot;
    } else if (src_slot < dest_slot && current_preset_slot > src_slot &&
               current_preset_slot <= dest_slot) {
        current_preset_slot--;
    } else if (src_slot > dest_slot && current_preset_slot >= dest_slot &&
               current_preset_slot < src_slot) {
        current_preset_slot++;
    }

    save_qjams_config(ui_state.config_path, &ui_state.config);
    update_preset_buttons();

    log_and_notify(1, "Status: Playlist Presets Reordered");

    return TRUE;
}

static void on_preset_button_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    int slot = GPOINTER_TO_INT(user_data);
    activate_playlist_preset(slot);
}

typedef struct {
    QJTrack **tracks;
    int count;
} PendingAddData;

static guint find_track_index_by_path(const char *target_path) {
    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(track_list_store));
    for (guint i = 0; i < n_items; i++) {
        QJTrack *item = QJ_TRACK(g_list_model_get_item(G_LIST_MODEL(track_list_store), i));
        bool matches = (g_strcmp0(item->filepath, target_path) == 0);
        g_object_unref(item);
        if (matches)
            return i;
    }
    return GTK_INVALID_LIST_POSITION;
}

static void execute_pending_adds(PendingAddData *pd) {
    int added = 0;
    for (int i = 0; i < pd->count; i++) {
        if (find_track_index_by_path(pd->tracks[i]->filepath) == GTK_INVALID_LIST_POSITION) {
            g_list_store_append(track_list_store, pd->tracks[i]);
            added++;
        }
        g_object_unref(pd->tracks[i]);
    }
    free(pd->tracks);
    free(pd);

    if (added > 0) {
        save_current_playlist();
        update_playlist_toggle_state();
    }
}

static void on_save_new_playlist_ready(GObject *source, GAsyncResult *res, gpointer data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    GError *err = NULL;
    GFile *file = gtk_file_dialog_save_finish(dialog, res, &err);
    PendingAddData *pd = (PendingAddData *)data;

    if (file) {
        char *path = g_file_get_path(file);
        if (!g_str_has_suffix(path, ".m3u")) {
            char *tmp = g_strdup_printf("%s.m3u", path);
            g_free(path);
            path = tmp;
        }

        strncpy(ui_state.playlist_path, path, sizeof(ui_state.playlist_path) - 1);
        ui_state.playlist_path[sizeof(ui_state.playlist_path) - 1] = '\0';
        strncpy(ui_state.config.last_playlist_path, path,
                sizeof(ui_state.config.last_playlist_path) - 1);
        ui_state.config.last_playlist_path[sizeof(ui_state.config.last_playlist_path) - 1] = '\0';

        if (current_preset_slot >= 0 && current_preset_slot < 22) {
            strncpy(ui_state.config.playlist_slots[current_preset_slot], path,
                    sizeof(ui_state.config.playlist_slots[0]) - 1);
            ui_state.config.playlist_slots[current_preset_slot]
                                          [sizeof(ui_state.config.playlist_slots[0]) - 1] = '\0';
        }

        save_qjams_config(ui_state.config_path, &ui_state.config);
        update_playlist_title_ui(path);
        update_preset_buttons();

        g_free(path);
        g_object_unref(file);

        execute_pending_adds(pd);
    } else {
        for (int i = 0; i < pd->count; i++)
            g_object_unref(pd->tracks[i]);
        free(pd->tracks);
        free(pd);
        if (err)
            g_error_free(err);
    }
}

static void request_append_tracks(QJTrack **tracks, int count, GtkWidget *parent) {
    PendingAddData *pd = malloc(sizeof(PendingAddData));
    pd->tracks = malloc(sizeof(QJTrack *) * count);
    pd->count = count;
    for (int i = 0; i < count; i++)
        pd->tracks[i] = tracks[i];

    if (strlen(ui_state.playlist_path) == 0) {
        GtkFileDialog *dialog = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dialog, "Save New Playlist");
        gtk_file_dialog_set_initial_name(dialog, "New_Playlist.m3u");

        if (strlen(ui_state.config.recordings_dir) > 0) {
            GFile *initial_folder = g_file_new_for_path(ui_state.config.recordings_dir);
            gtk_file_dialog_set_initial_folder(dialog, initial_folder);
            g_object_unref(initial_folder);
        }

        GtkFileFilter *filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, "M3U Playlists (*.m3u)");
        gtk_file_filter_add_pattern(filter, "*.m3u");
        GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
        g_list_store_append(filters, filter);
        gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
        gtk_file_dialog_set_default_filter(dialog, filter);

        g_object_unref(filter);
        g_object_unref(filters);
        gtk_file_dialog_save(dialog, GTK_WINDOW(parent), NULL, on_save_new_playlist_ready, pd);
        g_object_unref(dialog);
    } else {
        execute_pending_adds(pd);
    }
}

typedef struct {
    guint next_idx;
} CycleTrackData;

static void on_cycle_track_confirm(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    if (gtk_alert_dialog_choose_finish(alert, res, NULL) == 0) {
        CycleTrackData *cd = (CycleTrackData *)user_data;

        atomic_store_explicit(&current_recording_track, 0, memory_order_release);
        atomic_store_explicit(&selected_tracks_mask, 1, memory_order_release);

        QJTrack *next_track =
            QJ_TRACK(g_list_model_get_item(G_LIST_MODEL(track_list_store), cd->next_idx));
        strncpy(ui_state.selected_track_path, next_track->filepath,
                sizeof(ui_state.selected_track_path) - 1);

        execute_global_canvas_reset(next_track->filepath);
        g_object_unref(next_track);
    }
    g_free(user_data);
}

// ui_playlist.c
static void execute_jukebox_load(guint track_idx) {
    bool has_any_audio = false;
    for (int i = 0; i < QJ_MAX_TRACKS; i++) {
        if (atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) {
            has_any_audio = true;
            break;
        }
    }

    if (has_any_audio && is_session_dirty() && pristine_frames > 0) {
        CycleTrackData *cd = g_new(CycleTrackData, 1);
        cd->next_idx = track_idx;

        GtkAlertDialog *alert = gtk_alert_dialog_new("Discard Active Session?");
        gtk_alert_dialog_set_detail(
            alert,
            "Loading a new track will discard your currently recorded, unsaved audio. Continue?");
        const char *buttons[] = {"Discard & Load", "Cancel", NULL};
        gtk_alert_dialog_set_buttons(alert, buttons);
        gtk_alert_dialog_set_cancel_button(alert, 1);
        gtk_alert_dialog_set_default_button(alert, 1);

        gtk_alert_dialog_choose(alert, GTK_WINDOW(gtk_widget_get_root(lbl_track)), NULL,
                                on_cycle_track_confirm, cd);
        g_object_unref(alert);
        return;
    }

    atomic_store_explicit(&current_recording_track, 0, memory_order_release);
    atomic_store_explicit(&selected_tracks_mask, 1, memory_order_release);

    QJTrack *next_track =
        QJ_TRACK(g_list_model_get_item(G_LIST_MODEL(track_list_store), track_idx));
    strncpy(ui_state.selected_track_path, next_track->filepath,
            sizeof(ui_state.selected_track_path) - 1);

    execute_global_canvas_reset(next_track->filepath);

    g_object_unref(next_track);
}

bool cycle_playlist_track(int direction) {
    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(track_list_store));
    if (n_items == 0)
        return false;

    guint current_idx = find_track_index_by_path(ui_state.selected_track_path);

    if (current_idx == GTK_INVALID_LIST_POSITION) {
        current_idx = 0;
    } else {
        // Standardized ring buffer math: branchless and inherently safe
        current_idx = (guint)(((int)current_idx + direction + (int)n_items) % (int)n_items);
    }

    execute_jukebox_load(current_idx);
    return true;
}

void save_current_playlist(void) {
    // ... [Lines 399-408]
    if (strlen(ui_state.playlist_path) == 0)
        return;
    FILE *f = fopen(ui_state.playlist_path, "w");
    if (!f)
        return;

    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(track_list_store));
    for (guint i = 0; i < n_items; i++) {
        QJTrack *item = QJ_TRACK(g_list_model_get_item(G_LIST_MODEL(track_list_store), i));
        fprintf(f, "%s\n", item->filepath);
        g_object_unref(item);
    }
    fclose(f);
}

void remove_playlist_tracks(GtkBitset *selections) {
    if (!selections || gtk_bitset_is_empty(selections))
        return;

    GtkBitset *safe_selections = gtk_bitset_copy(selections);
    int count = 0;
    GtkBitsetIter iter;
    guint index;

    if (gtk_bitset_iter_init_last(&iter, safe_selections, &index)) {
        do {
            g_list_store_remove(track_list_store, index);
            count++;
        } while (gtk_bitset_iter_previous(&iter, &index));
    }

    gtk_bitset_unref(safe_selections);

    if (count > 0) {
        log_and_notify(1, "Status: Removed %d track(s)", count);
        save_current_playlist();
        update_playlist_toggle_state();
    }
}

void remove_active_track_from_playlist(const char *path) {
    guint index = find_track_index_by_path(path);
    if (index != GTK_INVALID_LIST_POSITION) {
        GtkBitset *bitset = gtk_bitset_new_empty();
        gtk_bitset_add(bitset, index);
        remove_playlist_tracks(bitset);
        gtk_bitset_unref(bitset);
    }
}

static void on_delete_selected_confirm(GObject *source_object, GAsyncResult *res,
                                       gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    if (gtk_alert_dialog_choose_finish(alert, res, NULL) == 0) {
        GtkBitset *selections = (GtkBitset *)user_data;
        remove_playlist_tracks(selections);
        gtk_bitset_unref(selections);
    } else {
        gtk_bitset_unref((GtkBitset *)user_data);
    }
}

static void on_delete_selected_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    GtkSelectionModel *model = gtk_list_view_get_model(GTK_LIST_VIEW(track_list_view));
    GtkBitset *selections = gtk_selection_model_get_selection(model);

    if (gtk_bitset_is_empty(selections)) {
        gtk_bitset_unref(selections);
        return;
    }

    GtkAlertDialog *alert = gtk_alert_dialog_new("Remove Selected Tracks?");
    gtk_alert_dialog_set_detail(
        alert, "Are you sure you want to remove the selected tracks from the playlist?");
    const char *buttons[] = {"Remove", "Cancel", NULL};
    gtk_alert_dialog_set_buttons(alert, buttons);
    gtk_alert_dialog_set_cancel_button(alert, 1);
    gtk_alert_dialog_set_default_button(alert, 1);

    GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(btn)));
    gtk_alert_dialog_choose(alert, parent, NULL, on_delete_selected_confirm, selections);
    g_object_unref(alert);
}

static gboolean on_playlist_key_pressed(GtkEventControllerKey *controller, guint keyval,
                                        guint keycode, GdkModifierType state, gpointer user_data) {
    (void)controller;
    (void)keycode;
    (void)state;
    (void)user_data;
    if (keyval == GDK_KEY_Delete) {
        if (btn_delete && gtk_widget_is_sensitive(btn_delete)) {
            g_signal_emit_by_name(btn_delete, "clicked");
        }
        return TRUE;
    }
    return FALSE;
}

bool playlist_handle_ctrl_a(void) {
    if (!track_selection_model)
        return FALSE;
    GtkBitset *selections =
        gtk_selection_model_get_selection(GTK_SELECTION_MODEL(track_selection_model));
    bool is_empty = gtk_bitset_is_empty(selections);
    gtk_bitset_unref(selections);
    return is_empty ? TRUE : FALSE;
}

static void update_playlist_title_ui(const char *filepath) {
    if (!lbl_playlist_title)
        return;
    if (!filepath || strlen(filepath) == 0) {
        gtk_label_set_markup(GTK_LABEL(lbl_playlist_title), "<b>Unsaved Playlist</b>");
        return;
    }

    char *basename = g_path_get_basename(filepath);
    char *dot = strrchr(basename, '.');
    if (dot)
        *dot = '\0';
    char *escaped_base = g_markup_escape_text(basename, -1);
    char markup[512];
    snprintf(markup, sizeof(markup), "<b>%s</b>", escaped_base);
    gtk_label_set_markup(GTK_LABEL(lbl_playlist_title), markup);
    g_free(escaped_base);
    g_free(basename);
}

void load_playlist_from_file(const char *filepath) {
    int found_slot = -1;
    for (int i = 0; i < 22; i++) {
        if (g_strcmp0(ui_state.config.playlist_slots[i], filepath) == 0) {
            found_slot = i;
            break;
        }
    }

    if (found_slot != -1) {
        current_preset_slot = found_slot;
    } else if (current_preset_slot >= 0 && current_preset_slot < 22 &&
               strlen(ui_state.config.playlist_slots[current_preset_slot]) == 0) {
        strncpy(ui_state.config.playlist_slots[current_preset_slot], filepath,
                sizeof(ui_state.config.playlist_slots[0]) - 1);
        ui_state.config
            .playlist_slots[current_preset_slot][sizeof(ui_state.config.playlist_slots[0]) - 1] =
            '\0';
        save_qjams_config(ui_state.config_path, &ui_state.config);
    } else {
        current_preset_slot = -1;
    }

    update_preset_buttons();

    FILE *f = fopen(filepath, "r");
    if (f) {
        char *base_dir = g_path_get_dirname(filepath);
        g_list_store_remove_all(track_list_store);
        char line[1024];

        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (strlen(line) == 0 || line[0] == '#')
                continue;
            if (!is_supported_media_file(line))
                continue;

            char *abs_path = NULL;
            if (g_path_is_absolute(line)) {
                abs_path = g_strdup(line);
            } else {
                abs_path = g_build_filename(base_dir, line, NULL);
            }

            char display_buf[512];
            bool is_native = extract_track_metadata(abs_path, display_buf, sizeof(display_buf));
            QJTrack *trk = qj_track_new(abs_path, display_buf, is_native);
            g_list_store_append(track_list_store, trk);

            g_object_unref(trk);
            g_free(abs_path);
        }
        g_free(base_dir);
        fclose(f);
        log_and_notify(1, "Status: Playlist Loaded");
        update_playlist_title_ui(filepath);
    } else {
        log_and_notify(1, "Status: Failed to load playlist file.");
    }
}

void update_playlist_toggle_state(void) {
    if (!btn_playlist_toggle)
        return;

    if (strlen(ui_state.selected_track_path) == 0) {
        gtk_button_set_icon_name(GTK_BUTTON(btn_playlist_toggle), "");
        gtk_button_set_label(GTK_BUTTON(btn_playlist_toggle), "+");
        gtk_widget_set_tooltip_text(btn_playlist_toggle, "No Track/Session Selected");
        gtk_widget_set_sensitive(btn_playlist_toggle, FALSE);
        gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(track_selection_model));
        return;
    }

    gtk_widget_set_sensitive(btn_playlist_toggle, TRUE);
    guint found_idx = find_track_index_by_path(ui_state.selected_track_path);

    if (found_idx != GTK_INVALID_LIST_POSITION) {
        gtk_button_set_icon_name(GTK_BUTTON(btn_playlist_toggle), "");
        gtk_button_set_label(GTK_BUTTON(btn_playlist_toggle), "-");
        gtk_widget_set_tooltip_text(btn_playlist_toggle, "Remove Track/Session from Playlist");
        gtk_selection_model_select_item(GTK_SELECTION_MODEL(track_selection_model), found_idx,
                                        TRUE);
    } else {
        gtk_button_set_icon_name(GTK_BUTTON(btn_playlist_toggle), "");
        gtk_button_set_label(GTK_BUTTON(btn_playlist_toggle), "+");
        gtk_widget_set_tooltip_text(btn_playlist_toggle, "Add Track/Session to Playlist");
        gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(track_selection_model));
    }
}

void on_playlist_toggle_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    if (strlen(ui_state.selected_track_path) == 0)
        return;

    guint existing_index = find_track_index_by_path(ui_state.selected_track_path);
    if (existing_index != GTK_INVALID_LIST_POSITION) {
        remove_active_track_from_playlist(ui_state.selected_track_path);
        return;
    }

    char display_buf[512];
    bool is_native =
        extract_track_metadata(ui_state.selected_track_path, display_buf, sizeof(display_buf));
    QJTrack *new_track = qj_track_new(ui_state.selected_track_path, display_buf, is_native);

    QJTrack *tracks[] = {new_track};
    request_append_tracks(tracks, 1, GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(button))));
}

static bool sort_ascending = true;
static bool is_playlist_custom_ordered = false;

static gint compare_tracks(gconstpointer a, gconstpointer b, gpointer user_data) {
    (void)user_data;
    QJTrack *track_a = *(QJTrack **)a;
    QJTrack *track_b = *(QJTrack **)b;
    int result = g_strcmp0(track_a->display_name, track_b->display_name);
    return sort_ascending ? result : -result;
}

static void perform_playlist_sort(GtkButton *button) {
    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(track_list_store));
    if (n_items <= 1)
        return;

    GPtrArray *array = g_ptr_array_new();
    for (guint i = 0; i < n_items; i++) {
        g_ptr_array_add(array, g_list_model_get_item(G_LIST_MODEL(track_list_store), i));
    }

    g_ptr_array_sort_with_data(array, compare_tracks, NULL);

    g_list_store_remove_all(track_list_store);
    for (guint i = 0; i < array->len; i++) {
        g_list_store_append(track_list_store, array->pdata[i]);
        g_object_unref(array->pdata[i]);
    }
    g_ptr_array_free(array, TRUE);

    sort_ascending = !sort_ascending;
    is_playlist_custom_ordered = false;
    gtk_button_set_icon_name(button, "");
    gtk_button_set_label(button, sort_ascending ? "⬇" : "⬆");
    save_current_playlist();
}

static void on_sort_confirm_response(GObject *source_object, GAsyncResult *res,
                                     gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    if (gtk_alert_dialog_choose_finish(alert, res, NULL) == 0) {
        perform_playlist_sort(GTK_BUTTON(user_data));
    }
}

static void on_sort_playlist_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    if (is_playlist_custom_ordered) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Sort Custom Playlist?");
        gtk_alert_dialog_set_detail(alert, "You have manually reordered tracks. Sorting will "
                                           "overwrite your custom order. Continue?");
        const char *buttons[] = {"Sort", "Cancel", NULL};
        gtk_alert_dialog_set_buttons(alert, buttons);
        gtk_alert_dialog_set_cancel_button(alert, 1);
        gtk_alert_dialog_set_default_button(alert, 1);
        gtk_alert_dialog_choose(alert, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button))), NULL,
                                on_sort_confirm_response, button);
        g_object_unref(alert);
    } else {
        perform_playlist_sort(button);
    }
}

static GdkContentProvider *on_drag_prepare(GtkDragSource *source, double x, double y,
                                           GtkWidget *widget) {
    (void)source;
    (void)x;
    (void)y;
    QJTrack *track = g_object_get_data(G_OBJECT(widget), "track");
    if (!track)
        return NULL;
    return gdk_content_provider_new_typed(QJ_TYPE_TRACK, track);
}

static gboolean on_drop(GtkDropTarget *target, const GValue *value, double x, double y,
                        GtkWidget *widget) {
    (void)target;
    (void)x;
    (void)y;
    QJTrack *source_track = g_value_get_object(value);
    QJTrack *target_track = g_object_get_data(G_OBJECT(widget), "track");

    if (!source_track || !target_track || source_track == target_track)
        return FALSE;

    guint src_pos = 0, dest_pos = 0;
    gboolean found_src = FALSE, found_dest = FALSE;
    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(track_list_store));

    for (guint i = 0; i < n_items; i++) {
        QJTrack *t = QJ_TRACK(g_list_model_get_item(G_LIST_MODEL(track_list_store), i));
        if (t == source_track) {
            src_pos = i;
            found_src = TRUE;
        }
        if (t == target_track) {
            dest_pos = i;
            found_dest = TRUE;
        }
        g_object_unref(t);
    }

    if (found_src && found_dest) {
        g_object_ref(source_track);
        g_list_store_remove(track_list_store, src_pos);
        if (src_pos < dest_pos)
            dest_pos--;
        g_list_store_insert(track_list_store, dest_pos, source_track);
        g_object_unref(source_track);
        is_playlist_custom_ordered = true;
        save_current_playlist();
        return TRUE;
    }
    return FALSE;
}

static void on_setup_list_item(GtkSignalListItemFactory *factory, GtkListItem *list_item,
                               gpointer user_data) {
    (void)factory;
    (void)user_data;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_margin_start(box, 5);
    gtk_widget_set_margin_top(box, 5);
    gtk_widget_set_margin_bottom(box, 5);

    GtkWidget *icon = gtk_label_new(NULL);
    GtkWidget *label = gtk_label_new(NULL);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);

    GtkDragSource *drag_source = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag_source, GDK_ACTION_COPY | GDK_ACTION_MOVE);
    g_signal_connect(drag_source, "prepare", G_CALLBACK(on_drag_prepare), box);
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drag_source));

    GtkDropTarget *drop_target = gtk_drop_target_new(QJ_TYPE_TRACK, GDK_ACTION_MOVE);
    g_signal_connect(drop_target, "drop", G_CALLBACK(on_drop), box);
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drop_target));

    g_object_set_data(G_OBJECT(box), "icon", icon);
    g_object_set_data(G_OBJECT(box), "label", label);
    gtk_list_item_set_child(list_item, box);
}

static void on_bind_list_item(GtkSignalListItemFactory *factory, GtkListItem *list_item,
                              gpointer user_data) {
    (void)factory;
    (void)user_data;
    QJTrack *track = QJ_TRACK(gtk_list_item_get_item(list_item));
    GtkWidget *box = gtk_list_item_get_child(list_item);

    GtkWidget *icon = g_object_get_data(G_OBJECT(box), "icon");
    GtkWidget *label = g_object_get_data(G_OBJECT(box), "label");

    g_object_set_data(G_OBJECT(box), "track", track);
    gtk_label_set_text(GTK_LABEL(label), track->display_name);

    if (g_str_has_suffix(track->filepath, ".qjams")) {
        gtk_label_set_text(GTK_LABEL(icon), "🗎");
    } else {
        gtk_label_set_text(GTK_LABEL(icon), "🎵");
    }
}

typedef struct {
    QJTrack *track;
    GtkWidget *entry;
    GtkWidget *dialog;
} RenameData;

static void free_rename_data(gpointer data) {
    RenameData *rd = (RenameData *)data;
    g_object_unref(rd->track);
    free(rd);
}

typedef struct {
    char *old_path;
    char *new_path;
    char *new_name;
    QJTrack *track;
    bool is_audio_rewrite;
    bool success;
} AsyncRenameData;

static void free_async_rename_data(gpointer data) {
    AsyncRenameData *ard = (AsyncRenameData *)data;
    g_free(ard->old_path);
    g_free(ard->new_path);
    g_free(ard->new_name);
    g_free(ard);
}

/**
 * @brief Background worker that renames a track file on disk and updates its title tag
 *        via FFmpeg stream copy when applicable. Uses an atomic staging path if the
 *        source and destination paths are identical to prevent in-place truncation.
 */
static void rename_track_thread(GTask *task, gpointer source_object, gpointer task_data,
                                GCancellable *cancellable) {
    (void)source_object;
    (void)cancellable;
    AsyncRenameData *ard = (AsyncRenameData *)task_data;
    bool same_path = (g_strcmp0(ard->old_path, ard->new_path) == 0);
    if (ard->is_audio_rewrite) {
        const char *ext = strrchr(ard->new_path, '.');
        if (!ext)
            ext = "";
        char *target_out = same_path ? g_strdup_printf("%s.ren_tmp%s", ard->new_path, ext)
                                     : g_strdup(ard->new_path);
        char *meta_title = g_strdup_printf("title=%s", ard->new_name);
        char *argv[] = {"ffmpeg", "-y",   "-loglevel", "error",    "-i",       ard->old_path,
                        "-c",     "copy", "-metadata", meta_title, target_out, NULL};
        int exit_status = 0;
        g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &exit_status,
                     NULL);
        g_free(meta_title);
        if (exit_status == 0 && g_file_test(target_out, G_FILE_TEST_IS_REGULAR)) {
            if (same_path) {
                ard->success = (rename(target_out, ard->new_path) == 0);
                if (!ard->success)
                    remove(target_out);
            } else {
                remove(ard->old_path);
                ard->success = true;
            }
        } else {
            if (same_path)
                remove(target_out);
            ard->success = false;
        }
        g_free(target_out);
    } else {
        if (same_path) {
            ard->success = true;
        } else if (rename(ard->old_path, ard->new_path) == 0) {
            ard->success = true;
        } else {
            ard->success = false;
        }
    }
    g_task_return_boolean(task, ard->success);
}

static void rename_track_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object;
    (void)user_data;
    GError *error = NULL;
    gboolean success = g_task_propagate_boolean(G_TASK(res), &error);
    AsyncRenameData *ard = (AsyncRenameData *)g_task_get_task_data(G_TASK(res));

    extern GtkWidget *main_spinner;
    if (main_spinner)
        gtk_spinner_stop(GTK_SPINNER(main_spinner));

    GtkWindow *win =
        gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (win) {
        GtkWidget *master_box = gtk_window_get_child(win);
        if (master_box)
            gtk_widget_set_sensitive(master_box, TRUE);
    }
    set_app_waiting_state(false);

    if (success) {
        g_free(ard->track->filepath);
        ard->track->filepath = g_strdup(ard->new_path);

        g_free(ard->track->display_name);
        char display_buf[512];
        extract_track_metadata(ard->new_path, display_buf, sizeof(display_buf));
        ard->track->display_name = g_strdup(display_buf);

        guint pos = find_track_index_by_path(ard->new_path);
        if (pos != GTK_INVALID_LIST_POSITION) {
            g_object_ref(ard->track);
            g_list_store_remove(track_list_store, pos);
            g_list_store_insert(track_list_store, pos, ard->track);
            gtk_selection_model_select_item(GTK_SELECTION_MODEL(track_selection_model), pos, TRUE);
            g_object_unref(ard->track);
        }

        save_current_playlist();

        if (g_strcmp0(ui_state.selected_track_path, ard->old_path) == 0) {
            strncpy(ui_state.selected_track_path, ard->track->filepath,
                    sizeof(ui_state.selected_track_path) - 1);
            ui_state.selected_track_path[sizeof(ui_state.selected_track_path) - 1] = '\0';
            char ui_text[600];
            snprintf(ui_text, sizeof(ui_text), "Track: %s", display_buf);
            extern GtkWidget *lbl_track;
            if (lbl_track)
                gtk_label_set_text(GTK_LABEL(lbl_track), ui_text);
        }
        log_and_notify(1, "Status: Track Renamed");
    } else {
        log_and_notify(1, "Status: Rename Failed");
    }

    g_object_unref(ard->track);
}

static void on_rename_confirm(GtkButton *btn, gpointer user_data) {
    (void)btn;
    RenameData *rd = (RenameData *)user_data;
    const char *new_name = gtk_editable_get_text(GTK_EDITABLE(rd->entry));

    if (strlen(new_name) > 0) {
        char *dir = g_path_get_dirname(rd->track->filepath);
        char *ext = strrchr(rd->track->filepath, '.');
        if (!ext)
            ext = "";

        char *new_path = g_strdup_printf("%s/%s%s", dir, new_name, ext);

        bool is_audio = false;
        if (g_str_has_suffix(rd->track->filepath, ".flac") ||
            g_str_has_suffix(rd->track->filepath, ".wav") ||
            g_str_has_suffix(rd->track->filepath, ".mp3") ||
            g_str_has_suffix(rd->track->filepath, ".ogg")) {
            is_audio = true;
        }

        AsyncRenameData *ard = g_new0(AsyncRenameData, 1);
        ard->old_path = g_strdup(rd->track->filepath);
        ard->new_path = g_strdup(new_path);
        ard->new_name = g_strdup(new_name);
        ard->track = rd->track;
        ard->is_audio_rewrite = is_audio;

        g_object_ref(ard->track);

        extern GtkWidget *main_spinner;
        if (main_spinner)
            gtk_spinner_start(GTK_SPINNER(main_spinner));

        GtkWindow *win_main =
            gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
        if (win_main) {
            GtkWidget *master_box = gtk_window_get_child(win_main);
            if (master_box)
                gtk_widget_set_sensitive(master_box, FALSE);
        }
        set_app_waiting_state(true);

        GTask *task = g_task_new(NULL, NULL, rename_track_ready, NULL);
        g_task_set_task_data(task, ard, free_async_rename_data);
        g_task_run_in_thread(task, rename_track_thread);
        g_object_unref(task);

        g_free(new_path);
        g_free(dir);
    }

    GtkWidget *win = rd->dialog;
    gtk_window_destroy(GTK_WINDOW(win));
}

static void on_rename_track_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    GtkBitset *selections =
        gtk_selection_model_get_selection(GTK_SELECTION_MODEL(track_selection_model));
    if (gtk_bitset_is_empty(selections)) {
        gtk_bitset_unref(selections);
        return;
    }

    guint pos = gtk_bitset_get_nth(selections, 0);
    gtk_bitset_unref(selections);

    gpointer item = g_list_model_get_item(G_LIST_MODEL(track_list_store), pos);
    if (!item)
        return;

    QJTrack *track = QJ_TRACK(item);

    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Rename Track");
    gtk_window_set_transient_for(GTK_WINDOW(dialog),
                                 GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button))));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 300, 100);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 15);
    gtk_widget_set_margin_end(vbox, 15);
    gtk_widget_set_margin_top(vbox, 15);
    gtk_widget_set_margin_bottom(vbox, 15);

    char *basename = g_path_get_basename(track->filepath);
    char *dot = strrchr(basename, '.');
    if (dot)
        *dot = '\0';

    GtkWidget *entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), basename);
    g_free(basename);

    GtkWidget *btn_confirm = gtk_button_new_with_label("Rename Track");
    gtk_widget_set_halign(btn_confirm, GTK_ALIGN_END);

    RenameData *rd = malloc(sizeof(RenameData));
    rd->track = track;
    rd->entry = entry;
    rd->dialog = dialog;

    g_object_set_data_full(G_OBJECT(dialog), "rename_data", rd, free_rename_data);
    g_signal_connect(btn_confirm, "clicked", G_CALLBACK(on_rename_confirm), rd);
    g_signal_connect(entry, "activate", G_CALLBACK(on_rename_confirm), rd);

    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Enter new track name:"));
    gtk_box_append(GTK_BOX(vbox), entry);
    gtk_box_append(GTK_BOX(vbox), btn_confirm);

    gtk_window_set_child(GTK_WINDOW(dialog), vbox);
    gtk_window_present(GTK_WINDOW(dialog));
}

static gboolean is_supported_media_file(const char *path) {
    if (!path)
        return FALSE;
    const char *ext = strrchr(path, '.');
    if (!ext)
        return FALSE;
    if (g_ascii_strcasecmp(ext, ".qjams") == 0)
        return TRUE;
    return is_audio_file(path) || is_video_file(path);
}

static void on_track_activated(GtkListView *list, guint position, gpointer user_data) {
    (void)list;
    (void)user_data;
    execute_jukebox_load(position);
}

static void on_playlist_selection_changed(GtkSelectionModel *model, guint position, guint n_items,
                                          gpointer user_data) {
    (void)position;
    (void)n_items;
    (void)user_data;
    GtkBitset *selections = gtk_selection_model_get_selection(model);
    guint count = gtk_bitset_get_size(selections);
    gtk_bitset_unref(selections);

    gtk_widget_set_sensitive(btn_rename, count == 1);
    gtk_widget_set_sensitive(btn_delete, count > 0);
}

static int get_unique_name_suffix(const char *base_title) {
    int max_suffix = 0;
    gboolean exact_found = FALSE;
    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(track_list_store));

    for (guint i = 0; i < n_items; i++) {
        QJTrack *t = QJ_TRACK(g_list_model_get_item(G_LIST_MODEL(track_list_store), i));
        const char *title = t->display_name;

        if (title) {
            if (g_str_has_prefix(title, base_title)) {
                const char *remainder = title + strlen(base_title);
                if (g_str_has_prefix(remainder, " [")) {
                    exact_found = TRUE;
                } else if (g_str_has_prefix(remainder, " (")) {
                    int suffix = 0;
                    if (sscanf(remainder, " (%d) [", &suffix) == 1) {
                        if (suffix > max_suffix)
                            max_suffix = suffix;
                    }
                }
            }
        }
        g_object_unref(t);
    }

    if (!exact_found)
        return 0;
    return max_suffix + 1;
}

typedef struct {
    char path[1024];
    char display[512];
    bool is_native;
    bool is_valid;
} PlaylistDropItem;

typedef struct {
    PlaylistDropItem *items;
    int count;
} PlaylistDropData;

static void free_playlist_drop_data(gpointer data) {
    PlaylistDropData *p = (PlaylistDropData *)data;
    if (p) {
        free(p->items);
        free(p);
    }
}

static void playlist_drop_thread(GTask *task, gpointer source_object, gpointer task_data,
                                 GCancellable *cancellable) {
    (void)source_object;
    (void)cancellable;
    PlaylistDropData *data = (PlaylistDropData *)task_data;

    for (int i = 0; i < data->count; i++) {
        if (is_supported_media_file(data->items[i].path)) {
            data->items[i].is_native = extract_track_metadata(
                data->items[i].path, data->items[i].display, sizeof(data->items[i].display));
            data->items[i].is_valid = true;
        } else {
            data->items[i].is_valid = false;
        }
    }
    g_task_return_boolean(task, TRUE);
}

static void playlist_drop_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object;
    (void)user_data;
    GError *error = NULL;
    g_task_propagate_boolean(G_TASK(res), &error);

    PlaylistDropData *data = g_task_get_task_data(G_TASK(res));
    if (!data)
        return;


    int valid_count = 0;
    QJTrack **pending_tracks = malloc(sizeof(QJTrack *) * data->count);

    for (int i = 0; i < data->count; i++) {
        if (!data->items[i].is_valid)
            continue;

        if (find_track_index_by_path(data->items[i].path) == GTK_INVALID_LIST_POSITION) {
            char base_name[512] = {0};
            char duration_part[64] = {0};
            char *bracket = strrchr(data->items[i].display, '[');

            if (bracket && bracket > data->items[i].display && *(bracket - 1) == ' ') {
                size_t len = bracket - data->items[i].display - 1;
                if (len >= sizeof(base_name))
                    len = sizeof(base_name) - 1;
                strncpy(base_name, data->items[i].display, len);
                base_name[len] = '\0';
                strncpy(duration_part, bracket, sizeof(duration_part) - 1);
                duration_part[sizeof(duration_part) - 1] = '\0';
            } else {
                strncpy(base_name, data->items[i].display, sizeof(base_name) - 1);
                base_name[sizeof(base_name) - 1] = '\0';
            }

            int suffix = get_unique_name_suffix(base_name);
            char final_display[1024] = {0};

            if (suffix > 0) {
                if (strlen(duration_part) > 0) {
                    snprintf(final_display, sizeof(final_display), "%s (%d) %s", base_name, suffix,
                             duration_part);
                } else {
                    snprintf(final_display, sizeof(final_display), "%s (%d)", base_name, suffix);
                }
            } else {
                snprintf(final_display, sizeof(final_display), "%s", data->items[i].display);
            }

            QJTrack *new_track =
                qj_track_new(data->items[i].path, final_display, data->items[i].is_native);
            pending_tracks[valid_count++] = new_track;
        }
    }

    if (valid_count > 0) {
        request_append_tracks(pending_tracks, valid_count,
                              GTK_WIDGET(gtk_widget_get_root(lbl_track)));
    } else {
        log_and_notify(1, "Status: Playlist up to date");
    }

    free(pending_tracks);
    gtk_spinner_stop(GTK_SPINNER(main_spinner));
}

static gboolean on_playlist_file_drop(GtkDropTarget *target, const GValue *value, double x,
                                      double y, gpointer user_data) {
    (void)target;
    (void)x;
    (void)y;
    (void)user_data;
    if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST))
        return FALSE;

    GdkFileList *file_list = g_value_get_boxed(value);
    if (!file_list)
        return FALSE;

    GSList *files = gdk_file_list_get_files(file_list);
    if (!files)
        return FALSE;

    char **paths = NULL;
    int path_count = 0;
    int path_capacity = 32;
    paths = malloc(path_capacity * sizeof(char *));

    for (GSList *l = files; l != NULL; l = l->next) {
        GFile *file = G_FILE(l->data);
        char *path = g_file_get_path(file);
        if (path) {
            if (g_str_has_suffix(path, ".m3u") || g_str_has_suffix(path, ".M3U")) {
                FILE *f = fopen(path, "r");
                if (f) {
                    char *base_dir = g_path_get_dirname(path);
                    char line[1024];

                    while (fgets(line, sizeof(line), f)) {
                        line[strcspn(line, "\r\n")] = 0;
                        if (strlen(line) == 0 || line[0] == '#')
                            continue;
                        if (!is_supported_media_file(line))
                            continue;

                        if (path_count >= path_capacity) {
                            path_capacity *= 2;
                            char **tmp_paths = realloc(paths, path_capacity * sizeof(char *));
                            if (!tmp_paths) {
                                log_and_notify(1, "Status: Playlist load aborted (Out of memory)");
                                break;
                            }
                            paths = tmp_paths;
                        }

                        if (g_path_is_absolute(line)) {
                            paths[path_count++] = g_strdup(line);
                        } else {
                            paths[path_count++] = g_build_filename(base_dir, line, NULL);
                        }
                    }
                    g_free(base_dir);
                    fclose(f);
                }
                g_free(path);
            } else {
                if (path_count >= path_capacity) {
                    path_capacity *= 2;
                    char **tmp_paths = realloc(paths, path_capacity * sizeof(char *));
                    if (!tmp_paths) {
                        g_free(path);
                        log_and_notify(1, "Status: Playlist load aborted (Out of memory)");
                        break;
                    }
                    paths = tmp_paths;
                }
                paths[path_count++] = path;
            }
        }
    }

    g_slist_free(files);

    if (path_count == 0) {
        free(paths);
        return FALSE;
    }

    PlaylistDropData *data = malloc(sizeof(PlaylistDropData));
    data->count = path_count;
    data->items = calloc(path_count, sizeof(PlaylistDropItem));

    for (int i = 0; i < path_count; i++) {
        strncpy(data->items[i].path, paths[i], sizeof(data->items[i].path) - 1);
        g_free(paths[i]);
    }
    free(paths);

    gtk_spinner_start(GTK_SPINNER(main_spinner));

    log_and_notify(1, "Status: Inspecting %d file(s)...", path_count);

    GTask *task = g_task_new(NULL, NULL, playlist_drop_ready, NULL);
    g_task_set_task_data(task, data, free_playlist_drop_data);
    g_task_run_in_thread(task, playlist_drop_thread);
    g_object_unref(task);

    return TRUE;
}

typedef struct {
    GtkWidget *entry;
    GtkWidget *dialog;
} RenamePlaylistData;

static void on_rename_playlist_confirm(GtkButton *btn, gpointer user_data) {
    (void)btn;
    RenamePlaylistData *rd = (RenamePlaylistData *)user_data;
    const char *new_name = gtk_editable_get_text(GTK_EDITABLE(rd->entry));

    if (strlen(new_name) > 0 && strlen(ui_state.playlist_path) > 0) {
        char *dir = g_path_get_dirname(ui_state.playlist_path);
        char *new_path = g_strdup_printf("%s/%s.m3u", dir, new_name);

        if (rename(ui_state.playlist_path, new_path) == 0) {
            for (int i = 0; i < 22; i++) {
                if (g_strcmp0(ui_state.config.playlist_slots[i], ui_state.playlist_path) == 0) {
                    strncpy(ui_state.config.playlist_slots[i], new_path,
                            sizeof(ui_state.config.playlist_slots[0]) - 1);
                }
            }
            strncpy(ui_state.playlist_path, new_path, sizeof(ui_state.playlist_path) - 1);
            strncpy(ui_state.config.last_playlist_path, new_path,
                    sizeof(ui_state.config.last_playlist_path) - 1);

            save_qjams_config(ui_state.config_path, &ui_state.config);
            update_playlist_title_ui(ui_state.playlist_path);
            update_preset_buttons();
        }
        g_free(new_path);
        g_free(dir);
    }

    GtkWidget *win = rd->dialog;
    gtk_window_destroy(GTK_WINDOW(win));
}

static void on_rename_playlist_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    if (strlen(ui_state.playlist_path) == 0)
        return;

    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Rename Playlist");
    gtk_window_set_transient_for(GTK_WINDOW(dialog),
                                 GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button))));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 300, 100);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 15);
    gtk_widget_set_margin_end(vbox, 15);
    gtk_widget_set_margin_top(vbox, 15);
    gtk_widget_set_margin_bottom(vbox, 15);

    char *basename = g_path_get_basename(ui_state.playlist_path);
    char *dot = strrchr(basename, '.');
    if (dot)
        *dot = '\0';

    GtkWidget *entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), basename);
    g_free(basename);

    GtkWidget *btn_confirm = gtk_button_new_with_label("Rename File");
    gtk_widget_set_halign(btn_confirm, GTK_ALIGN_END);

    RenamePlaylistData *rd = malloc(sizeof(RenamePlaylistData));
    rd->entry = entry;
    rd->dialog = dialog;

    g_object_set_data_full(G_OBJECT(dialog), "rename_data", rd, free);
    g_signal_connect(btn_confirm, "clicked", G_CALLBACK(on_rename_playlist_confirm), rd);
    g_signal_connect(entry, "activate", G_CALLBACK(on_rename_playlist_confirm), rd);

    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Enter new playlist name:"));
    gtk_box_append(GTK_BOX(vbox), entry);
    gtk_box_append(GTK_BOX(vbox), btn_confirm);

    gtk_window_set_child(GTK_WINDOW(dialog), vbox);
    gtk_window_present(GTK_WINDOW(dialog));
}

static void on_clear_playlist_confirm(GObject *source_object, GAsyncResult *res,
                                      gpointer user_data) {
    (void)user_data;
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    if (gtk_alert_dialog_choose_finish(alert, res, NULL) == 0) {
        if (current_preset_slot >= 0 && current_preset_slot < 22) {
            ui_state.config.playlist_slots[current_preset_slot][0] = '\0';
            save_qjams_config(ui_state.config_path, &ui_state.config);
        }
        ui_state.playlist_path[0] = '\0';
        g_list_store_remove_all(track_list_store);
        update_playlist_title_ui("");
        update_preset_buttons();

        log_and_notify(1, "Status: Playlist Slot Cleared");
    }
}

static void on_clear_playlist_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    if (current_preset_slot == -1 && strlen(ui_state.playlist_path) == 0)
        return;

    GtkAlertDialog *alert = gtk_alert_dialog_new("Clear Active Playlist?");
    gtk_alert_dialog_set_detail(alert,
                                "This will clear the active playlist from memory and unbind the "
                                "preset slot. The .m3u file on your disk will NOT be deleted.");
    const char *buttons[] = {"Clear Playlist", "Cancel", NULL};
    gtk_alert_dialog_set_buttons(alert, buttons);
    gtk_alert_dialog_set_cancel_button(alert, 1);
    gtk_alert_dialog_set_default_button(alert, 1);

    GtkWidget *parent = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(btn)));
    gtk_alert_dialog_choose(alert, GTK_WINDOW(parent), NULL, on_clear_playlist_confirm, NULL);
    g_object_unref(alert);
}

static void on_delete_playlist_confirm(GObject *source_object, GAsyncResult *res,
                                       gpointer user_data) {
    (void)user_data;
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    if (gtk_alert_dialog_choose_finish(alert, res, NULL) == 0) {
        if (strlen(ui_state.playlist_path) > 0) {
            remove(ui_state.playlist_path);

            if (current_preset_slot >= 0 && current_preset_slot < 22) {
                ui_state.config.playlist_slots[current_preset_slot][0] = '\0';
                save_qjams_config(ui_state.config_path, &ui_state.config);
            }

            ui_state.playlist_path[0] = '\0';
            g_list_store_remove_all(track_list_store);
            update_playlist_title_ui("");
            update_preset_buttons();

            log_and_notify(1, "Status: Playlist Deleted from Disk");
        }
    }
}

static void on_delete_playlist_clicked(GtkButton *btn, gpointer user_data) {
    (void)user_data;
    if (strlen(ui_state.playlist_path) == 0)
        return;

    char *basename = g_path_get_basename(ui_state.playlist_path);
    char detail_msg[512];
    snprintf(detail_msg, sizeof(detail_msg),
             "Are you sure you want to permanently delete '%s' from your disk? This action cannot "
             "be undone.",
             basename);

    GtkAlertDialog *alert = gtk_alert_dialog_new("Delete Playlist File?");
    gtk_alert_dialog_set_detail(alert, detail_msg);
    g_free(basename);

    const char *buttons[] = {"Delete from Disk", "Cancel", NULL};
    gtk_alert_dialog_set_buttons(alert, buttons);
    gtk_alert_dialog_set_cancel_button(alert, 1);
    gtk_alert_dialog_set_default_button(alert, 1);

    GtkWidget *parent = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(btn)));
    gtk_alert_dialog_choose(alert, GTK_WINDOW(parent), NULL, on_delete_playlist_confirm, NULL);
    g_object_unref(alert);
}

// Safety [Moderate]: Uses strict GtkBitset iterators to prevent memory faults during
// multi-selection processing.
static gboolean on_preset_track_drop(GtkDropTarget *target, const GValue *value, double x, double y,
                                     gpointer user_data) {
    if (G_VALUE_HOLDS(value, G_TYPE_INT)) {
        return on_preset_drop(target, value, x, y, user_data);
    }
    (void)target;
    (void)x;
    (void)y;
    int dest_slot = GPOINTER_TO_INT(user_data);
    if (dest_slot < 0 || dest_slot >= 22)
        return FALSE;

    char target_m3u[1024];
    if (strlen(ui_state.config.playlist_slots[dest_slot]) == 0) {
        snprintf(target_m3u, sizeof(target_m3u), "%s/Playlist_%d.m3u",
                 ui_state.config.recordings_dir, dest_slot + 1);
        strncpy(ui_state.config.playlist_slots[dest_slot], target_m3u,
                sizeof(ui_state.config.playlist_slots[0]) - 1);
        ui_state.config.playlist_slots[dest_slot][sizeof(ui_state.config.playlist_slots[0]) - 1] =
            '\0';
        save_qjams_config(ui_state.config_path, &ui_state.config);

        update_preset_buttons();
    } else {
        strncpy(target_m3u, ui_state.config.playlist_slots[dest_slot], sizeof(target_m3u) - 1);
        target_m3u[sizeof(target_m3u) - 1] = '\0';
    }

    bool is_same_playlist = (current_preset_slot == dest_slot);
    int added_count = 0;

    if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        GdkFileList *file_list = g_value_get_boxed(value);
        if (file_list) {
            FILE *f = fopen(target_m3u, "a");
            GSList *files = gdk_file_list_get_files(file_list);
            for (GSList *l = files; l != NULL; l = l->next) {
                GFile *file = G_FILE(l->data);
                char *path = g_file_get_path(file);
                if (path) {
                    if (f)
                        fprintf(f, "%s\n", path);
                    if (is_same_playlist) {
                        char display_buf[512];
                        bool is_native =
                            extract_track_metadata(path, display_buf, sizeof(display_buf));
                        QJTrack *trk = qj_track_new(path, display_buf, is_native);
                        g_list_store_append(track_list_store, trk);
                        g_object_unref(trk);
                    }
                    g_free(path);
                    added_count++;
                }
            }
            if (f)
                fclose(f);
            g_slist_free(files);
        }
    } else if (G_VALUE_HOLDS(value, QJ_TYPE_TRACK)) {
        QJTrack *dropped_track = g_value_get_object(value);
        if (!dropped_track || !dropped_track->filepath)
            return FALSE;

        GtkBitset *selections =
            gtk_selection_model_get_selection(GTK_SELECTION_MODEL(track_selection_model));
        bool is_in_selection = false;

        guint n_items = g_list_model_get_n_items(G_LIST_MODEL(track_list_store));
        for (guint i = 0; i < n_items; i++) {
            if (gtk_bitset_contains(selections, i)) {
                QJTrack *t = QJ_TRACK(g_list_model_get_item(G_LIST_MODEL(track_list_store), i));
                if (t == dropped_track)
                    is_in_selection = true;
                g_object_unref(t);
                if (is_in_selection)
                    break;
            }
        }

        FILE *f = fopen(target_m3u, "a");

        if (is_in_selection) {
            GtkBitsetIter iter;
            guint index;
            if (gtk_bitset_iter_init_first(&iter, selections, &index)) {
                do {
                    QJTrack *t =
                        QJ_TRACK(g_list_model_get_item(G_LIST_MODEL(track_list_store), index));
                    if (t && t->filepath) {
                        if (f)
                            fprintf(f, "%s\n", t->filepath);
                        if (is_same_playlist) {
                            QJTrack *dup =
                                qj_track_new(t->filepath, t->display_name, t->is_qjams_native);
                            g_list_store_append(track_list_store, dup);
                            g_object_unref(dup);
                        }
                        added_count++;
                    }
                    if (t)
                        g_object_unref(t);
                } while (gtk_bitset_iter_next(&iter, &index));
            }
            if (f)
                fclose(f);

            if (!is_same_playlist) {
                remove_playlist_tracks(selections);
            }
        } else {
            if (f) {
                fprintf(f, "%s\n", dropped_track->filepath);
                fclose(f);
            }
            if (is_same_playlist) {
                QJTrack *dup = qj_track_new(dropped_track->filepath, dropped_track->display_name,
                                            dropped_track->is_qjams_native);
                g_list_store_append(track_list_store, dup);
                g_object_unref(dup);
            }
            added_count++;

            if (!is_same_playlist) {
                remove_active_track_from_playlist(dropped_track->filepath);
            }
        }
        gtk_bitset_unref(selections);
    }

    if (added_count > 0) {
        log_and_notify(1, "Status: Appended %d track(s) to Preset %d", added_count, dest_slot + 1);
        return TRUE;
    }
    return FALSE;
}

GtkWidget *create_playlist_widget(void) {
    track_list_store = g_list_store_new(QJ_TYPE_TRACK);
    track_selection_model =
        GTK_MULTI_SELECTION(gtk_multi_selection_new(G_LIST_MODEL(track_list_store)));

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(on_setup_list_item), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(on_bind_list_item), NULL);

    track_list_view = gtk_list_view_new(GTK_SELECTION_MODEL(track_selection_model), factory);
    g_signal_connect(track_selection_model, "selection-changed",
                     G_CALLBACK(on_playlist_selection_changed), NULL);

    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_playlist_key_pressed), NULL);
    gtk_widget_add_controller(track_list_view, key_ctrl);

    g_signal_connect(track_list_view, "activate", G_CALLBACK(on_track_activated), NULL);

    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled_window, TRUE);
    gtk_widget_set_valign(scrolled_window, GTK_ALIGN_FILL);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled_window), 250);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scrolled_window), FALSE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);

    GtkDropTarget *file_drop_target = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(file_drop_target, "drop", G_CALLBACK(on_playlist_file_drop), NULL);
    gtk_widget_add_controller(scrolled_window, GTK_EVENT_CONTROLLER(file_drop_target));

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), track_list_view);

    GtkWidget *header_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_bottom(header_vbox, 5);

    lbl_playlist_title = gtk_label_new("<b>Unsaved Playlist</b>");
    gtk_label_set_use_markup(GTK_LABEL(lbl_playlist_title), TRUE);
    gtk_label_set_xalign(GTK_LABEL(lbl_playlist_title), 0.0);

    // Clean geometry + global 10.5pt class
    gtk_widget_set_halign(lbl_playlist_title, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(lbl_playlist_title, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(lbl_playlist_title), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(lbl_playlist_title, "subpixel-label");

    GtkWidget *action_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

    GtkWidget *left_action_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_hexpand(left_action_box, TRUE);
    gtk_widget_set_halign(left_action_box, GTK_ALIGN_START);

    GtkWidget *right_action_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_halign(right_action_box, GTK_ALIGN_END);

    GtkWidget *btn_rename_playlist = gtk_button_new_with_label("🖉");
    gtk_widget_set_tooltip_text(btn_rename_playlist, "Rename Active Playlist");
    gtk_widget_add_css_class(btn_rename_playlist, "flat");
    gtk_widget_add_css_class(btn_rename_playlist, "symbol-btn");
    g_signal_connect(btn_rename_playlist, "clicked", G_CALLBACK(on_rename_playlist_clicked), NULL);

    GtkWidget *btn_clear_playlist = gtk_button_new_with_label("⏫");
    gtk_widget_set_tooltip_text(btn_clear_playlist, "Unbind/Eject Playlist from Slot");
    gtk_widget_add_css_class(btn_clear_playlist, "flat");
    gtk_widget_add_css_class(btn_clear_playlist, "symbol-btn");
    gtk_widget_add_css_class(btn_clear_playlist, "btn-eject");
    g_signal_connect(btn_clear_playlist, "clicked", G_CALLBACK(on_clear_playlist_clicked), NULL);

    GtkWidget *btn_delete_playlist = gtk_button_new_with_label("🗑");
    gtk_widget_set_tooltip_text(btn_delete_playlist, "Delete Playlist from Disk");
    gtk_widget_add_css_class(btn_delete_playlist, "flat");
    gtk_widget_add_css_class(btn_delete_playlist, "symbol-btn");
    gtk_widget_add_css_class(btn_delete_playlist, "btn-reset");
    g_signal_connect(btn_delete_playlist, "clicked", G_CALLBACK(on_delete_playlist_clicked), NULL);

    btn_rename = gtk_button_new_with_label("🖉");
    gtk_widget_set_tooltip_text(btn_rename, "Rename Selected Track");
    gtk_widget_add_css_class(btn_rename, "flat");
    gtk_widget_add_css_class(btn_rename, "symbol-btn");
    gtk_widget_set_sensitive(btn_rename, FALSE);
    g_signal_connect(btn_rename, "clicked", G_CALLBACK(on_rename_track_clicked), NULL);

    btn_delete = gtk_button_new_with_label("🗑");
    gtk_widget_set_tooltip_text(btn_delete, "Remove Selected Tracks (Del)");
    gtk_widget_add_css_class(btn_delete, "flat");
    gtk_widget_add_css_class(btn_delete, "symbol-btn");
    gtk_widget_add_css_class(btn_delete, "btn-reset");
    gtk_widget_set_sensitive(btn_delete, FALSE);
    g_signal_connect(btn_delete, "clicked", G_CALLBACK(on_delete_selected_clicked), NULL);

    GtkWidget *btn_sort = gtk_button_new_with_label("🔃");
    gtk_widget_set_tooltip_text(btn_sort, "Sort Alphabetically");
    gtk_widget_add_css_class(btn_sort, "flat");
    gtk_widget_add_css_class(btn_sort, "symbol-btn");
    g_signal_connect(btn_sort, "clicked", G_CALLBACK(on_sort_playlist_clicked), NULL);

    gtk_box_append(GTK_BOX(left_action_box), btn_rename_playlist);
    gtk_box_append(GTK_BOX(left_action_box), btn_clear_playlist);
    gtk_box_append(GTK_BOX(left_action_box), btn_delete_playlist);

    gtk_box_append(GTK_BOX(right_action_box), btn_rename);
    gtk_box_append(GTK_BOX(right_action_box), btn_delete);
    gtk_box_append(GTK_BOX(right_action_box), btn_sort);

    gtk_box_append(GTK_BOX(action_hbox), left_action_box);
    gtk_box_append(GTK_BOX(action_hbox), right_action_box);

    gtk_box_append(GTK_BOX(header_vbox), lbl_playlist_title);
    gtk_box_append(GTK_BOX(header_vbox), action_hbox);

    GtkWidget *preset_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_bottom(preset_vbox, 5);

    GtkWidget *preset_box_1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_halign(preset_box_1, GTK_ALIGN_CENTER);
    GtkWidget *preset_box_2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_halign(preset_box_2, GTK_ALIGN_CENTER);

    for (int i = 0; i < 22; i++) {
        preset_btns[i] = gtk_button_new_with_label("-");
        gtk_widget_set_size_request(preset_btns[i], 30, 30);
        gtk_widget_add_css_class(preset_btns[i], "flat");

        g_signal_connect(preset_btns[i], "clicked", G_CALLBACK(on_preset_button_clicked),
                         GINT_TO_POINTER(i));

        GtkDragSource *drag_src = gtk_drag_source_new();
        gtk_drag_source_set_actions(drag_src, GDK_ACTION_MOVE);
        g_signal_connect(drag_src, "prepare", G_CALLBACK(on_preset_drag_prepare),
                         GINT_TO_POINTER(i));
        gtk_widget_add_controller(preset_btns[i], GTK_EVENT_CONTROLLER(drag_src));

        GType preset_drop_types[] = {G_TYPE_INT, GDK_TYPE_FILE_LIST, QJ_TYPE_TRACK};
        GtkDropTarget *track_drop_target =
            gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY | GDK_ACTION_MOVE);
        gtk_drop_target_set_gtypes(track_drop_target, preset_drop_types, 3);
        g_signal_connect(track_drop_target, "drop", G_CALLBACK(on_preset_track_drop),
                         GINT_TO_POINTER(i));
        gtk_widget_add_controller(preset_btns[i], GTK_EVENT_CONTROLLER(track_drop_target));

        if (i < 11)
            gtk_box_append(GTK_BOX(preset_box_1), preset_btns[i]);
        else
            gtk_box_append(GTK_BOX(preset_box_2), preset_btns[i]);
    }

    gtk_box_append(GTK_BOX(preset_vbox), preset_box_1);
    gtk_box_append(GTK_BOX(preset_vbox), preset_box_2);

    current_preset_slot = -1;
    for (int i = 0; i < 22; i++) {
        if (strlen(ui_state.config.playlist_slots[i]) > 0 &&
            g_strcmp0(ui_state.config.playlist_slots[i], ui_state.playlist_path) == 0) {
            current_preset_slot = i;
            break;
        }
    }
    update_preset_buttons();

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(main_box, 8);
    gtk_widget_set_margin_end(main_box, 8);
    gtk_widget_set_margin_top(main_box, 8);
    gtk_widget_set_margin_bottom(main_box, 8);
    // CRITICAL: Pass expansion to the inner box
    gtk_widget_set_hexpand(main_box, TRUE);
    gtk_widget_set_halign(main_box, GTK_ALIGN_FILL);

    gtk_box_append(GTK_BOX(main_box), header_vbox);
    gtk_box_append(GTK_BOX(main_box), preset_vbox);
    gtk_box_append(GTK_BOX(main_box), scrolled_window);

    GtkWidget *frame = gtk_frame_new(NULL);
    // Hard mathematical lock to 405px
    gtk_widget_set_size_request(frame, 405, -1);
    gtk_widget_set_hexpand(frame, FALSE);
    gtk_widget_set_vexpand(frame, TRUE);
    gtk_widget_set_halign(frame, GTK_ALIGN_FILL);
    gtk_widget_set_valign(frame, GTK_ALIGN_FILL);
    gtk_frame_set_child(GTK_FRAME(frame), main_box);

    FILE *f = fopen(ui_state.playlist_path, "r");
    if (f) {
        char *base_dir = g_path_get_dirname(ui_state.playlist_path);
        char line[1024];

        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (strlen(line) == 0 || line[0] == '#')
                continue;
            if (!is_supported_media_file(line))
                continue;

            char *abs_path = NULL;
            if (g_path_is_absolute(line)) {
                abs_path = g_strdup(line);
            } else {
                abs_path = g_build_filename(base_dir, line, NULL);
            }

            if (!g_file_test(abs_path, G_FILE_TEST_EXISTS)) {
                g_free(abs_path);
                continue;
            }

            char display_buf[512];
            bool is_native = extract_track_metadata(abs_path, display_buf, sizeof(display_buf));
            QJTrack *trk = qj_track_new(abs_path, display_buf, is_native);
            g_list_store_append(track_list_store, trk);

            g_object_unref(trk);
            g_free(abs_path);
        }
        g_free(base_dir);
        fclose(f);
    }

    update_playlist_title_ui(ui_state.playlist_path);
    return frame;
}
