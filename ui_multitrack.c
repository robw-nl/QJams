#include "ui_multitrack.h"
#include "ui_globals.h"
#include "ui_playlist.h"
#include "ui_waveforms.h"
#include "audio_engine.h"
#include "ui_dashboard.h"
#include <string.h>

extern _Atomic bool engine_is_playing;

static char user_track_names[MAX_TRACKS][64] = {0};
static char hidden_track_names[MAX_TRACKS][64] = {0};
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

    mark_session_dirty();

    extern GtkWidget *waveform_area_bt;
    extern void invalidate_waveform_caches(void);
    invalidate_waveform_caches();
    if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
}

/**
 * @brief Updates the multitrack header label to reflect the active session state.
 * @note Safety [Moderate]: Memory allocated by g_path_get_basename must be explicitly freed to prevent UI leaks.
 */
void update_multitrack_status_ui(void) {
    char buf[256];

    if (ui_state.is_existing_session && strlen(ui_state.selected_track_path) > 0) {
        gchar *basename = g_path_get_basename(ui_state.selected_track_path);
        char *dot = strrchr(basename, '.');
        if (dot) *dot = '\0';

        snprintf(buf, sizeof(buf), "Active Session: %s%s", basename, is_session_dirty() ? " *" : "");
        g_free(basename);
    } else {
        snprintf(buf, sizeof(buf), "Unsaved Session");
    }

    extern GtkWidget *lbl_multitrack_status;
    if (lbl_multitrack_status) gtk_label_set_text(GTK_LABEL(lbl_multitrack_status), buf);

    extern void refresh_track_time_display(void);
    refresh_track_time_display();

    int current = atomic_load_explicit(&current_recording_track, memory_order_acquire);
    size_t bt_frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
    bool base_ready = (bt_frames > 0 && current < MAX_TRACKS - 1);
    extern GtkWidget *btn_multitrack_next;
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

        abort_active_video_preview(false);

        int canvas_limit_min = ui_state.config.default_canvas_limit_min > 0 ? ui_state.config.default_canvas_limit_min : 5;
        init_empty_loop_canvas(canvas_limit_min * 60);
        zoom_multiplier = 1.0;
        atomic_store_explicit(&active_track_count, 1, memory_order_release);
        atomic_store_explicit(&current_recording_track, 0, memory_order_release);

        char track_lbl[128];
        snprintf(track_lbl, sizeof(track_lbl), "Track: Blank Loop Canvas [%02d:00 limit]", canvas_limit_min);
        gtk_label_set_text(GTK_LABEL(lbl_track), track_lbl);

        mark_session_saved();
        invalidate_waveform_caches();
        gtk_widget_queue_draw(waveform_area_bt);
        gtk_widget_queue_draw(waveform_area_input);
        update_multitrack_status_ui();
        gtk_widget_set_sensitive(btn_play, FALSE);
        gtk_widget_set_sensitive(btn_record, TRUE);
        update_zoom_button_label_to_length();
    }
}

void on_blank_canvas_toggled(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)pspec; (void)user_data;
    gboolean active = gtk_switch_get_active(GTK_SWITCH(gobject));

    if (active && pristine_frames > 0 && is_session_dirty()) {
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

        abort_active_video_preview(false);

        int canvas_limit_min = ui_state.config.default_canvas_limit_min > 0 ? ui_state.config.default_canvas_limit_min : 5;
        init_empty_loop_canvas(canvas_limit_min * 60);
        zoom_multiplier = 1.0;
        atomic_store_explicit(&active_track_count, 1, memory_order_release);
        atomic_store_explicit(&current_recording_track, 0, memory_order_release);

        set_multitrack_track_name(0, "Blank Canvas");

        mark_session_saved();
        extern void invalidate_waveform_caches(void);
        invalidate_waveform_caches();
        extern GtkWidget *waveform_area_bt;
        extern GtkWidget *waveform_area_input;
        if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
        if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);

        update_multitrack_status_ui();

        gtk_widget_set_sensitive(btn_play, FALSE);
        gtk_widget_set_sensitive(btn_record, TRUE);
        extern void update_zoom_button_label_to_length(void);
        update_zoom_button_label_to_length();
    } else {
        if (strlen(ui_state.selected_track_path) == 0) {
            extern void prepare_engine_for_new_track(void);
            prepare_engine_for_new_track();
            gtk_widget_set_sensitive(btn_play, FALSE);
            gtk_widget_set_sensitive(btn_record, FALSE);
            extern GtkWidget *btn_record_video;
            if (btn_record_video) gtk_widget_set_sensitive(btn_record_video, FALSE);
            extern GtkWidget *lbl_track;
            if (lbl_track) gtk_label_set_text(GTK_LABEL(lbl_track), "Track: None Selected");
            extern void invalidate_waveform_caches(void);
            invalidate_waveform_caches();
            extern GtkWidget *waveform_area_bt;
            extern GtkWidget *waveform_area_input;
            if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
            if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);
        }
    }
}

