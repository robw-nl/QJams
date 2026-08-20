#include "ui_playlist.h"
#include "ui_globals.h"
#include "ui_multitrack.h"
#include "audio_engine.h"
#include <libavformat/avformat.h>
#include <time.h> // NEW: Required for the FFmpeg probe timeout

// --- PLAYLIST UI GLOBALS ---
static GListStore *track_list_store = NULL;
static GtkMultiSelection *track_selection_model = NULL;
static GtkWidget *track_list_view;
static GtkWidget *btn_rename;
static GtkWidget *btn_delete;
static GtkWidget *preset_btns[10];
static GtkWidget *lbl_playlist_title = NULL;
static int current_preset_slot = -1;
G_DEFINE_TYPE(QjTrack, qj_track, G_TYPE_OBJECT)

static gboolean is_supported_media_file(const char *path);

/**
 * @brief Destructor for the QjTrack GObject, freeing strings.
 * @param object The GObject to finalize.
 * @return void
 */
static void qj_track_finalize(GObject *object) {
    QjTrack *self = QJ_TRACK(object);
    g_free(self->filepath);
    g_free(self->display_name);
    G_OBJECT_CLASS(qj_track_parent_class)->finalize(object);
}

/**
 * @brief Class initializer for the QjTrack GObject.
 * @param klass The class structure.
 * @return void
 */
static void qj_track_class_init(QjTrackClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = qj_track_finalize;
}

/**
 * @brief Instance initializer for the QjTrack GObject.
 * @param self The QjTrack instance.
 * @return void
 */
static void qj_track_init(QjTrack *self) {
    self->filepath = NULL;
    self->display_name = NULL;
    self->is_qjams_native = false;
}

/**
 * @brief Allocates and initializes a new QjTrack object.
 * @param filepath The absolute path of the audio file.
 * @param display_name The name formatted for the UI.
 * @return A pointer to the new QjTrack instance.
 */
QjTrack *qj_track_new(const char *filepath, const char *display_name, bool is_native) {
    QjTrack *self = g_object_new(QJ_TYPE_TRACK, NULL);
    self->filepath = g_strdup(filepath);
    self->display_name = g_strdup(display_name);
    self->is_qjams_native = is_native;
    return self;
}

// --- FFMPEG METADATA EXTRACTOR ---

// NEW: FFmpeg interrupt callback to prevent infinite hangs on corrupt files
typedef struct {
    time_t start_time;
    int max_duration;
} FFmpegProbeTimeout;

static int probe_interrupt_cb(void *ctx) {
    FFmpegProbeTimeout *timeout = (FFmpegProbeTimeout *)ctx;
    if (time(NULL) - timeout->start_time > timeout->max_duration) {
        return 1; // Abort FFmpeg blocking operation
    }
    return 0; // Continue
}

