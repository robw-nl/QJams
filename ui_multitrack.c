#include "ui_multitrack.h"
#include "ui_globals.h"
#include "ui_playlist.h"
#include "ui_waveforms.h"
#include "audio_engine.h"
#include "ui_dashboard.h"
#include <string.h>

extern _Atomic bool engine_is_playing;

static char user_track_names[MAX_TRACKS][64] = {0};
static char hidden_track_names[MAX_TRACKS][64] = {0}; // Stores custom names during undo
static bool names_initialized = false;

static GtkWidget *lbl_indicators[MAX_TRACKS];
static GtkWidget *name_editors[MAX_TRACKS];
static GtkWidget *btn_solos[MAX_TRACKS];
static GtkWidget *row_boxes[MAX_TRACKS];
static GtkWidget *spin_gains[MAX_TRACKS];
static GtkWidget *color_dots[MAX_TRACKS];

static void on_track_gain_changed(GtkSpinButton *spin_button, gpointer user_data) {
    int track_idx = GPOINTER_TO_INT(user_data);
    double db = gtk_spin_button_get_value(spin_button);
    float multiplier = (db <= -24.0) ? 0.0f : powf(10.0f, (float)(db / 20.0));
    set_multitrack_layer_gain(track_idx, multiplier);

    extern GtkWidget *waveform_area_bt;
    extern void invalidate_waveform_caches(void);
    invalidate_waveform_caches();
    if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
}

void update_multitrack_status_ui(void) {
    int current = atomic_load_explicit(&current_recording_track, memory_order_acquire);
    char buf[64];
    if (current == 0) {
        snprintf(buf, sizeof(buf), "Multi-Track Ready - Base Track");
    } else {
        snprintf(buf, sizeof(buf), "Multi-Track Ready - Overdub %d / %d", current, MAX_TRACKS - 1);
    }
    gtk_label_set_text(GTK_LABEL(lbl_multitrack_status), buf);

    size_t frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
    bool base_ready = (frames > 0 && current < MAX_TRACKS - 1);
    if (btn_multitrack_next) gtk_widget_set_sensitive(btn_multitrack_next, base_ready);

    refresh_multitrack_tracks_ui();
}

static void on_blank_canvas_confirm_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    GtkWidget *sw = GTK_WIDGET(user_data);
    int response = gtk_alert_dialog_choose_finish(alert, res, NULL);

    if (response == 0) {
        g_signal_handlers_block_by_func(sw, G_CALLBACK(on_blank_canvas_toggled), NULL);
        gtk_switch_set_active(GTK_SWITCH(sw), TRUE);
        g_signal_handlers_unblock_by_func(sw, G_CALLBACK(on_blank_canvas_toggled), NULL);

        ui_state.config.multitrack_blank_canvas = 1;
        save_qjams_config(ui_state.config_path, &ui_state.config);

        ui_state.selected_track_path[0] = '\0';
        update_playlist_toggle_state();

        int loop_min = ui_state.config.multitrack_duration_min > 0 ? ui_state.config.multitrack_duration_min : 5;
        init_empty_loop_canvas(loop_min * 60);
        zoom_multiplier = 1.0;
        atomic_store_explicit(&active_track_count, 1, memory_order_release);
        atomic_store_explicit(&current_recording_track, 0, memory_order_release);

        char track_lbl[128];
        snprintf(track_lbl, sizeof(track_lbl), "Track: Blank Loop Canvas [%02d:00]", loop_min);
        gtk_label_set_text(GTK_LABEL(lbl_track), track_lbl);

        ui_state.session_is_dirty = false;
        invalidate_waveform_caches();
        gtk_widget_queue_draw(waveform_area_bt);
        gtk_widget_queue_draw(waveform_area_input);
        update_multitrack_status_ui();
        gtk_widget_set_sensitive(btn_play, TRUE);
        gtk_widget_set_sensitive(btn_record, TRUE);
        update_zoom_button_label_to_length();
    }
}