void reset_multitrack_ui_states(void) {
    for (int i = 0; i < MAX_TRACKS; i++) {
        atomic_store_explicit(&master_tracks[i].is_soloed, false, memory_order_release);
        snprintf(user_track_names[i], 64, "Track %d", i + 1);
    }
    atomic_store_explicit(&selected_tracks_mask, 1, memory_order_release);
    refresh_multitrack_tracks_ui();
}

void multitrack_select_all_active(void) {
    uint32_t new_mask = 0;
    int highest_active = -1;
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) {
            new_mask |= (1 << i);
            highest_active = i;
        }
    }
    if (new_mask != 0) {
        atomic_store_explicit(&selected_tracks_mask, new_mask, memory_order_release);
        if (highest_active >= 0) {
            atomic_store_explicit(&current_recording_track, highest_active, memory_order_release);
        }
        extern void update_multitrack_status_ui(void);
        update_multitrack_status_ui();
    }
}

static void on_invert_solo_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press; (void)x; (void)y; (void)user_data;
    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
    bool changed = false;

    if (state & GDK_CONTROL_MASK) {
        bool target_state = false;
        bool found_first = false;

        for (int i = 0; i < MAX_TRACKS; i++) {
            if (atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) {
                if (!found_first) {
                    target_state = !atomic_load_explicit(&master_tracks[i].is_soloed, memory_order_acquire);
                    found_first = true;
                }
                if (atomic_load_explicit(&master_tracks[i].is_soloed, memory_order_acquire) != target_state) {
                    atomic_store_explicit(&master_tracks[i].is_soloed, target_state, memory_order_release);
                    changed = true;
                }
            }
        }
    } else {
        for (int i = 0; i < MAX_TRACKS; i++) {
            if (atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire)) {
                bool current_solo = atomic_load_explicit(&master_tracks[i].is_soloed, memory_order_acquire);
                atomic_store_explicit(&master_tracks[i].is_soloed, !current_solo, memory_order_release);
                changed = true;
            }
        }
    }

    if (changed) {
        extern void mark_session_dirty(void);
        mark_session_dirty();

        extern void invalidate_waveform_caches(void);
        extern GtkWidget *waveform_area_bt;
        invalidate_waveform_caches();
        if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);

        refresh_multitrack_tracks_ui();
    }
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

void ui_multitrack_init(GtkBuilder *b_multi) {
    lbl_multitrack_status = GTK_WIDGET(gtk_builder_get_object(b_multi, "lbl_multitrack_status"));

    GtkWidget *btn_invert_solo = GTK_WIDGET(gtk_builder_get_object(b_multi, "btn_invert_solo"));
    if (btn_invert_solo) {
        GtkGesture *click_gesture = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click_gesture), GDK_BUTTON_PRIMARY);
        g_signal_connect(click_gesture, "pressed", G_CALLBACK(on_invert_solo_pressed), NULL);
        gtk_widget_add_controller(btn_invert_solo, GTK_EVENT_CONTROLLER(click_gesture));
    }
}

static void init_default_track_names(void) {
    if (names_initialized) return;
    for (int i = 0; i < MAX_TRACKS; i++) {
        snprintf(user_track_names[i], 64, "Track %d", i + 1);
    }
    names_initialized = true;
}