static bool extract_track_metadata(const char *filepath, char *output_buffer, size_t max_len) {
    bool is_native = false;

    if (g_str_has_suffix(filepath, ".qjams")) {
        gchar *basename = g_path_get_basename(filepath);
        char *dot = strrchr(basename, '.');
        if (dot) *dot = '\0';
        snprintf(output_buffer, max_len, "%s", basename);
        g_free(basename);
        return true;
    }

    // FIX: Allocate the context manually so we can bind the timeout interrupt BEFORE opening the file
    AVFormatContext *fmt_ctx = avformat_alloc_context();
    FFmpegProbeTimeout timeout = { .start_time = time(NULL), .max_duration = 3 }; // 3-second hard limit
    fmt_ctx->interrupt_callback.callback = probe_interrupt_cb;
    fmt_ctx->interrupt_callback.opaque = &timeout;

    if (avformat_open_input(&fmt_ctx, filepath, NULL, NULL) == 0) {
        avformat_find_stream_info(fmt_ctx, NULL);

        AVDictionaryEntry *title = NULL;
        AVDictionaryEntry *artist = NULL;
        AVDictionaryEntry *tag = NULL;

        // 1. Iterate all global tags. Bypass strict container mapping by checking all values for our signature.
        while ((tag = av_dict_get(fmt_ctx->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
            if (g_str_has_prefix(tag->value, "Recorded with QJams")) is_native = true;
            if (g_ascii_strcasecmp(tag->key, "title") == 0) title = tag;
            if (g_ascii_strcasecmp(tag->key, "artist") == 0) artist = tag;
        }

        // 2. Matroska Fallback: Check stream-level metadata if it wasn't found globally
        if (!is_native) {
            for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
                tag = NULL;
                while ((tag = av_dict_get(fmt_ctx->streams[i]->metadata, "", tag, AV_DICT_IGNORE_SUFFIX))) {
                    if (g_str_has_prefix(tag->value, "Recorded with QJams")) is_native = true;
                }
            }
        }

        int duration_sec = 0;
        if (fmt_ctx->duration != AV_NOPTS_VALUE) {
            duration_sec = fmt_ctx->duration / AV_TIME_BASE;
        }

        if (title && artist) {
            snprintf(output_buffer, max_len, "%s - %s [%02d:%02d]", artist->value, title->value, duration_sec / 60, duration_sec % 60);
        } else if (title) {
            snprintf(output_buffer, max_len, "%s [%02d:%02d]", title->value, duration_sec / 60, duration_sec % 60);
        } else {
            gchar *basename = g_path_get_basename(filepath);
            snprintf(output_buffer, max_len, "%s", basename);
            g_free(basename);
        }
        avformat_close_input(&fmt_ctx);
    } else {
        gchar *basename = g_path_get_basename(filepath);
        snprintf(output_buffer, max_len, "%s", basename);
        g_free(basename);
    }

    // Force UTF-8 validity to prevent GTK Pango crashes on legacy ID3 tags
    gchar *valid_utf8 = g_utf8_make_valid(output_buffer, -1);
    strncpy(output_buffer, valid_utf8, max_len - 1);
    output_buffer[max_len - 1] = '\0';
    g_free(valid_utf8);

    return is_native;
}

static void update_preset_buttons(void) {
    for (int i = 0; i < 10; i++) {
        if (!preset_btns[i]) continue;

        gtk_widget_remove_css_class(preset_btns[i], "active-preset");
        if (i == current_preset_slot) {
            gtk_widget_add_css_class(preset_btns[i], "active-preset");
        }

        if (strlen(ui_state.config.playlist_slots[i]) > 0) {
            char *base = g_path_get_basename(ui_state.config.playlist_slots[i]);
            char letter[2] = { '?', '\0' };
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
            gtk_widget_set_tooltip_text(preset_btns[i], "Empty Slot (Ctrl+Click: Assign, Right-Click: Delete)");
        }
    }
}

void assign_playlist_preset(int slot) {
    if (slot < 0 || slot >= 10) return;
    if (strlen(ui_state.playlist_path) > 0) {
        strncpy(ui_state.config.playlist_slots[slot], ui_state.playlist_path, sizeof(ui_state.config.playlist_slots[0]) - 1);
        save_qjams_config(ui_state.config_path, &ui_state.config);
        current_preset_slot = slot;
        update_preset_buttons();
        gtk_widget_grab_focus(preset_btns[slot]); // MOVES THE HIGHLIGHT RECTANGLE
        extern GtkWidget *lbl_status;
        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Playlist Preset Assigned");
    }
}

void activate_playlist_preset(int slot) {
    if (slot < 0 || slot >= 10) return;
    gtk_widget_grab_focus(preset_btns[slot]);
    current_preset_slot = slot;

    if (strlen(ui_state.config.playlist_slots[slot]) > 0) {
        strncpy(ui_state.playlist_path, ui_state.config.playlist_slots[slot], sizeof(ui_state.playlist_path) - 1);
        ui_state.playlist_path[sizeof(ui_state.playlist_path) - 1] = '\0';

        strncpy(ui_state.config.last_playlist_path, ui_state.config.playlist_slots[slot], sizeof(ui_state.config.last_playlist_path) - 1);
        ui_state.config.last_playlist_path[sizeof(ui_state.config.last_playlist_path) - 1] = '\0';

        save_qjams_config(ui_state.config_path, &ui_state.config);
        load_playlist_from_file(ui_state.playlist_path);
    } else {
        // NEW: Auto-create an empty playlist instead of prompting the user
        char new_path[1024];
        snprintf(new_path, sizeof(new_path), "%s/Preset_%d.m3u", ui_state.config.recordings_dir, slot + 1);

        FILE *f = fopen(new_path, "w");
        if (f) fclose(f);

        strncpy(ui_state.config.playlist_slots[slot], new_path, sizeof(ui_state.config.playlist_slots[0]) - 1);
        strncpy(ui_state.config.last_playlist_path, new_path, sizeof(ui_state.config.last_playlist_path) - 1);
        strncpy(ui_state.playlist_path, new_path, sizeof(ui_state.playlist_path) - 1);

        save_qjams_config(ui_state.config_path, &ui_state.config);

        load_playlist_from_file(new_path);
        update_preset_buttons();

        extern GtkWidget *lbl_status;
        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: New Empty Preset Created");
    }
}

static void on_preset_clear_confirm(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    int response = gtk_alert_dialog_choose_finish(alert, res, NULL);
    if (response == 0) { // Confirmed
        int slot = GPOINTER_TO_INT(user_data);
        ui_state.config.playlist_slots[slot][0] = '\0';
        save_qjams_config(ui_state.config_path, &ui_state.config);
        if (current_preset_slot == slot) current_preset_slot = -1;
        update_preset_buttons();
    }
}

/**
 * @brief Prepares an integer payload containing the source preset slot index for drag operations.
 * @param source The drag source controller.
 * @param x Drag start X coordinate.
 * @param y Drag start Y coordinate.
 * @param user_data Source slot index cast to gpointer.
 * @return GdkContentProvider containing the integer slot index.
 */
static GdkContentProvider* on_preset_drag_prepare(GtkDragSource *source, double x, double y, gpointer user_data) {
    (void)source; (void)x; (void)y;
    int slot = GPOINTER_TO_INT(user_data);
    GValue value = G_VALUE_INIT;
    g_value_init(&value, G_TYPE_INT);
    g_value_set_int(&value, slot);
    return gdk_content_provider_new_for_value(&value);
}

/**
 * @brief Handles dropping one preset button onto another, ripple-shifting slot assignments.
 * Matches the multitrack array reordering logic and updates config state.
 * @param target The drop target controller.
 * @param value The dragged GValue containing the source slot index.
 * @param x Drop X coordinate.
 * @param y Drop Y coordinate.
 * @param user_data Destination slot index cast to gpointer.
 * @return TRUE if reordered successfully, FALSE otherwise.
 */
static gboolean on_preset_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data) {
    (void)target; (void)x; (void)y;
    if (!G_VALUE_HOLDS(value, G_TYPE_INT)) return FALSE;

    int src_slot = g_value_get_int(value);
    int dest_slot = GPOINTER_TO_INT(user_data);

    if (src_slot == dest_slot || src_slot < 0 || src_slot >= 10 || dest_slot < 0 || dest_slot >= 10) {
        return FALSE;
    }

    char temp_slot[1024];
    strncpy(temp_slot, ui_state.config.playlist_slots[src_slot], sizeof(temp_slot) - 1);
    temp_slot[sizeof(temp_slot) - 1] = '\0';

    int step = (src_slot < dest_slot) ? 1 : -1;
    for (int i = src_slot; i != dest_slot; i += step) {
        int next = i + step;
        strncpy(ui_state.config.playlist_slots[i], ui_state.config.playlist_slots[next], sizeof(ui_state.config.playlist_slots[0]) - 1);
        ui_state.config.playlist_slots[i][sizeof(ui_state.config.playlist_slots[0]) - 1] = '\0';
    }

    strncpy(ui_state.config.playlist_slots[dest_slot], temp_slot, sizeof(ui_state.config.playlist_slots[0]) - 1);
    ui_state.config.playlist_slots[dest_slot][sizeof(ui_state.config.playlist_slots[0]) - 1] = '\0';

    if (current_preset_slot == src_slot) {
        current_preset_slot = dest_slot;
    } else if (src_slot < dest_slot && current_preset_slot > src_slot && current_preset_slot <= dest_slot) {
        current_preset_slot--;
    } else if (src_slot > dest_slot && current_preset_slot >= dest_slot && current_preset_slot < src_slot) {
        current_preset_slot++;
    }

    save_qjams_config(ui_state.config_path, &ui_state.config);
    update_preset_buttons();

    extern GtkWidget *lbl_status;
    gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Playlist Presets Reordered");

    return TRUE;
}