void on_blank_canvas_toggled(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)pspec; (void)user_data;
    gboolean active = gtk_switch_get_active(GTK_SWITCH(gobject));

    if (active && pristine_frames > 0 && ui_state.session_is_dirty) {
        g_signal_handlers_block_by_func(gobject, G_CALLBACK(on_blank_canvas_toggled), NULL);
        gtk_switch_set_active(GTK_SWITCH(gobject), FALSE);
        g_signal_handlers_unblock_by_func(gobject, G_CALLBACK(on_blank_canvas_toggled), NULL);

        GtkAlertDialog *alert = gtk_alert_dialog_new("Enter Blank Canvas Mode?");
        gtk_alert_dialog_set_detail(alert, "This will discard the currently loaded track and any recorded overdubs. Continue?");
        const char *buttons[] = { "Discard & Continue", "Cancel", NULL };
        gtk_alert_dialog_set_buttons(alert, buttons);
        gtk_alert_dialog_set_cancel_button(alert, 1);
        gtk_alert_dialog_set_default_button(alert, 0);

        gtk_alert_dialog_choose(alert, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(gobject))), NULL, on_blank_canvas_confirm_response, gobject);
        g_object_unref(alert);
        return;
    }

    ui_state.config.multitrack_blank_canvas = active ? 1 : 0;
    save_qjams_config(ui_state.config_path, &ui_state.config);

    if (active) {
        ui_state.selected_track_path[0] = '\0';
        update_playlist_toggle_state();

        int loop_min = ui_state.config.multitrack_duration_min > 0 ? ui_state.config.multitrack_duration_min : 5;
        init_empty_loop_canvas(loop_min * 60);
        zoom_multiplier = 1.0;
        atomic_store_explicit(&active_track_count, 1, memory_order_release);
        atomic_store_explicit(&current_recording_track, 0, memory_order_release);

        char track_lbl[128];
        snprintf(track_lbl, sizeof(track_lbl), "Track: Blank Loop Canvas [%02d:00]", loop_min);
        gtk_label_set_text(GTK_LABEL(lbl_track), track_lbl);

        ui_state.session_is_dirty = false;
        invalidate_waveform_caches();
        gtk_widget_queue_draw(waveform_area_bt);
        gtk_widget_queue_draw(waveform_area_input);
        update_multitrack_status_ui();
        gtk_widget_set_sensitive(btn_play, TRUE);
        gtk_widget_set_sensitive(btn_record, TRUE);
        update_zoom_button_label_to_length();
    } else {
        if (strlen(ui_state.selected_track_path) == 0) {
            prepare_engine_for_new_track();
            gtk_widget_set_sensitive(btn_play, FALSE);
            gtk_widget_set_sensitive(btn_record, FALSE);
            gtk_label_set_text(GTK_LABEL(lbl_track), "Track: None Selected");
            invalidate_waveform_caches();
            if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
            if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);
        }
    }
}

void reset_multitrack_ui_states(void) {
    for (int i = 0; i < MAX_TRACKS; i++) {
        atomic_store_explicit(&track_is_soloed[i], false, memory_order_release);
        snprintf(user_track_names[i], 64, "Track %d", i + 1);
    }
    refresh_multitrack_tracks_ui();
}

void ui_multitrack_init(GtkBuilder *b_multi, GtkBuilder *b_stack) {
    mode_stack = GTK_WIDGET(gtk_builder_get_object(b_stack, "mode_stack"));
    // Ensure you place lbl_multitrack_status inside multitrack.ui, or move to b_cmd if it sits with the buttons
    lbl_multitrack_status = GTK_WIDGET(gtk_builder_get_object(b_multi, "lbl_multitrack_status"));
}

static void init_default_track_names(void) {
    if (names_initialized) return;
    for (int i = 0; i < MAX_TRACKS; i++) {
        snprintf(user_track_names[i], 64, "Track %d", i + 1);
    }
    names_initialized = true;
}

// Forward declaration required for C's top-to-bottom compilation
static void on_solo_track_toggled(GtkToggleButton *btn, gpointer user_data);

/**
 * @brief Gets the custom name for a specific multitrack layer.
 * @param track_idx The index of the layer.
 * @param out_name Buffer to store the extracted name.
 */
void get_multitrack_track_name(int track_idx, char *out_name) {
    if (track_idx >= 0 && track_idx < MAX_TRACKS) {
        strncpy(out_name, user_track_names[track_idx], 64);
    } else {
        out_name[0] = '\0';
    }
}

/**
 * @brief Sets a custom name for a specific multitrack layer.
 * @param track_idx The index of the layer.
 * @param name The new name string.
 */
void set_multitrack_track_name(int track_idx, const char *name) {
    if (track_idx >= 0 && track_idx < MAX_TRACKS) {
        strncpy(user_track_names[track_idx], name, 63);
        user_track_names[track_idx][63] = '\0';
    }
}

/**
 * @brief Refreshes the visual state (names, solo toggles, indicators) of all multitrack layer rows.
 */