static void on_solo_track_toggled(GtkToggleButton *btn, gpointer user_data);

void get_multitrack_track_name(int track_idx, char *out_name) {
    if (track_idx >= 0 && track_idx < MAX_TRACKS) {
        strncpy(out_name, user_track_names[track_idx], 64);
    } else {
        out_name[0] = '\0';
    }
}

void set_multitrack_track_name(int track_idx, const char *name) {
    if (track_idx >= 0 && track_idx < MAX_TRACKS) {
        strncpy(user_track_names[track_idx], name, 63);
        user_track_names[track_idx][63] = '\0';
    }
}

void refresh_multitrack_tracks_ui(void) {
    init_default_track_names();
    int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);
    uint32_t mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);

    for (int i = 0; i < MAX_TRACKS; i++) {
        if (!lbl_indicators[i]) continue;
        gtk_label_set_text(GTK_LABEL(lbl_indicators[i]), (i == rec_track) ? "🔴" : "  ");

        if (mask & (1 << i)) gtk_widget_add_css_class(row_boxes[i], "active-row");
        else gtk_widget_remove_css_class(row_boxes[i], "active-row");

        gtk_label_set_text(GTK_LABEL(name_editors[i]), user_track_names[i]);
        gtk_widget_queue_draw(name_editors[i]);

        if (color_dots[i]) gtk_widget_queue_draw(color_dots[i]);

        g_signal_handlers_block_by_func(btn_solos[i], G_CALLBACK(on_solo_track_toggled), GINT_TO_POINTER(i));
        bool is_soloed = atomic_load_explicit(&master_tracks[i].is_soloed, memory_order_acquire);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn_solos[i]), is_soloed);
        g_signal_handlers_unblock_by_func(btn_solos[i], G_CALLBACK(on_solo_track_toggled), GINT_TO_POINTER(i));

        if (spin_gains[i]) {
            float current_gain = atomic_load_explicit(&master_tracks[i].gain, memory_order_relaxed);
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

    atomic_store_explicit(&master_tracks[track_idx].is_soloed, is_soloed, memory_order_release);

    mark_session_dirty();

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
        mark_session_dirty();
        refresh_multitrack_tracks_ui();

        int current = atomic_load_explicit(&current_recording_track, memory_order_acquire);
        if (current == rd->track_idx) {
            update_multitrack_status_ui();
        }
    }

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
    (void)n_press;
    int track_idx = GPOINTER_TO_INT(user_data);
    if (track_idx < 0 || track_idx >= MAX_TRACKS) return;

    GtkWidget *hbox = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    gtk_widget_set_focusable(hbox, TRUE);
    gtk_widget_grab_focus(hbox);

    GtkWidget *target = gtk_widget_pick(hbox, x, y, GTK_PICK_DEFAULT);

    if (target != hbox) {
        GtkWidget *parent = target;
        while (parent && parent != hbox) {
            if (GTK_IS_BUTTON(parent) || GTK_IS_SPIN_BUTTON(parent)) {
                gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED);
                return;
            }
            parent = gtk_widget_get_parent(parent);
        }
    }

    GdkModifierType state = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(gesture));
    uint32_t current_mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);
    int rec_track = atomic_load_explicit(&current_recording_track, memory_order_acquire);

    if (state & GDK_SHIFT_MASK) {
        int start = (rec_track < track_idx) ? rec_track : track_idx;
        int end = (rec_track > track_idx) ? rec_track : track_idx;
        uint32_t new_mask = current_mask;
        for (int i = start; i <= end; i++) new_mask |= (1 << i);
        atomic_store_explicit(&selected_tracks_mask, new_mask, memory_order_release);
    } else if (state & GDK_CONTROL_MASK) {
        atomic_store_explicit(&selected_tracks_mask, current_mask ^ (1 << track_idx), memory_order_release);
    } else {
        atomic_store_explicit(&current_recording_track, track_idx, memory_order_release);
        atomic_store_explicit(&selected_tracks_mask, (1 << track_idx), memory_order_release);
    }

    int active = atomic_load_explicit(&active_track_count, memory_order_acquire);
    if (track_idx >= active) atomic_store_explicit(&active_track_count, track_idx + 1, memory_order_release);

    update_multitrack_status_ui();
}