/**
 * @brief Standard left-click activation via native button signal.
 */
static void on_preset_button_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    int slot = GPOINTER_TO_INT(user_data);
    activate_playlist_preset(slot);
}

/**
 * @brief Intercepts modifier clicks (Ctrl) and right-clicks before the button processes them.
 * Bypasses normal left clicks to allow GtkDragSource and native button signals to function.
 */
static void on_preset_click_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press; (void)x; (void)y;
    int slot = GPOINTER_TO_INT(user_data);
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));

    if (button == GDK_BUTTON_SECONDARY) { // Right Click = Delete Preset
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        if (strlen(ui_state.config.playlist_slots[slot]) > 0) {
            GtkAlertDialog *alert = gtk_alert_dialog_new("Clear Preset Slot?");
            gtk_alert_dialog_set_detail(alert, "Remove the playlist assignment from this shortcut button?\n\n(The .m3u file itself will not be deleted from your drive).");
            const char *buttons[] = { "Clear Slot", "Cancel", NULL };
            gtk_alert_dialog_set_buttons(alert, buttons);
            gtk_alert_dialog_set_cancel_button(alert, 1);
            gtk_alert_dialog_set_default_button(alert, 1);

            GtkWidget *parent = GTK_WIDGET(gtk_widget_get_root(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture))));
            gtk_alert_dialog_choose(alert, GTK_WINDOW(parent), NULL, on_preset_clear_confirm, GINT_TO_POINTER(slot));
            g_object_unref(alert);
        }
    } else if (state & GDK_CONTROL_MASK) { // Ctrl + Click = Assign Preset
        gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
        assign_playlist_preset(slot);
    }
}

// --- PLAYLIST I/O & LOGIC ---
/**
 * @brief Searches the playlist store for a specific track by file path.
 * @param target_path The absolute file path to search for.
 * @return The index of the track if found, or GTK_INVALID_LIST_POSITION if not found.
 */
static guint find_track_index_by_path(const char* target_path) {
    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(track_list_store));
    for (guint i = 0; i < n_items; i++) {
        QjTrack *item = QJ_TRACK(g_list_model_get_item(G_LIST_MODEL(track_list_store), i));
        bool matches = (g_strcmp0(item->filepath, target_path) == 0);
        g_object_unref(item);
        if (matches) return i;
    }
    return GTK_INVALID_LIST_POSITION;
}

/**
 * @brief Cycles to the next or previous track in the playlist.
 * @param direction Direction to cycle (-1 for previous, 1 for next).
 * @return true if a new track was loaded, false otherwise.
 */
bool cycle_playlist_track(int direction) {
    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(track_list_store));
    if (n_items == 0) return false;

    guint current_idx = find_track_index_by_path(ui_state.selected_track_path);

    if (current_idx == GTK_INVALID_LIST_POSITION) {
        current_idx = 0;
    } else {
        int target_idx = ((int)current_idx + direction) % (int)n_items;
        if (target_idx < 0) target_idx += n_items;
        current_idx = (guint)target_idx;
    }

    QjTrack *next_track = QJ_TRACK(g_list_model_get_item(G_LIST_MODEL(track_list_store), current_idx));
    strncpy(ui_state.selected_track_path, next_track->filepath, sizeof(ui_state.selected_track_path) - 1);

    bool is_session = g_str_has_suffix(next_track->filepath, ".qjams");
    g_object_unref(next_track);

    if (is_session) {
        extern void command_post_load_session(const char *path);
        command_post_load_session(ui_state.selected_track_path);
    } else {
        trigger_track_load();
    }

    return true;
}

/**
 * @brief Saves the current playlist order and tracks to the default .m3u file.
 * @return void
 */
void save_current_playlist(void) {
    FILE *f = fopen(ui_state.playlist_path, "w");
    if (!f) return;

    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(track_list_store));
    for (guint i = 0; i < n_items; i++) {
        QjTrack *item = QJ_TRACK(g_list_model_get_item(G_LIST_MODEL(track_list_store), i));
        fprintf(f, "%s\n", item->filepath);
        g_object_unref(item);
    }
    fclose(f);
}

void remove_playlist_tracks(GtkBitset *selections) {
    if (!selections || gtk_bitset_is_empty(selections)) return;

    // NEW: Create an immutable copy of the bitset.
    // GTK mutates the original selection state every time we remove an item from the list!
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

    gtk_bitset_unref(safe_selections); // Free our copy to prevent memory leaks

    if (count > 0) {
        char status_msg[128];
        snprintf(status_msg, sizeof(status_msg), "Status: Removed %d track(s)", count);
        extern GtkWidget *lbl_status;
        gtk_label_set_text(GTK_LABEL(lbl_status), status_msg);
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

static void on_delete_selected_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    GtkSelectionModel *model = gtk_list_view_get_model(GTK_LIST_VIEW(track_list_view));
    GtkBitset *selections = gtk_selection_model_get_selection(model);
    remove_playlist_tracks(selections);
    gtk_bitset_unref(selections);
}

static gboolean on_playlist_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
    (void)controller; (void)keycode; (void)state; (void)user_data;
    if (keyval == GDK_KEY_Delete) {
        GtkSelectionModel *model = gtk_list_view_get_model(GTK_LIST_VIEW(track_list_view));
        GtkBitset *selections = gtk_selection_model_get_selection(model);
        remove_playlist_tracks(selections);
        gtk_bitset_unref(selections);
        return TRUE;
    }
    return FALSE;
}

/**
 * @brief Removes a track from the playlist and saves the updated list to disk.
 * @param filepath The absolute path to the track.
 * @return void
 */
void remove_track_from_playlist(const char* filepath) {
    guint idx = find_track_index_by_path(filepath);
    if (idx != GTK_INVALID_LIST_POSITION) {
        g_list_store_remove(track_list_store, idx);
        save_current_playlist();
    }
}

static void update_playlist_title_ui(const char *filepath) {
    if (!lbl_playlist_title || !filepath || strlen(filepath) == 0) return;
    char *basename = g_path_get_basename(filepath);
    char *dot = strrchr(basename, '.');
    if (dot) *dot = '\0';

    char markup[512];
    snprintf(markup, sizeof(markup), "<b>%s</b>", basename);
    gtk_label_set_markup(GTK_LABEL(lbl_playlist_title), markup);
    g_free(basename);
}