void refresh_multitrack_tracks_ui(void) {
    init_default_track_names();
    int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);

    for (int i = 0; i < MAX_TRACKS; i++) {
        if (!lbl_indicators[i]) continue;
        gtk_label_set_text(GTK_LABEL(lbl_indicators[i]), (i == rec_track) ? "🔴" : "  ");
        if (i == rec_track) gtk_widget_add_css_class(row_boxes[i], "selected");
        else gtk_widget_remove_css_class(row_boxes[i], "selected");

        gtk_label_set_text(GTK_LABEL(name_editors[i]), user_track_names[i]);

        if (color_dots[i]) gtk_widget_queue_draw(color_dots[i]);

        g_signal_handlers_block_by_func(btn_solos[i], G_CALLBACK(on_solo_track_toggled), GINT_TO_POINTER(i));
        bool is_soloed = atomic_load_explicit(&track_is_soloed[i], memory_order_acquire);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn_solos[i]), is_soloed);
        g_signal_handlers_unblock_by_func(btn_solos[i], G_CALLBACK(on_solo_track_toggled), GINT_TO_POINTER(i));

        if (spin_gains[i]) {
            float current_gain = atomic_load_explicit(&multitrack_track_gains[i], memory_order_relaxed);
            float loaded_db = (current_gain <= 0.001f) ? -24.0f : 20.0f * log10f(current_gain);
            g_signal_handlers_block_by_func(spin_gains[i], G_CALLBACK(on_track_gain_changed), GINT_TO_POINTER(i));
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_gains[i]), loaded_db);
            g_signal_handlers_unblock_by_func(spin_gains[i], G_CALLBACK(on_track_gain_changed), GINT_TO_POINTER(i));
        }
    }
}

static void on_solo_track_toggled(GtkToggleButton *btn, gpointer user_data) {
    int track_idx = GPOINTER_TO_INT(user_data);
    bool is_soloed = gtk_toggle_button_get_active(btn);

    // Strictly update the mixing bus state without touching the transport controls
    atomic_store_explicit(&track_is_soloed[track_idx], is_soloed, memory_order_release);

    // Force a UI redraw so the canvas instantly hides muted tracks while stopped
    extern void invalidate_waveform_caches(void);
    extern GtkWidget *waveform_area_bt;
    invalidate_waveform_caches();
    if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
}

typedef struct {
    int track_idx;
    GtkWidget *entry;
    GtkWidget *dialog;
} RenameTrackData;

static void on_rename_track_confirm(GtkButton *btn, gpointer user_data) {
    (void)btn;
    RenameTrackData *rd = (RenameTrackData *)user_data;
    const char *new_name = gtk_editable_get_text(GTK_EDITABLE(rd->entry));

    if (strlen(new_name) > 0) {
        set_multitrack_track_name(rd->track_idx, new_name);
        refresh_multitrack_tracks_ui();
    }

    // FIX: Extract the window pointer safely, then destroy.
    // GTK will naturally call free(rd) via the finalizer.
    GtkWidget *win = rd->dialog;
    gtk_window_destroy(GTK_WINDOW(win));
}

static void open_rename_track_dialog(int track_idx, GtkWidget *parent_window) {
    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Rename Track");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(parent_window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 300, 100);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 15);
    gtk_widget_set_margin_end(vbox, 15);
    gtk_widget_set_margin_top(vbox, 15);
    gtk_widget_set_margin_bottom(vbox, 15);

    char current_name[64];
    get_multitrack_track_name(track_idx, current_name);

    GtkWidget *entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), current_name);

    GtkWidget *btn_confirm = gtk_button_new_with_label("Rename Track");
    gtk_widget_set_halign(btn_confirm, GTK_ALIGN_END);

    RenameTrackData *rd = malloc(sizeof(RenameTrackData));
    rd->track_idx = track_idx;
    rd->entry = entry;
    rd->dialog = dialog;

    // Bind the struct explicitly to the GObject finalizer to guarantee leak-free cleanup
    g_object_set_data_full(G_OBJECT(dialog), "rename_data", rd, free);

    g_signal_connect(btn_confirm, "clicked", G_CALLBACK(on_rename_track_confirm), rd);

    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Enter new track name:"));
    gtk_box_append(GTK_BOX(vbox), entry);
    gtk_box_append(GTK_BOX(vbox), btn_confirm);

    gtk_window_set_child(GTK_WINDOW(dialog), vbox);
    gtk_window_present(GTK_WINDOW(dialog));
}