static GdkContentProvider* on_track_drag_prepare(GtkDragSource *source, double x, double y, gpointer user_data) {
    (void)source; (void)x; (void)y;
    int track_idx = GPOINTER_TO_INT(user_data);
    GValue value = G_VALUE_INIT;
    g_value_init(&value, G_TYPE_INT);
    g_value_set_int(&value, track_idx);
    return gdk_content_provider_new_for_value(&value);
}

/**
 * @brief Defers UI refresh to the GTK main loop, preventing race conditions with spinbox signal blockers.
 */
static gboolean deferred_refresh_multitrack_ui(gpointer user_data) {
    (void)user_data;
    refresh_multitrack_tracks_ui();
    return G_SOURCE_REMOVE;
}

static gboolean on_track_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data) {
    (void)target; (void)x; (void)y;
    int src_idx = g_value_get_int(value);
    int dest_idx = GPOINTER_TO_INT(user_data);

    if (src_idx == dest_idx || src_idx < 0 || src_idx >= MAX_TRACKS || dest_idx < 0 || dest_idx >= MAX_TRACKS) return FALSE;

    if (!await_rt_thread_detach()) return FALSE;

    int current_rec = atomic_load_explicit(&current_recording_track, memory_order_acquire);
    int step = (src_idx < dest_idx) ? 1 : -1;

    for (int i = src_idx; i != dest_idx; i += step) {
        int next = i + step;

        float *tmp_multi = master_tracks[i].active_buffer;
        master_tracks[i].active_buffer = master_tracks[next].active_buffer;
        master_tracks[next].active_buffer = tmp_multi;

        char temp_name[64];
        strncpy(temp_name, user_track_names[i], 64);
        strncpy(user_track_names[i], user_track_names[next], 64);
        strncpy(user_track_names[next], temp_name, 64);

        char temp_hidden[64];
        strncpy(temp_hidden, hidden_track_names[i], 64);
        strncpy(hidden_track_names[i], hidden_track_names[next], 64);
        strncpy(hidden_track_names[next], temp_hidden, 64);

        bool solo_i = atomic_load_explicit(&master_tracks[i].is_soloed, memory_order_acquire);
        bool solo_next = atomic_load_explicit(&master_tracks[next].is_soloed, memory_order_acquire);
        atomic_store_explicit(&master_tracks[i].is_soloed, solo_next, memory_order_release);
        atomic_store_explicit(&master_tracks[next].is_soloed, solo_i, memory_order_release);

        float gain_i = atomic_load_explicit(&master_tracks[i].gain, memory_order_acquire);
        float gain_next = atomic_load_explicit(&master_tracks[next].gain, memory_order_acquire);
        atomic_store_explicit(&master_tracks[i].gain, gain_next, memory_order_release);
        atomic_store_explicit(&master_tracks[next].gain, gain_i, memory_order_release);

        bool audio_i = atomic_load_explicit(&master_tracks[i].has_audio, memory_order_acquire);
        bool audio_next = atomic_load_explicit(&master_tracks[next].has_audio, memory_order_acquire);
        atomic_store_explicit(&master_tracks[i].has_audio, audio_next, memory_order_release);
        atomic_store_explicit(&master_tracks[next].has_audio, audio_i, memory_order_release);

        if (current_rec == i) current_rec = next;
        else if (current_rec == next) current_rec = i;
    }

    for (int k = 0; k < global_undo_ledger.max_capacity; k++) {
        if (global_undo_ledger.states[k].track_idx == src_idx) {
            global_undo_ledger.states[k].track_idx = dest_idx;
        } else if (src_idx < dest_idx && global_undo_ledger.states[k].track_idx > src_idx && global_undo_ledger.states[k].track_idx <= dest_idx) {
            global_undo_ledger.states[k].track_idx--;
        } else if (src_idx > dest_idx && global_undo_ledger.states[k].track_idx >= dest_idx && global_undo_ledger.states[k].track_idx < src_idx) {
            global_undo_ledger.states[k].track_idx++;
        }
    }

    atomic_store_explicit(&current_recording_track, current_rec, memory_order_release);

    extern _Atomic int timebase_track;
    int current_timebase = atomic_load_explicit(&timebase_track, memory_order_acquire);

    if (src_idx == current_timebase) {
        atomic_store_explicit(&timebase_track, dest_idx, memory_order_release);
        current_timebase = dest_idx;
    } else if (src_idx < dest_idx && current_timebase > src_idx && current_timebase <= dest_idx) {
        atomic_store_explicit(&timebase_track, current_timebase - 1, memory_order_release);
        current_timebase--;
    } else if (src_idx > dest_idx && current_timebase >= dest_idx && current_timebase < src_idx) {
        atomic_store_explicit(&timebase_track, current_timebase + 1, memory_order_release);
        current_timebase++;
    }

    if (src_idx == current_timebase || dest_idx == current_timebase) {
        extern float *pristine_bt_buf;
        extern size_t pristine_frames;
        if (pristine_bt_buf && master_tracks[current_timebase].active_buffer) {
            memcpy(pristine_bt_buf, master_tracks[current_timebase].active_buffer, pristine_frames * 2 * sizeof(float));

            extern atomic_size_t playback_pos;
            extern atomic_size_t backing_track_frames;
            size_t pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
            size_t frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
            if (frames > 0) {
                extern void seek_backing_track(double);
                seek_backing_track((double)pos / (double)frames);
            }
        }
    }

    int current_active = atomic_load_explicit(&active_track_count, memory_order_acquire);
    if (dest_idx >= current_active) {
        atomic_store_explicit(&active_track_count, dest_idx + 1, memory_order_release);
    }

    mark_session_dirty();
    resume_rt_thread();

    g_idle_add(deferred_refresh_multitrack_ui, NULL);

    extern void invalidate_waveform_caches(void);
    invalidate_waveform_caches();
    if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);

    return TRUE;
}