/**
 * @brief Loads track entries into the playlist from a given .m3u file.
 * @param filepath The absolute path to the playlist file.
 * @return void
 */
void load_playlist_from_file(const char *filepath) {
    current_preset_slot = -1;
    for (int i = 0; i < 5; i++) {
        if (g_strcmp0(ui_state.config.playlist_slots[i], filepath) == 0) {
            current_preset_slot = i;
            break;
        }
    }
    update_preset_buttons();

    FILE *f = fopen(filepath, "r");
    if (f) {
        g_list_store_remove_all(track_list_store);
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (strlen(line) == 0 || line[0] == '#') continue;
            if (!is_supported_media_file(line)) continue;

            char display_buf[512];
            bool is_native = extract_track_metadata(line, display_buf, sizeof(display_buf));
            QjTrack *trk = qj_track_new(line, display_buf, is_native);
            g_list_store_append(track_list_store, trk);
            g_object_unref(trk);
        }
        fclose(f);
        save_current_playlist();
        extern GtkWidget *lbl_status;
        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Playlist Loaded");

        update_playlist_title_ui(filepath); // ADD THIS
    }
}

/**
 * @brief Updates the UI state of the playlist toggle button based on the currently selected track.
 * @return void
 */
/**
 * @brief Updates the UI state of the playlist toggle button based on the currently selected track.
 * @return void
 */
void update_playlist_toggle_state(void) {
    if (!btn_playlist_toggle) return;

    if (strlen(ui_state.selected_track_path) == 0) {
        gtk_button_set_label(GTK_BUTTON(btn_playlist_toggle), "+");
        gtk_widget_set_tooltip_text(btn_playlist_toggle, "No Track Selected");
        gtk_widget_set_sensitive(btn_playlist_toggle, FALSE);
        gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(track_selection_model));
        return;
    }

    gtk_widget_set_sensitive(btn_playlist_toggle, TRUE);
    guint found_idx = find_track_index_by_path(ui_state.selected_track_path);

    if (found_idx != GTK_INVALID_LIST_POSITION) {
        gtk_button_set_label(GTK_BUTTON(btn_playlist_toggle), "-");
        gtk_widget_set_tooltip_text(btn_playlist_toggle, "Remove from Playlist");
        gtk_selection_model_select_item(GTK_SELECTION_MODEL(track_selection_model), found_idx, TRUE);
    } else {
        gtk_button_set_label(GTK_BUTTON(btn_playlist_toggle), "+");
        gtk_widget_set_tooltip_text(btn_playlist_toggle, "Add to Playlist");
        gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(track_selection_model));
    }
}

/**
 * @brief Callback that adds or removes the currently selected track from the active playlist.
 * @param button The GTK button triggering the callback.
 * @param user_data Optional user data passed to the callback.
 * @return void
 */
void on_playlist_toggle_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    (void)button;
    if (strlen(ui_state.selected_track_path) == 0) return;

    guint existing_index = find_track_index_by_path(ui_state.selected_track_path);

    if (existing_index != GTK_INVALID_LIST_POSITION) {
        remove_active_track_from_playlist(ui_state.selected_track_path);
    } else {
        char display_buf[512];
        bool is_native = extract_track_metadata(ui_state.selected_track_path, display_buf, sizeof(display_buf));
        QjTrack *new_track = qj_track_new(ui_state.selected_track_path, display_buf, is_native);
        g_list_store_append(track_list_store, new_track);
        g_object_unref(new_track);
        save_current_playlist();
        update_playlist_toggle_state();
    }
}

// --- NEW: ONE-SHOT PLAYLIST SORTING ---
static bool sort_ascending = true;
static bool is_playlist_custom_ordered = false;

static gint compare_tracks(gconstpointer a, gconstpointer b, gpointer user_data) {
    (void)user_data;
    QjTrack *track_a = *(QjTrack **)a;
    QjTrack *track_b = *(QjTrack **)b;
    int result = g_strcmp0(track_a->display_name, track_b->display_name);
    return sort_ascending ? result : -result;
}

static void perform_playlist_sort(GtkButton *button) {
    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(track_list_store));
    if (n_items <= 1) return;

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
    gtk_button_set_icon_name(button, sort_ascending ? "view-sort-descending-symbolic" : "view-sort-ascending-symbolic");
    save_current_playlist();
}

static void on_sort_confirm_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    int response = gtk_alert_dialog_choose_finish(alert, res, NULL);
    if (response == 0) {
        perform_playlist_sort(GTK_BUTTON(user_data));
    }
}

static void on_sort_playlist_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    if (is_playlist_custom_ordered) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Sort Custom Playlist?");
        gtk_alert_dialog_set_detail(alert, "You have manually reordered tracks. Sorting will overwrite your custom order. Continue?");
        const char *buttons[] = { "Sort", "Cancel", NULL };
        gtk_alert_dialog_set_buttons(alert, buttons);
        gtk_alert_dialog_set_cancel_button(alert, 1);
        gtk_alert_dialog_set_default_button(alert, 1);
        gtk_alert_dialog_choose(alert, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button))), NULL, on_sort_confirm_response, button);
        g_object_unref(alert);
        return;
    }
    perform_playlist_sort(button);
}

// --- TRACK LIST WIDGET FACTORIES (Drag & Drop) ---
/**
 * @brief Prepares data for a drag-and-drop operation within the playlist.
 * @param source The drag source.
 * @param x The X coordinate of the drag start.
 * @param y The Y coordinate of the drag start.
 * @param widget The widget initiating the drag.
 * @return A GdkContentProvider with the track data, or NULL if invalid.
 */
static GdkContentProvider* on_drag_prepare(GtkDragSource *source, double x, double y, GtkWidget *widget) {
    (void)source; (void)x; (void)y;
    QjTrack *track = g_object_get_data(G_OBJECT(widget), "track");
    if (!track) return NULL;
    return gdk_content_provider_new_typed(QJ_TYPE_TRACK, track);
}

/**
 * @brief Handles the drop event to reorder items in the playlist.
 * @param target The drop target.
 * @param value The value containing the dragged track.
 * @param x The X coordinate of the drop.
 * @param y The Y coordinate of the drop.
 * @param widget The widget receiving the drop.
 * @return TRUE if the drop was successful, FALSE otherwise.
 */