static void on_rename_clicked(GtkButton *btn, gpointer user_data) {
    int track_idx = GPOINTER_TO_INT(user_data);
    open_rename_track_dialog(track_idx, GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(btn))));
}

static void on_row_clicked(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)gesture; (void)n_press; (void)x; (void)y;
    int track_idx = GPOINTER_TO_INT(user_data);
    int current = atomic_load_explicit(&current_recording_track, memory_order_acquire);
    if (current != track_idx && track_idx >= 0 && track_idx < MAX_TRACKS) {
        atomic_store_explicit(&current_recording_track, track_idx, memory_order_release);
        int active = atomic_load_explicit(&active_track_count, memory_order_acquire);
        if (track_idx >= active) atomic_store_explicit(&active_track_count, track_idx + 1, memory_order_release);
        update_multitrack_status_ui();
    }
}

static GdkContentProvider* on_track_drag_prepare(GtkDragSource *source, double x, double y, gpointer user_data) {
    (void)source; (void)x; (void)y;
    int track_idx = GPOINTER_TO_INT(user_data);
    GValue value = G_VALUE_INIT;
    g_value_init(&value, G_TYPE_INT);
    g_value_set_int(&value, track_idx);
    return gdk_content_provider_new_for_value(&value);
}

static gboolean on_track_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data) {
    (void)target; (void)x; (void)y;
    int src_idx = g_value_get_int(value);
    int dest_idx = GPOINTER_TO_INT(user_data);

    if (src_idx == dest_idx || src_idx < 0 || src_idx >= MAX_TRACKS || dest_idx < 0 || dest_idx >= MAX_TRACKS) return FALSE;

    // 1. Detach the DSP engine ONCE for the entire drag operation to prevent race conditions and timeouts
    if (!await_rt_thread_detach()) return FALSE;

    int current_rec = atomic_load_explicit(&current_recording_track, memory_order_acquire);
    int step = (src_idx < dest_idx) ? 1 : -1;

    for (int i = src_idx; i != dest_idx; i += step) {
        int next = i + step;

        // 2. Swap Audio Pointers explicitly inside this unified barrier
        float *tmp_multi = multitrack_tracks[i];
        multitrack_tracks[i] = multitrack_tracks[next];
        multitrack_tracks[next] = tmp_multi;

        float *tmp_undo = undo_tracks[i];
        undo_tracks[i] = undo_tracks[next];
        undo_tracks[next] = tmp_undo;

        // 3. Swap Names
        char temp_name[64];
        strncpy(temp_name, user_track_names[i], 64);
        strncpy(user_track_names[i], user_track_names[next], 64);
        strncpy(user_track_names[next], temp_name, 64);

        char temp_hidden[64];
        strncpy(temp_hidden, hidden_track_names[i], 64);
        strncpy(hidden_track_names[i], hidden_track_names[next], 64);
        strncpy(hidden_track_names[next], temp_hidden, 64);

        // 4. Swap Solo States
        bool solo_i = atomic_load_explicit(&track_is_soloed[i], memory_order_acquire);
        bool solo_next = atomic_load_explicit(&track_is_soloed[next], memory_order_acquire);
        atomic_store_explicit(&track_is_soloed[i], solo_next, memory_order_release);
        atomic_store_explicit(&track_is_soloed[next], solo_i, memory_order_release);

        // 5. Swap Clip Gains
        float gain_i = atomic_load_explicit(&multitrack_track_gains[i], memory_order_acquire);
        float gain_next = atomic_load_explicit(&multitrack_track_gains[next], memory_order_acquire);
        atomic_store_explicit(&multitrack_track_gains[i], gain_next, memory_order_release);
        atomic_store_explicit(&multitrack_track_gains[next], gain_i, memory_order_release);

        if (current_rec == i) current_rec = next;
        else if (current_rec == next) current_rec = i;
    }

    atomic_store_explicit(&current_recording_track, current_rec, memory_order_release);

    // 6. Dynamically expand active_track_count so the renderer loops far enough to draw the dragged track
    int current_active = atomic_load_explicit(&active_track_count, memory_order_acquire);
    if (dest_idx >= current_active) {
        atomic_store_explicit(&active_track_count, dest_idx + 1, memory_order_release);
    }

    // 7. Safely resume the DSP engine
    resume_rt_thread();

    invalidate_waveform_caches();
    extern GtkWidget *waveform_area_bt;
    extern GtkWidget *waveform_area_input;
    if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
    if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);
    refresh_multitrack_tracks_ui();
    return TRUE;
}