typedef struct {
    char *filepath;
    int track_idx;
    bool resume_playback; // Track playback state
} ImportData;

static void free_import_data(gpointer data) {
    ImportData *id = (ImportData *)data;
    g_free(id->filepath);
    g_free(id);
}

static GMutex import_mutex;

static void import_stem_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)source_object; (void)cancellable;
    ImportData *data = (ImportData *)task_data;

    g_mutex_lock(&import_mutex);

    // FAILSAFE: If the canvas failed to allocate on cold boot due to OS hardware handoff,
    // autonomously allocate it here in the background thread.
    extern size_t pristine_frames;
    if (pristine_frames == 0) {
        int canvas_limit_min = ui_state.config.default_canvas_limit_min > 0 ? ui_state.config.default_canvas_limit_min : 5;
        extern int init_empty_loop_canvas(int);
        init_empty_loop_canvas(canvas_limit_min * 60);
    }

    if (pristine_frames == 0) {
        g_mutex_unlock(&import_mutex);
        g_task_return_int(task, -1);
        return;
    }

    extern int import_to_layer(const char*, int);
    int result = import_to_layer(data->filepath, data->track_idx);
    g_mutex_unlock(&import_mutex);

    g_task_return_int(task, result);
}

static gboolean reset_status_label_deferred(gpointer user_data) {
    (void)user_data;
    log_and_notify(1, "<span>Status: Ready</span>");
    return G_SOURCE_REMOVE;
}