static gboolean on_drop(GtkDropTarget *target, const GValue *value, double x, double y, GtkWidget *widget) {
    (void)target; (void)x; (void)y;
    QjTrack *source_track = g_value_get_object(value);
    QjTrack *target_track = g_object_get_data(G_OBJECT(widget), "track");

    if (!source_track || !target_track || source_track == target_track) return FALSE;

    guint src_pos = 0, dest_pos = 0;
    gboolean found_src = FALSE, found_dest = FALSE;
    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(track_list_store));

    for (guint i = 0; i < n_items; i++) {
        QjTrack *t = QJ_TRACK(g_list_model_get_item(G_LIST_MODEL(track_list_store), i));
        if (t == source_track) { src_pos = i; found_src = TRUE; }
        if (t == target_track) { dest_pos = i; found_dest = TRUE; }
        g_object_unref(t);
    }

    if (found_src && found_dest) {
        g_object_ref(source_track);
        g_list_store_remove(track_list_store, src_pos);
        if (src_pos < dest_pos) dest_pos--;
        g_list_store_insert(track_list_store, dest_pos, source_track);
        g_object_unref(source_track);
        is_playlist_custom_ordered = true;
        save_current_playlist();
        return TRUE;
    }
    return FALSE;
}

/**
 * @brief Factory callback to set up the internal structure of a new playlist item.
 * @param factory The list item factory.
 * @param list_item The list item to set up.
 * @param user_data Optional user data.
 * @return void
 */
static void on_setup_list_item(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
    (void)factory; (void)user_data;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_margin_start(box, 5);
    gtk_widget_set_margin_top(box, 5);
    gtk_widget_set_margin_bottom(box, 5);

    GtkWidget *icon = gtk_image_new();
    GtkWidget *label = gtk_label_new(NULL);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END); // Kill natural size leak
    gtk_widget_set_hexpand(label, TRUE);

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), label);

    GtkDragSource *drag_source = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag_source, GDK_ACTION_MOVE);
    g_signal_connect(drag_source, "prepare", G_CALLBACK(on_drag_prepare), box);
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drag_source));

    GtkDropTarget *drop_target = gtk_drop_target_new(QJ_TYPE_TRACK, GDK_ACTION_MOVE);
    g_signal_connect(drop_target, "drop", G_CALLBACK(on_drop), box);
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drop_target));

    g_object_set_data(G_OBJECT(box), "icon", icon);
    g_object_set_data(G_OBJECT(box), "label", label);

    gtk_list_item_set_child(list_item, box);
}

/**
 * @brief Factory callback to bind data from a QjTrack to a playlist UI item.
 * @param factory The list item factory.
 * @param list_item The list item to bind data to.
 * @param user_data Optional user data.
 * @return void
 */
static void on_bind_list_item(GtkSignalListItemFactory *factory, GtkListItem *list_item, gpointer user_data) {
    (void)factory; (void)user_data;
    QjTrack *track = QJ_TRACK(gtk_list_item_get_item(list_item));
    GtkWidget *box = gtk_list_item_get_child(list_item);

    GtkWidget *icon = g_object_get_data(G_OBJECT(box), "icon");
    GtkWidget *label = g_object_get_data(G_OBJECT(box), "label");

    g_object_set_data(G_OBJECT(box), "track", track);
    gtk_label_set_text(GTK_LABEL(label), track->display_name);

    if (g_str_has_suffix(track->filepath, ".qjams")) {
    gtk_image_set_from_icon_name(GTK_IMAGE(icon), "view-list-symbolic");
    } else {
        gtk_image_set_from_icon_name(GTK_IMAGE(icon), "audio-x-generic-symbolic");
    }
}

// --- QJAMS NATIVE RENAMING ---
typedef struct {
    QjTrack *track;
    GtkWidget *entry;
    GtkWidget *dialog;
} RenameData;

// Custom free function to unref the nested GObject before freeing the struct
static void free_rename_data(gpointer data) {
    RenameData *rd = (RenameData *)data;
    g_object_unref(rd->track);
    free(rd);
}

static void on_rename_confirm(GtkButton *btn, gpointer user_data) {
    (void)btn;
    RenameData *rd = (RenameData *)user_data;
    const char *new_name = gtk_editable_get_text(GTK_EDITABLE(rd->entry));

    if (strlen(new_name) > 0) {
        char *dir = g_path_get_dirname(rd->track->filepath);
        char *ext = strrchr(rd->track->filepath, '.');
        if (!ext) ext = "";

        char *new_path = g_strdup_printf("%s/%s%s", dir, new_name, ext);

        // Apply FFmpeg metadata injection to ALL audio formats to prevent embedded tags from overriding the new name
        if (g_str_has_suffix(rd->track->filepath, ".flac") ||
            g_str_has_suffix(rd->track->filepath, ".wav")  ||
            g_str_has_suffix(rd->track->filepath, ".mp3")  ||
            g_str_has_suffix(rd->track->filepath, ".ogg")) {

            char *meta_title = g_strdup_printf("title=%s", new_name);
        char *argv[] = {"ffmpeg", "-y", "-i", rd->track->filepath, "-c", "copy", "-metadata", meta_title, new_path, NULL};
        g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
        g_free(meta_title);
        remove(rd->track->filepath);
            } else {
                // Fallback for .mkv and unknown files to prevent massive UI freezes during video copying
                rename(rd->track->filepath, new_path);
            }

            g_free(rd->track->filepath);
            rd->track->filepath = new_path;

            g_free(rd->track->display_name);
            char display_buf[512];
            extract_track_metadata(new_path, display_buf, sizeof(display_buf));
            rd->track->display_name = g_strdup(display_buf);

            guint pos = find_track_index_by_path(new_path);
            if (pos != GTK_INVALID_LIST_POSITION) {
                g_object_ref(rd->track);
                g_list_store_remove(track_list_store, pos);
                g_list_store_insert(track_list_store, pos, rd->track);
                gtk_selection_model_select_item(GTK_SELECTION_MODEL(track_selection_model), pos, TRUE);
                g_object_unref(rd->track);
            }

            save_current_playlist();
            g_free(dir);

            if (g_strcmp0(ui_state.selected_track_path, rd->track->filepath) != 0) {
                strncpy(ui_state.selected_track_path, rd->track->filepath, sizeof(ui_state.selected_track_path) - 1);
                char ui_text[600];
                snprintf(ui_text, sizeof(ui_text), "Track: %s", display_buf);
                gtk_label_set_text(GTK_LABEL(lbl_track), ui_text);
            }
    }

    GtkWidget *win = rd->dialog;
    gtk_window_destroy(GTK_WINDOW(win));
}