// --- ASYNC STEM IMPORT PIPELINE ---
typedef struct {
    char *filepath;
    int track_idx;
} ImportData;

static void free_import_data(gpointer data) {
    ImportData *id = (ImportData *)data;
    g_free(id->filepath);
    g_free(id);
}

// Statically allocated global mutex to serialize concurrent GTask imports
static GMutex import_mutex;

static void import_stem_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)source_object; (void)cancellable;
    ImportData *data = (ImportData *)task_data;

    // PRE-CHECK: If the user hit reset and wiped the canvas, abort the queue early
    if (pristine_frames == 0) {
        g_task_return_int(task, -1);
        return;
    }

    // Serialize asynchronous imports to prevent double-frees on the undo buffers
    g_mutex_lock(&import_mutex);

    // DOUBLE-CHECK: Ensure canvas wasn't wiped while we were waiting for the lock
    if (pristine_frames == 0) {
        g_mutex_unlock(&import_mutex);
        g_task_return_int(task, -1);
        return;
    }

    int result = import_to_layer(data->filepath, data->track_idx);
    g_mutex_unlock(&import_mutex);

    g_task_return_int(task, result);
}

static gboolean reset_status_label_deferred(gpointer user_data) {
    (void)user_data;
    extern GtkWidget *lbl_status;

    // Use an explicit neutral span to force GTK/Pango to drop the cached red attributes
    gtk_label_set_markup(GTK_LABEL(lbl_status), "<span>Status: Ready</span>");
    return G_SOURCE_REMOVE;
}

static void sanitize_track_name(const char *filename, char *out_name, size_t max_len) {
    size_t i = 0, j = 0;
    bool last_space = false;
    while (filename[i] != '\0' && filename[i] != '.' && j < max_len - 1) {
        if (g_ascii_isalpha(filename[i])) {
            out_name[j++] = filename[i];
            last_space = false;
        } else if ((filename[i] == ' ' || filename[i] == '_' || filename[i] == '-') && !last_space && j > 0) {
            out_name[j++] = ' ';
            last_space = true;
        }
        i++;
    }
    if (j > 0 && out_name[j-1] == ' ') j--;
    out_name[j] = '\0';
    if (j == 0) snprintf(out_name, max_len, "Track");
}

static void import_stem_ready(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object;
    int track_idx = GPOINTER_TO_INT(user_data);
    GError *error = NULL;
    int result = g_task_propagate_int(G_TASK(res), &error);

    ImportData *data = (ImportData *)g_task_get_task_data(G_TASK(res));
    gtk_spinner_stop(GTK_SPINNER(main_spinner));

    if (result == 0 || result == 1) {
        ui_state.session_is_dirty = true;

        // FIX: Register Base Track imports for the '+' button
        if (track_idx == 0) {
            strncpy(ui_state.selected_track_path, data->filepath, sizeof(ui_state.selected_track_path) - 1);
            ui_state.selected_track_path[sizeof(ui_state.selected_track_path) - 1] = '\0';
            update_playlist_toggle_state();
        }

        char *basename = g_path_get_basename(data->filepath);
        char clean_name[64];
        sanitize_track_name(basename, clean_name, sizeof(clean_name));

        set_multitrack_track_name(track_idx, clean_name);
        g_free(basename);

        refresh_multitrack_tracks_ui();
        invalidate_waveform_caches();
        gtk_widget_queue_draw(waveform_area_bt);
        gtk_widget_queue_draw(waveform_area_input);

        if (result == 1) {
            gtk_label_set_markup(GTK_LABEL(lbl_status), "<span foreground='#ff4444' weight='heavy'>WARNING: Dropped track was truncated to match canvas length!</span>");
            g_timeout_add(3000, reset_status_label_deferred, NULL);
        } else {
            gtk_label_set_markup(GTK_LABEL(lbl_status), "<span>Status: Stem Imported Successfully</span>");
        }
    } else if (result == -2) {
        gtk_label_set_markup(GTK_LABEL(lbl_status), "<span foreground='#ff4444'><b>Status: Insufficient RAM for Stem</b></span>");
    } else if (result == -3) {
        gtk_label_set_markup(GTK_LABEL(lbl_status), "<span foreground='#ff4444'><b>Status: Stem Import Failed (Corrupt or Empty Media)</b></span>");
    } else {
        gtk_label_set_markup(GTK_LABEL(lbl_status), "<span foreground='#ff4444'><b>Status: Stem Import Failed</b></span>");
    }
}