static void sanitize_track_name(const char *filename, char *out_name, size_t max_len) {
    size_t i = 0, j = 0;
    bool last_space = false;
    // Fix: Relax limits to explicitly preserve numeric take/stem identifiers
    while (filename[i] != '\0' && filename[i] != '.' && j < max_len - 1) {
        if (g_ascii_isalnum(filename[i])) {
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
    extern GtkWidget *main_spinner;
    if (main_spinner) gtk_spinner_stop(GTK_SPINNER(main_spinner));

    // NEW: Unlock the UI's master container
    GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (win) {
        GtkWidget *master_box = gtk_window_get_child(win);
        if (master_box) gtk_widget_set_sensitive(master_box, TRUE);
    }

    if (result == 0 || result == 1) {
        if (track_idx > 0 || ui_state.is_existing_session) {
            mark_session_dirty();
        } else {
            // FIX: Prevent false-positive dirty states from the engine's initial undo registration
            extern void mark_session_saved(void);
            mark_session_saved();
        }

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
        update_multitrack_status_ui();

        extern void invalidate_waveform_caches(void);
        invalidate_waveform_caches();
        extern GtkWidget *waveform_area_bt;
        extern GtkWidget *waveform_area_input;
        if (waveform_area_bt) gtk_widget_queue_draw(waveform_area_bt);
        if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);

        // Wake up the transport buttons now that the canvas contains audio data
        if (btn_play) gtk_widget_set_sensitive(btn_play, TRUE);
        if (btn_record) gtk_widget_set_sensitive(btn_record, TRUE);
        extern GtkWidget *btn_record_video;
        if (btn_record_video) gtk_widget_set_sensitive(btn_record_video, TRUE);

        extern GtkAdjustment *waveform_adj;
        extern double zoom_multiplier;
        extern GtkWidget *waveform_area_bt;
        extern GtkWidget *waveform_area_input;
        size_t current_pos = atomic_load_explicit(&playback_pos, memory_order_acquire);
        size_t total_frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);

        if (total_frames > 0 && waveform_adj && waveform_area_bt) {
            int width = gtk_widget_get_width(waveform_area_bt);
            double virtual_width = width * zoom_multiplier;
            double target_pixel = ((double)current_pos / (double)total_frames) * virtual_width;

            double page_size = gtk_adjustment_get_page_size(waveform_adj);
            double upper = gtk_adjustment_get_upper(waveform_adj);

            double new_val = target_pixel - (page_size / 2.0);
            if (new_val < 0) new_val = 0;
            if (new_val > upper - page_size) new_val = upper - page_size;

            gtk_adjustment_set_value(waveform_adj, new_val);
            gtk_widget_queue_draw(waveform_area_bt);
            if (waveform_area_input) gtk_widget_queue_draw(waveform_area_input);
        }

        // Resume playback if it was playing before the import
        if (data->resume_playback) {
            if (btn_play && gtk_widget_is_sensitive(btn_play)) {
                g_signal_emit_by_name(btn_play, "clicked");
            }
        }

        if (result == 1) {
            log_and_notify(1, "<span foreground='#ff4444' weight='heavy'>WARNING: Dropped track was truncated to match canvas length!</span>");
            g_timeout_add(3000, reset_status_label_deferred, NULL);
        } else {
            log_and_notify(1, "<span>Status: Stem Imported Successfully</span>");
        }
    } else if (result == -2) {
        log_and_notify(1, "<span foreground='#ff4444'><b>Status: Insufficient RAM for Stem</b></span>");
    } else if (result == -3) {
        log_and_notify(1, "<span foreground='#ff4444'><b>Status: Stem Import Failed (Corrupt or Empty Media)</b></span>");
    } else {
        log_and_notify(1, "<span foreground='#ff4444'><b>Status: Stem Import Failed</b></span>");
    }
}