static void on_rename_track_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    GtkBitset *selections = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(track_selection_model));
    if (gtk_bitset_is_empty(selections)) {
        gtk_bitset_unref(selections);
        return;
    }

    guint pos = gtk_bitset_get_nth(selections, 0);
    gtk_bitset_unref(selections);

    gpointer item = g_list_model_get_item(G_LIST_MODEL(track_list_store), pos);
    if (!item) return;

    QjTrack *track = QJ_TRACK(item);

    // NATIVE GUARDRAIL HAS BEEN REMOVED HERE! ALL TRACKS CAN BE RENAMED.

    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Rename Track");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button))));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 300, 100);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 15);
    gtk_widget_set_margin_end(vbox, 15);
    gtk_widget_set_margin_top(vbox, 15);
    gtk_widget_set_margin_bottom(vbox, 15);

    char *basename = g_path_get_basename(track->filepath);
    char *dot = strrchr(basename, '.');
    if (dot) *dot = '\0';

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

    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Enter new track name:"));
    gtk_box_append(GTK_BOX(vbox), entry);
    gtk_box_append(GTK_BOX(vbox), btn_confirm);

    gtk_window_set_child(GTK_WINDOW(dialog), vbox);
    gtk_window_present(GTK_WINDOW(dialog));
}

static gboolean is_supported_media_file(const char *path) {
    if (!path) return FALSE;
    const char *ext = strrchr(path, '.');
    if (!ext) return FALSE;
    if (g_ascii_strcasecmp(ext, ".flac") == 0 || g_ascii_strcasecmp(ext, ".wav") == 0 ||
        g_ascii_strcasecmp(ext, ".ogg") == 0 || g_ascii_strcasecmp(ext, ".mp3") == 0 ||
        g_ascii_strcasecmp(ext, ".mkv") == 0 || g_ascii_strcasecmp(ext, ".qjams") == 0) {
        return TRUE;
    }
    return FALSE;
}

/**
 * @brief Callback triggered when a track is double-clicked or activated in the playlist view.
 * @param list The GTK list view triggering the event.
 * @param position The index of the activated item.
 * @param user_data Optional user data passed to the callback.
 * @return void
 */
static void on_track_activated(GtkListView *list, guint position, gpointer user_data) {
    (void)list; (void)user_data;
    QjTrack *track = QJ_TRACK(g_list_model_get_item(G_LIST_MODEL(track_list_store), position));
    if (!track) return;

    if (g_str_has_suffix(track->filepath, ".qjams")) {
        // Route sessions directly to the engine
        extern void command_post_load_session(const char *path);
        command_post_load_session(track->filepath);
    } else if (atomic_load_explicit(&is_multitrack_mode, memory_order_acquire)) {
        int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);
        multitrack_import_file_async(track->filepath, rec_track);
    } else {
        strncpy(ui_state.selected_track_path, track->filepath, sizeof(ui_state.selected_track_path) - 1);
        trigger_track_load();
    }

    g_object_unref(track);
}

static void on_playlist_selection_changed(GtkSelectionModel *model, guint position, guint n_items, gpointer user_data) {
    (void)position; (void)n_items; (void)user_data;
    GtkBitset *selections = gtk_selection_model_get_selection(model);
    guint count = gtk_bitset_get_size(selections);
    gtk_bitset_unref(selections);

    // Only the Track Pen requires a selection of exactly 1
    gtk_widget_set_sensitive(btn_rename, count == 1);
    gtk_widget_set_sensitive(btn_delete, count > 0);
}

static int get_unique_name_suffix(const char *base_title) {
    int max_suffix = 0;
    gboolean exact_found = FALSE;
    guint n_items = g_list_model_get_n_items(G_LIST_MODEL(track_list_store));

    for (guint i = 0; i < n_items; i++) {
        QjTrack *t = QJ_TRACK(g_list_model_get_item(G_LIST_MODEL(track_list_store), i));
        const char *title = t->display_name;

        if (title) {
            if (g_str_has_prefix(title, base_title)) {
                const char *remainder = title + strlen(base_title);
                if (g_str_has_prefix(remainder, " [")) {
                    exact_found = TRUE;
                } else if (g_str_has_prefix(remainder, " (")) {
                    int suffix = 0;
                    if (sscanf(remainder, " (%d) [", &suffix) == 1) {
                        if (suffix > max_suffix) max_suffix = suffix;
                    }
                }
            }
        }
        g_object_unref(t);
    }

    if (!exact_found) return 0;
    return max_suffix + 1;
}

// --- ASYNC PLAYLIST IMPORT PIPELINE ---

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

static void playlist_drop_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)source_object; (void)cancellable;
    PlaylistDropData *data = (PlaylistDropData *)task_data;

    // Heavy FFmpeg parsing runs securely in the background
    for (int i = 0; i < data->count; i++) {
        if (is_supported_media_file(data->items[i].path)) {
            data->items[i].is_native = extract_track_metadata(data->items[i].path, data->items[i].display, sizeof(data->items[i].display));
            data->items[i].is_valid = true;
        } else {
            data->items[i].is_valid = false;
        }
    }

    g_task_return_boolean(task, TRUE);
}