/**
 * @brief Handles track-level destructive cuts, V-fades, and blend crossfades.
 * Reads modifier keys: Click (Cut), Ctrl (Smart V-Fade), Shift (Blend Crossfade).
 * Invalidates peak caches and forces canvas redrawing upon completion.
 * @param gesture The click gesture controller.
 * @param n_press Press count.
 * @param x Coordinate x.
 * @param y Coordinate y.
 * @param user_data Target layer index cast to gpointer.
 */
static void on_cut_track_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press; (void)x; (void)y;
    int track_idx = GPOINTER_TO_INT(user_data);
    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));

    bool has_ctrl = (state & GDK_CONTROL_MASK) != 0;
    bool has_shift = (state & GDK_SHIFT_MASK) != 0;

    if (has_shift) {
        extern void apply_blend_fade_to_track(int);
        apply_blend_fade_to_track(track_idx);
    } else if (has_ctrl) {
        extern void apply_smart_fade_to_track(int);
        apply_smart_fade_to_track(track_idx);
    } else {
        extern void cut_multitrack_track_selection(int);
        cut_multitrack_track_selection(track_idx);
    }

    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    refresh_multitrack_tracks_ui();

    extern GtkWidget *waveform_area_bt;
    extern GtkWidget *waveform_area_input;
    extern void invalidate_waveform_caches(void);
    invalidate_waveform_caches();
    if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
    if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);
}

/**
 * @brief Async dialog response handler for full destructive track wipe confirmation.
 * @param source_object The alert dialog instance.
 * @param res Async result object.
 * @param user_data Layer index cast to gpointer.
 */
static void on_wipe_track_confirmed(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    int track_idx = GPOINTER_TO_INT(user_data);
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    GError *error = NULL;
    int response = gtk_alert_dialog_choose_finish(alert, res, &error);

    if (error) {
        g_error_free(error);
        return;
    }

    // Response 0 is "Wipe Track"
    if (response == 0) {
        extern void clear_multitrack_track(int);
        clear_multitrack_track(track_idx);

        char default_name[64];
        snprintf(default_name, sizeof(default_name), "Track %d", track_idx + 1);
        set_multitrack_track_name(track_idx, default_name);

        refresh_multitrack_tracks_ui();

        extern GtkWidget *waveform_area_bt;
        extern GtkWidget *waveform_area_input;
        extern void invalidate_waveform_caches(void);
        invalidate_waveform_caches();
        if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
        if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);
    }
}

/**
 * @brief Handles track-level undo operations and routes to wipe confirmation when empty.
 * Restores previous audio snapshot or displays warning dialog if buffer is exhausted.
 * @param gesture The click gesture controller.
 * @param n_press Press count.
 * @param x Coordinate x.
 * @param y Coordinate y.
 * @param user_data Target layer index cast to gpointer.
 */
static void on_undo_track_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press; (void)x; (void)y;
    int track_idx = GPOINTER_TO_INT(user_data);
    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
    bool has_ctrl = (state & GDK_CONTROL_MASK) != 0;

    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);

    extern _Atomic bool track_has_undo[MAX_TRACKS];
    bool has_undo = atomic_load_explicit(&track_has_undo[track_idx], memory_order_acquire);

    if (has_ctrl || !has_undo) {
        GtkWindow *window = GTK_WINDOW(gtk_widget_get_ancestor(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture)), GTK_TYPE_WINDOW));
        char title[64];
        snprintf(title, sizeof(title), "Wipe Track %d?", track_idx + 1);
        GtkAlertDialog *alert = gtk_alert_dialog_new("%s", title);
        gtk_alert_dialog_set_detail(alert, "This will permanently delete all audio on this track. This action cannot be undone.");

        const char *buttons[] = {"Wipe Track", "Cancel", NULL};
        gtk_alert_dialog_set_buttons(alert, buttons);
        gtk_alert_dialog_set_cancel_button(alert, 1);
        gtk_alert_dialog_set_default_button(alert, 0);

        gtk_alert_dialog_choose(alert, window, NULL, on_wipe_track_confirmed, GINT_TO_POINTER(track_idx));
    } else {
        extern void undo_track_edit(int);
        undo_track_edit(track_idx);
        refresh_multitrack_tracks_ui();

        extern GtkWidget *waveform_area_bt;
        extern GtkWidget *waveform_area_input;
        extern void invalidate_waveform_caches(void);
        invalidate_waveform_caches();
        if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
        if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);
    }
}