gboolean handle_track_drop_payload(const GValue *value, int start_track_idx) {
    if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        GdkFileList *file_list = g_value_get_boxed(value);
        if (file_list) {
            GSList *files = gdk_file_list_get_files(file_list);
            if (files) {
                int current_target = start_track_idx;
                for (GSList *l = files; l != NULL; l = l->next) {
                    if (current_target >= MAX_TRACKS) break;
                    GFile *file = G_FILE(l->data);
                    char *path = g_file_get_path(file);
                    if (path) {
                        multitrack_import_file_async(path, current_target);
                        g_free(path);
                        current_target++;
                    }
                }
                g_slist_free(files);
                return TRUE;
            }
        }
    } else if (G_VALUE_HOLDS(value, QJ_TYPE_TRACK)) {
        QjTrack *track = g_value_get_object(value);
        if (track && track->filepath) {
            if (start_track_idx < MAX_TRACKS) {
                multitrack_import_file_async(track->filepath, start_track_idx);
            }
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean on_track_file_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data) {
    (void)target; (void)x; (void)y;
    int track_idx = GPOINTER_TO_INT(user_data);
    return handle_track_drop_payload(value, track_idx);
}

static gboolean on_multitrack_key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data) {
    (void)controller; (void)keycode; (void)user_data;

    // Only intercept Up/Down arrows. Let Left/Right bubble to the spin buttons or global timeline seek.
    if (keyval == GDK_KEY_Up || keyval == GDK_KEY_Down) {
        int current = atomic_load_explicit(&current_recording_track, memory_order_acquire);
        int next = current;

        if (keyval == GDK_KEY_Up && current > 0) next = current - 1;
        else if (keyval == GDK_KEY_Down && current < MAX_TRACKS - 1) next = current + 1;

        if (next != current) {
            if (state & GDK_SHIFT_MASK) {
                // Expand selection
                uint32_t mask = atomic_load_explicit(&selected_tracks_mask, memory_order_acquire);
                mask |= (1 << next);
                atomic_store_explicit(&selected_tracks_mask, mask, memory_order_release);
            } else {
                // Single selection
                atomic_store_explicit(&selected_tracks_mask, (1 << next), memory_order_release);
            }

            atomic_store_explicit(&current_recording_track, next, memory_order_release);

            int active = atomic_load_explicit(&active_track_count, memory_order_acquire);
            if (next >= active) {
                atomic_store_explicit(&active_track_count, next + 1, memory_order_release);
            }

            // Force GTK's internal focus ring to follow our DAW selection
            if (row_boxes[next]) {
                gtk_widget_grab_focus(row_boxes[next]);
            }

            update_multitrack_status_ui();
            return TRUE; // Consume the keystroke
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

    // NEW: Attach the keyboard interceptor to the entire multitrack list
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_multitrack_key_pressed), NULL);
    gtk_widget_add_controller(main_box, key_ctrl);

    for (int i = 0; i < MAX_TRACKS; i++) {
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        row_boxes[i] = hbox;

        // CRITICAL FIX: Make the row itself natively focusable by the keyboard
        gtk_widget_set_focusable(hbox, TRUE);

        gtk_widget_add_css_class(hbox, "multitrack-row");
        gtk_widget_set_margin_start(hbox, 5);
        gtk_widget_set_margin_end(hbox, 5);

        GtkGesture *click_gesture = gtk_gesture_click_new();
        g_signal_connect(click_gesture, "pressed", G_CALLBACK(on_row_clicked), GINT_TO_POINTER(i));
        gtk_widget_add_controller(hbox, GTK_EVENT_CONTROLLER(click_gesture));

        GtkWidget *drag_icon = gtk_label_new("☰");
        gtk_widget_set_opacity(drag_icon, 0.5);

        GtkDragSource *drag_source = gtk_drag_source_new();
        gtk_drag_source_set_actions(drag_source, GDK_ACTION_MOVE);
        g_signal_connect(drag_source, "prepare", G_CALLBACK(on_track_drag_prepare), GINT_TO_POINTER(i));
        gtk_widget_add_controller(drag_icon, GTK_EVENT_CONTROLLER(drag_source));

        GtkDropTarget *drop_target = gtk_drop_target_new(G_TYPE_INT, GDK_ACTION_MOVE);
        g_signal_connect(drop_target, "drop", G_CALLBACK(on_track_drop), GINT_TO_POINTER(i));
        gtk_widget_add_controller(hbox, GTK_EVENT_CONTROLLER(drop_target));

        GType file_drop_types[] = { GDK_TYPE_FILE_LIST, QJ_TYPE_TRACK };
        GtkDropTarget *file_drop_target = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY | GDK_ACTION_MOVE);
        gtk_drop_target_set_gtypes(file_drop_target, file_drop_types, 2);
        g_signal_connect(file_drop_target, "drop", G_CALLBACK(on_track_file_drop), GINT_TO_POINTER(i));
        gtk_widget_add_controller(hbox, GTK_EVENT_CONTROLLER(file_drop_target));

        lbl_indicators[i] = gtk_label_new("  ");
        gtk_widget_set_margin_end(lbl_indicators[i], 5);

        name_editors[i] = gtk_label_new(user_track_names[i]);
        gtk_widget_set_halign(name_editors[i], GTK_ALIGN_START);
        gtk_widget_set_hexpand(name_editors[i], TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(name_editors[i]), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(name_editors[i]), 1);

        GtkWidget *color_dot = gtk_drawing_area_new();
        color_dots[i] = color_dot;
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(color_dot), 14);
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(color_dot), 14);
        gtk_widget_set_valign(color_dot, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_end(color_dot, 5);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(color_dot), on_draw_track_color_dot, GINT_TO_POINTER(i), NULL);

        GtkWidget *btn_rename = gtk_button_new_with_label("🖉");
        gtk_widget_set_tooltip_text(btn_rename, "Rename Track (F2)");
        gtk_widget_add_css_class(btn_rename, "flat");
        gtk_widget_add_css_class(btn_rename, "symbol-btn");
        g_signal_connect(btn_rename, "clicked", G_CALLBACK(on_rename_clicked), GINT_TO_POINTER(i));

        float current_gain = atomic_load_explicit(&master_tracks[i].gain, memory_order_relaxed);
        float loaded_db = (current_gain <= 0.001f) ? -24.0f : 20.0f * log10f(current_gain);

        GtkAdjustment *adj = gtk_adjustment_new(loaded_db, -24.0, 24.0, 1.0, 5.0, 0.0);
        spin_gains[i] = gtk_spin_button_new(adj, 1, 0);

        gtk_widget_add_css_class(spin_gains[i], "compact-spin");
        gtk_widget_set_hexpand(spin_gains[i], FALSE);
        gtk_widget_set_size_request(spin_gains[i], 50, -1);
        gtk_spin_button_set_climb_rate(GTK_SPIN_BUTTON(spin_gains[i]), 1.0);
        gtk_widget_set_tooltip_text(spin_gains[i], "Track Level (Non-Destructive, Post-Buffer)");
        g_signal_connect(spin_gains[i], "value-changed", G_CALLBACK(on_track_gain_changed), GINT_TO_POINTER(i));

        btn_solos[i] = gtk_toggle_button_new_with_label("𝅘𝅥𝅮");
        gtk_widget_add_css_class(btn_solos[i], "symbol-btn");
        gtk_widget_set_tooltip_text(btn_solos[i], "Solo this track");
        g_signal_connect(btn_solos[i], "toggled", G_CALLBACK(on_solo_track_toggled), GINT_TO_POINTER(i));

        gtk_box_append(GTK_BOX(hbox), drag_icon);
        gtk_box_append(GTK_BOX(hbox), lbl_indicators[i]);
        gtk_box_append(GTK_BOX(hbox), name_editors[i]);
        gtk_box_append(GTK_BOX(hbox), color_dot);
        gtk_box_append(GTK_BOX(hbox), btn_rename);
        gtk_box_append(GTK_BOX(hbox), spin_gains[i]);
        gtk_box_append(GTK_BOX(hbox), btn_solos[i]);

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
    extern GtkWidget *main_spinner;
    extern _Atomic bool engine_is_playing;

    bool was_playing = atomic_load_explicit(&engine_is_playing, memory_order_acquire);
    if (was_playing) {
        on_stop_clicked(NULL, NULL);
    }

    // NEW: Lock the UI's master container to protect memory arrays during import
    GtkWindow *win = gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
    if (win) {
        GtkWidget *master_box = gtk_window_get_child(win);
        if (master_box) gtk_widget_set_sensitive(master_box, FALSE);
    }

    gtk_spinner_start(GTK_SPINNER(main_spinner));
    log_and_notify(1, "Status: Importing stem...");

    ImportData *data = g_new(ImportData, 1);
    data->filepath = g_strdup(filepath);
    data->track_idx = track_idx;
    data->resume_playback = was_playing;

    GTask *task = g_task_new(NULL, NULL, import_stem_ready, GINT_TO_POINTER(track_idx));
    g_task_set_task_data(task, data, free_import_data);
    g_task_run_in_thread(task, import_stem_thread);
    g_object_unref(task);
}