static void playlist_drop_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object; (void)user_data;
    GError *error = NULL;
    g_task_propagate_boolean(G_TASK(res), &error);

    PlaylistDropData *data = g_task_get_task_data(G_TASK(res));
    if (!data) return;

    extern GtkWidget *lbl_status;
    extern GtkWidget *main_spinner;
    int added_count = 0;
    int discarded_count = 0;

    // Safely update the GTK UI List Models exclusively in the main thread
    for (int i = 0; i < data->count; i++) {
        if (!data->items[i].is_valid) {
            discarded_count++;
            continue;
        }

        if (find_track_index_by_path(data->items[i].path) == GTK_INVALID_LIST_POSITION) {
            char base_name[512] = {0};
            char duration_part[64] = {0};
            char *bracket = strrchr(data->items[i].display, '[');

            if (bracket && bracket > data->items[i].display && *(bracket - 1) == ' ') {
                size_t len = bracket - data->items[i].display - 1;
                if (len >= sizeof(base_name)) len = sizeof(base_name) - 1;
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
                    snprintf(final_display, sizeof(final_display), "%s (%d) %s", base_name, suffix, duration_part);
                } else {
                    snprintf(final_display, sizeof(final_display), "%s (%d)", base_name, suffix);
                }
            } else {
                snprintf(final_display, sizeof(final_display), "%s", data->items[i].display);
            }

            QjTrack *new_track = qj_track_new(data->items[i].path, final_display, data->items[i].is_native);
            g_list_store_append(track_list_store, new_track);
            g_object_unref(new_track);
            added_count++;
        }
    }

    if (added_count > 0 || discarded_count > 0) {
        char status_msg[256];
        if (added_count > 0 && discarded_count > 0) {
            snprintf(status_msg, sizeof(status_msg), "Status: Added %d tracks (%d unsupported)", added_count, discarded_count);
        } else if (added_count > 0) {
            snprintf(status_msg, sizeof(status_msg), "Status: Added %d tracks", added_count);
        } else if (discarded_count > 0) {
            snprintf(status_msg, sizeof(status_msg), "Status: 0 tracks added (%d unsupported)", discarded_count);
        }
        gtk_label_set_text(GTK_LABEL(lbl_status), status_msg);
    } else {
        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Playlist up to date");
    }

    if (added_count > 0) {
        save_current_playlist();
        update_playlist_toggle_state();
    }

    gtk_spinner_stop(GTK_SPINNER(main_spinner));
    // Memory is automatically released by the GTask using free_playlist_drop_data
}

static gboolean on_playlist_file_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data) {
    (void)target; (void)x; (void)y; (void)user_data;
    if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) return FALSE;

    GdkFileList *file_list = g_value_get_boxed(value);
    if (!file_list) return FALSE;

    GSList *files = gdk_file_list_get_files(file_list);
    if (!files) return FALSE;

    int count = g_slist_length(files);
    if (count == 0) {
        g_slist_free(files);
        return FALSE;
    }

    PlaylistDropData *data = malloc(sizeof(PlaylistDropData));
    data->count = count;
    data->items = calloc(count, sizeof(PlaylistDropItem));

    int i = 0;
    for (GSList *l = files; l != NULL; l = l->next) {
        GFile *file = G_FILE(l->data);
        char *path = g_file_get_path(file);
        if (path) {
            strncpy(data->items[i].path, path, sizeof(data->items[i].path) - 1);
            g_free(path);
        }
        i++;
    }

    // FIX: Safely free the container list without destroying GTK's GFile objects
    g_slist_free(files);

    extern GtkWidget *lbl_status;
    extern GtkWidget *main_spinner;
    gtk_spinner_start(GTK_SPINNER(main_spinner));

    char msg[128];
    snprintf(msg, sizeof(msg), "Status: Inspecting %d file(s)...", count);
    gtk_label_set_text(GTK_LABEL(lbl_status), msg);

    // Spin up the worker thread
    GTask *task = g_task_new(NULL, NULL, playlist_drop_ready, NULL);
    g_task_set_task_data(task, data, free_playlist_drop_data);
    g_task_run_in_thread(task, playlist_drop_thread);
    g_object_unref(task);

    // INSTANT RETURN: Releases the Wayland DND proxy immediately, preventing the timeout crash
    return TRUE;
}

// --- PLAYLIST RENAMING ---
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
            // Update all configuration references to the new file path
            for (int i = 0; i < 10; i++) {
                if (g_strcmp0(ui_state.config.playlist_slots[i], ui_state.playlist_path) == 0) {
                    strncpy(ui_state.config.playlist_slots[i], new_path, sizeof(ui_state.config.playlist_slots[0]) - 1);
                }
            }
            strncpy(ui_state.playlist_path, new_path, sizeof(ui_state.playlist_path) - 1);
            strncpy(ui_state.config.last_playlist_path, new_path, sizeof(ui_state.config.last_playlist_path) - 1);

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
    if (strlen(ui_state.playlist_path) == 0) return;

    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Rename Playlist");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button))));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 300, 100);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 15);
    gtk_widget_set_margin_end(vbox, 15);
    gtk_widget_set_margin_top(vbox, 15);
    gtk_widget_set_margin_bottom(vbox, 15);

    char *basename = g_path_get_basename(ui_state.playlist_path);
    char *dot = strrchr(basename, '.');
    if (dot) *dot = '\0';

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

    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Enter new playlist name:"));
    gtk_box_append(GTK_BOX(vbox), entry);
    gtk_box_append(GTK_BOX(vbox), btn_confirm);

    gtk_window_set_child(GTK_WINDOW(dialog), vbox);
    gtk_window_present(GTK_WINDOW(dialog));
}

/**
 * @brief Creates and initializes the main GTK playlist widget structure.
 * @return A pointer to the created GtkWidget (scrolled window containing the list).
 */