static gboolean on_track_file_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data) {
    (void)target; (void)x; (void)y;
    int track_idx = GPOINTER_TO_INT(user_data);

    if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        GSList *files = gdk_file_list_get_files(g_value_get_boxed(value));
        if (files && files->data) {
            GFile *file = G_FILE(files->data);
            char *path = g_file_get_path(file);
            if (path) {
                multitrack_import_file_async(path, track_idx);
                g_free(path);
                return TRUE;
            }
        }
    } else if (G_VALUE_HOLDS(value, QJ_TYPE_TRACK)) {
        QjTrack *track = g_value_get_object(value);
        if (track && track->filepath) {
            multitrack_import_file_async(track->filepath, track_idx);
            return TRUE;
        }
    }
    return FALSE;
}

GtkWidget* create_multitrack_tracks_widget(void) {
    init_default_track_names();
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(main_box, TRUE);
    gtk_widget_set_hexpand(main_box, FALSE);
    gtk_widget_set_halign(main_box, GTK_ALIGN_FILL);
    gtk_widget_add_css_class(main_box, "multitrack-canvas");

    for (int i = 0; i < MAX_TRACKS; i++) {
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        row_boxes[i] = hbox;
        gtk_widget_add_css_class(hbox, "multitrack-row");
        gtk_widget_set_margin_start(hbox, 5);
        gtk_widget_set_margin_end(hbox, 5);

        GtkGesture *click_gesture = gtk_gesture_click_new();
        g_signal_connect(click_gesture, "pressed", G_CALLBACK(on_row_clicked), GINT_TO_POINTER(i));
        gtk_widget_add_controller(hbox, GTK_EVENT_CONTROLLER(click_gesture));

        GtkWidget *drag_icon = gtk_image_new_from_icon_name("list-drag-handle-symbolic");
        gtk_widget_set_opacity(drag_icon, 0.5);

        GtkDragSource *drag_source = gtk_drag_source_new();
        gtk_drag_source_set_actions(drag_source, GDK_ACTION_MOVE);
        g_signal_connect(drag_source, "prepare", G_CALLBACK(on_track_drag_prepare), GINT_TO_POINTER(i));
        gtk_widget_add_controller(drag_icon, GTK_EVENT_CONTROLLER(drag_source));

        GtkDropTarget *drop_target = gtk_drop_target_new(G_TYPE_INT, GDK_ACTION_MOVE);
        g_signal_connect(drop_target, "drop", G_CALLBACK(on_track_drop), GINT_TO_POINTER(i));
        gtk_widget_add_controller(hbox, GTK_EVENT_CONTROLLER(drop_target));

        // --- NEW: PHASE 4 DUAL-FORMAT FILE DROP FOR STEM IMPORTS ---
        GType file_drop_types[] = { GDK_TYPE_FILE_LIST, QJ_TYPE_TRACK };
        GtkDropTarget *file_drop_target = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY | GDK_ACTION_MOVE);
        gtk_drop_target_set_gtypes(file_drop_target, file_drop_types, 2);
        g_signal_connect(file_drop_target, "drop", G_CALLBACK(on_track_file_drop), GINT_TO_POINTER(i));
        gtk_widget_add_controller(hbox, GTK_EVENT_CONTROLLER(file_drop_target));
        // ----------------------------------------------------------

        lbl_indicators[i] = gtk_label_new("  ");
        gtk_widget_set_margin_end(lbl_indicators[i], 5);

        name_editors[i] = gtk_label_new(user_track_names[i]);
        gtk_widget_set_halign(name_editors[i], GTK_ALIGN_START);
        gtk_widget_set_hexpand(name_editors[i], TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(name_editors[i]), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(name_editors[i]), 1); // Kill Natural Size Leak

        GtkWidget *color_dot = gtk_drawing_area_new();
        color_dots[i] = color_dot;
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(color_dot), 14);
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(color_dot), 14);
        gtk_widget_set_valign(color_dot, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_end(color_dot, 5);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(color_dot), on_draw_track_color_dot, GINT_TO_POINTER(i), NULL);

        GtkWidget *btn_rename = gtk_button_new_from_icon_name("document-edit-symbolic");
        gtk_widget_set_tooltip_text(btn_rename, "Rename Track (F2)");
        gtk_widget_add_css_class(btn_rename, "flat");
        g_signal_connect(btn_rename, "clicked", G_CALLBACK(on_rename_clicked), GINT_TO_POINTER(i));

        /** Read directly from the audio engine instead of the persistent config */
        float current_gain = atomic_load_explicit(&multitrack_track_gains[i], memory_order_relaxed);
        float loaded_db = (current_gain <= 0.001f) ? -24.0f : 20.0f * log10f(current_gain);

        GtkAdjustment *adj = gtk_adjustment_new(loaded_db, -24.0, 24.0, 1.0, 5.0, 0.0);
        spin_gains[i] = gtk_spin_button_new(adj, 1, 0);

        gtk_widget_add_css_class(spin_gains[i], "compact-spin");
        gtk_widget_set_hexpand(spin_gains[i], FALSE);
        gtk_widget_set_size_request(spin_gains[i], 50, -1);
        gtk_spin_button_set_climb_rate(GTK_SPIN_BUTTON(spin_gains[i]), 1.0);
        gtk_widget_set_tooltip_text(spin_gains[i], "Track Level (Non-Destructive, Post-Buffer)");
        g_signal_connect(spin_gains[i], "value-changed", G_CALLBACK(on_track_gain_changed), GINT_TO_POINTER(i));

        btn_solos[i] = gtk_toggle_button_new();
        gtk_button_set_icon_name(GTK_BUTTON(btn_solos[i]), "audio-headphones-symbolic");
        gtk_widget_set_tooltip_text(btn_solos[i], "Solo this track");
        g_signal_connect(btn_solos[i], "toggled", G_CALLBACK(on_solo_track_toggled), GINT_TO_POINTER(i));

        GtkWidget *btn_undo = gtk_button_new_from_icon_name("edit-undo-symbolic");
        gtk_widget_set_tooltip_text(btn_undo, "Undo / Redo Audio (Ctrl+Click: Full Track Wipe)");
        GtkGesture *undo_click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(undo_click), GDK_BUTTON_PRIMARY);
        g_signal_connect(undo_click, "pressed", G_CALLBACK(on_undo_track_pressed), GINT_TO_POINTER(i));
        gtk_widget_add_controller(btn_undo, GTK_EVENT_CONTROLLER(undo_click));

        GtkWidget *btn_cut = gtk_button_new_from_icon_name("edit-cut-symbolic");
        gtk_widget_set_tooltip_text(btn_cut, "Cut (Click) | V-Fade (Ctrl) | Blend (Shift)");
        gtk_widget_add_css_class(btn_cut, "flat");

        GtkGesture *cut_click = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(cut_click), GDK_BUTTON_PRIMARY);
        g_signal_connect(cut_click, "pressed", G_CALLBACK(on_cut_track_pressed), GINT_TO_POINTER(i));
        gtk_widget_add_controller(btn_cut, GTK_EVENT_CONTROLLER(cut_click));

        gtk_box_append(GTK_BOX(hbox), drag_icon);
        gtk_box_append(GTK_BOX(hbox), lbl_indicators[i]);
        gtk_box_append(GTK_BOX(hbox), name_editors[i]);
        gtk_box_append(GTK_BOX(hbox), color_dot);
        gtk_box_append(GTK_BOX(hbox), btn_rename);
        gtk_box_append(GTK_BOX(hbox), spin_gains[i]);
        gtk_box_append(GTK_BOX(hbox), btn_solos[i]);
        gtk_box_append(GTK_BOX(hbox), btn_cut);
        gtk_box_append(GTK_BOX(hbox), btn_undo);

        gtk_box_append(GTK_BOX(main_box), hbox);
    }
    refresh_multitrack_tracks_ui();
    return main_box;
}

void multitrack_start_rename(void) {
    int selected_idx = atomic_load_explicit(&current_recording_track, memory_order_acquire);
    if (selected_idx >= 0 && selected_idx < MAX_TRACKS) {
        extern GtkWidget *lbl_multitrack_status;
        open_rename_track_dialog(selected_idx, GTK_WIDGET(gtk_widget_get_root(lbl_multitrack_status)));
    }
}

void multitrack_import_file_async(const char *filepath, int track_idx) {
    extern GtkWidget *lbl_status;
    extern GtkWidget *main_spinner;

    gtk_spinner_start(GTK_SPINNER(main_spinner));
    gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Importing stem...");

    ImportData *data = g_new(ImportData, 1);
    data->filepath = g_strdup(filepath);
    data->track_idx = track_idx;

    GTask *task = g_task_new(NULL, NULL, import_stem_ready, GINT_TO_POINTER(track_idx));
    g_task_set_task_data(task, data, free_import_data);
    g_task_run_in_thread(task, import_stem_thread);
    g_object_unref(task);
}