GtkWidget* create_playlist_widget(void) {
    track_list_store = g_list_store_new(QJ_TYPE_TRACK);
    track_selection_model = GTK_MULTI_SELECTION(gtk_multi_selection_new(G_LIST_MODEL(track_list_store)));

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(on_setup_list_item), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(on_bind_list_item), NULL);

    track_list_view = gtk_list_view_new(GTK_SELECTION_MODEL(track_selection_model), factory);

    // FIX: Removed the duplicate "selection-changed" string
    g_signal_connect(track_selection_model, "selection-changed", G_CALLBACK(on_playlist_selection_changed), NULL);

    // RESTORED: Keyboard and Activation controllers that were accidentally dropped during the revert
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_playlist_key_pressed), NULL);
    gtk_widget_add_controller(track_list_view, key_ctrl);

    g_signal_connect(track_list_view, "activate", G_CALLBACK(on_track_activated), NULL);

    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled_window, TRUE);
    gtk_widget_set_valign(scrolled_window, GTK_ALIGN_FILL);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled_window), 250);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scrolled_window), FALSE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    GtkDropTarget *file_drop_target = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    g_signal_connect(file_drop_target, "drop", G_CALLBACK(on_playlist_file_drop), NULL);
    gtk_widget_add_controller(scrolled_window, GTK_EVENT_CONTROLLER(file_drop_target));

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), track_list_view);

    // Header Bar for Playlist
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_margin_bottom(header_box, 5);

    lbl_playlist_title = gtk_label_new("<b>Playlist</b>");
    gtk_label_set_use_markup(GTK_LABEL(lbl_playlist_title), TRUE);
    gtk_widget_set_hexpand(lbl_playlist_title, FALSE);
    gtk_widget_set_halign(lbl_playlist_title, GTK_ALIGN_FILL);
    gtk_label_set_xalign(GTK_LABEL(lbl_playlist_title), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(lbl_playlist_title), PANGO_ELLIPSIZE_END);

    // BUTTON 1: Rename the Playlist
    GtkWidget *btn_rename_playlist = gtk_button_new_from_icon_name("insert-text-symbolic");
    gtk_widget_set_tooltip_text(btn_rename_playlist, "Rename Active Playlist");
    gtk_widget_add_css_class(btn_rename_playlist, "flat");
    g_signal_connect(btn_rename_playlist, "clicked", G_CALLBACK(on_rename_playlist_clicked), NULL);

    // BUTTON 2: Rename the Track
    btn_rename = gtk_button_new_from_icon_name("document-edit-symbolic");
    gtk_widget_set_tooltip_text(btn_rename, "Rename Selected Track");
    gtk_widget_add_css_class(btn_rename, "flat");
    gtk_widget_set_sensitive(btn_rename, FALSE);
    g_signal_connect(btn_rename, "clicked", G_CALLBACK(on_rename_track_clicked), NULL);

    btn_delete = gtk_button_new_from_icon_name("edit-delete-symbolic");
    gtk_widget_set_tooltip_text(btn_delete, "Remove Selected Tracks (Del)");
    gtk_widget_add_css_class(btn_delete, "flat");
    gtk_widget_set_sensitive(btn_delete, FALSE);
    g_signal_connect(btn_delete, "clicked", G_CALLBACK(on_delete_selected_clicked), NULL);

    GtkWidget *btn_sort = gtk_button_new_from_icon_name("view-sort-descending-symbolic");
    gtk_widget_set_tooltip_text(btn_sort, "Sort Alphabetically");
    gtk_widget_add_css_class(btn_sort, "flat");
    g_signal_connect(btn_sort, "clicked", G_CALLBACK(on_sort_playlist_clicked), NULL);

    gtk_box_append(GTK_BOX(header_box), lbl_playlist_title);
    gtk_box_append(GTK_BOX(header_box), btn_rename_playlist);
    gtk_box_append(GTK_BOX(header_box), btn_rename);
    gtk_box_append(GTK_BOX(header_box), btn_delete);
    gtk_box_append(GTK_BOX(header_box), btn_sort);

    // Render the Car Radio Presets in Two Rows
    GtkWidget *preset_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_margin_bottom(preset_vbox, 5);

    GtkWidget *preset_box_1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_halign(preset_box_1, GTK_ALIGN_CENTER);
    GtkWidget *preset_box_2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_halign(preset_box_2, GTK_ALIGN_CENTER);

    for (int i = 0; i < 10; i++) {
        preset_btns[i] = gtk_button_new_with_label("-");
        gtk_widget_set_size_request(preset_btns[i], 30, 30);
        gtk_widget_add_css_class(preset_btns[i], "flat");

        // Native button click for standard drag-compatible activation
        g_signal_connect(preset_btns[i], "clicked", G_CALLBACK(on_preset_button_clicked), GINT_TO_POINTER(i));

        GtkGesture *click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0); // Allow all buttons
        gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(click), GTK_PHASE_CAPTURE);
        g_signal_connect(click, "pressed", G_CALLBACK(on_preset_click_pressed), GINT_TO_POINTER(i));
        gtk_widget_add_controller(preset_btns[i], GTK_EVENT_CONTROLLER(click));

        GtkDragSource *drag_src = gtk_drag_source_new();
        gtk_drag_source_set_actions(drag_src, GDK_ACTION_MOVE);
        g_signal_connect(drag_src, "prepare", G_CALLBACK(on_preset_drag_prepare), GINT_TO_POINTER(i));
        gtk_widget_add_controller(preset_btns[i], GTK_EVENT_CONTROLLER(drag_src));

        GtkDropTarget *drop_target = gtk_drop_target_new(G_TYPE_INT, GDK_ACTION_MOVE);
        g_signal_connect(drop_target, "drop", G_CALLBACK(on_preset_drop), GINT_TO_POINTER(i));
        gtk_widget_add_controller(preset_btns[i], GTK_EVENT_CONTROLLER(drop_target));

        if (i < 5) gtk_box_append(GTK_BOX(preset_box_1), preset_btns[i]);
        else gtk_box_append(GTK_BOX(preset_box_2), preset_btns[i]);
    }

    gtk_box_append(GTK_BOX(preset_vbox), preset_box_1);
    gtk_box_append(GTK_BOX(preset_vbox), preset_box_2);

    current_preset_slot = -1;
    for (int i = 0; i < 10; i++) {
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

    gtk_box_append(GTK_BOX(main_box), header_box);
    gtk_box_append(GTK_BOX(main_box), preset_vbox);
    gtk_box_append(GTK_BOX(main_box), scrolled_window);

    // Wrap the entire construct in a frame before handing it off to gui.c
    GtkWidget *frame = gtk_frame_new(NULL);

    // Explicitly claim the 340px width natively so it reaches the buttons
    gtk_widget_set_size_request(frame, 340, -1);

    gtk_widget_set_hexpand(frame, FALSE); // Stop propagation!
    gtk_widget_set_halign(frame, GTK_ALIGN_FILL);
    gtk_frame_set_child(GTK_FRAME(frame), main_box);

    // Initial load from disk
    FILE *f = fopen(ui_state.playlist_path, "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (strlen(line) == 0 || line[0] == '#') continue;
            char display_buf[512];
            bool is_native = extract_track_metadata(line, display_buf, sizeof(display_buf));
            QjTrack *trk = qj_track_new(line, display_buf, is_native);
            g_list_store_append(track_list_store, trk);
            g_object_unref(trk);
        }
        fclose(f);
    }

    update_playlist_title_ui(ui_state.playlist_path);

    return frame;
}
