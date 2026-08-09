#include "ui_looper.h"
#include "ui_globals.h"
#include "ui_playlist.h"
#include "ui_waveforms.h"
#include "audio_engine.h"
#include "ui_dashboard.h"
#include <string.h>

extern _Atomic bool engine_is_playing;

static char user_layer_names[MAX_LOOPS][64] = {0};
static bool names_initialized = false;

static GtkWidget *lbl_indicators[MAX_LOOPS];
static GtkWidget *name_editors[MAX_LOOPS];
static GtkWidget *btn_solos[MAX_LOOPS];
static GtkWidget *btn_clears[MAX_LOOPS];
static GtkWidget *row_boxes[MAX_LOOPS];

void update_looper_status_ui(void) {
    int current = atomic_load_explicit(&current_recording_layer, memory_order_acquire);
    char buf[64];
    if (current == 0) {
        snprintf(buf, sizeof(buf), "Looper Ready - Base Track");
    } else {
        snprintf(buf, sizeof(buf), "Looper Ready - Overdub %d / %d", current, MAX_LOOPS - 1);
    }
    gtk_label_set_text(GTK_LABEL(lbl_looper_status), buf);

    size_t frames = atomic_load_explicit(&backing_track_frames, memory_order_acquire);
    bool base_ready = (frames > 0 && current < MAX_LOOPS - 1);
    if (btn_looper_next) gtk_widget_set_sensitive(btn_looper_next, base_ready);

    refresh_looper_layers_ui();
}

static void on_looper_undo_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    looper_undo_layer();
    update_looper_status_ui();
    gtk_widget_queue_draw(waveform_area_bt);
    gtk_widget_queue_draw(waveform_area_input);
}

static void on_looper_next_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    on_stop_clicked(NULL, NULL);
    seek_backing_track(0.0);
    update_looper_status_ui();
    if (gtk_widget_is_sensitive(btn_record)) on_start_clicked(GTK_BUTTON(btn_record), NULL);
    gtk_widget_queue_draw(waveform_area_bt);
    gtk_widget_queue_draw(waveform_area_input);
}
static void on_blank_canvas_confirm_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    GtkWidget *sw = GTK_WIDGET(user_data);
    int response = gtk_alert_dialog_choose_finish(alert, res, NULL);

    if (response == 0) {
        g_signal_handlers_block_by_func(sw, G_CALLBACK(on_blank_canvas_toggled), NULL);
        gtk_switch_set_active(GTK_SWITCH(sw), TRUE);
        g_signal_handlers_unblock_by_func(sw, G_CALLBACK(on_blank_canvas_toggled), NULL);

        ui_state.config.looper_blank_canvas = 1;
        save_qjams_config(ui_state.config_path, &ui_state.config);

        ui_state.selected_track_path[0] = '\0';
        update_playlist_toggle_state();

        int loop_min = ui_state.config.looper_duration_min > 0 ? ui_state.config.looper_duration_min : 5;
        init_empty_loop_canvas(loop_min * 60);
        zoom_multiplier = 1.0;
        atomic_store_explicit(&active_layer_count, 1, memory_order_release);
        atomic_store_explicit(&current_recording_layer, 0, memory_order_release);

        char track_lbl[128];
        snprintf(track_lbl, sizeof(track_lbl), "Track: Blank Loop Canvas [%02d:00]", loop_min);
        gtk_label_set_text(GTK_LABEL(lbl_track), track_lbl);

        ui_state.session_is_dirty = false;
        invalidate_waveform_caches();
        gtk_widget_queue_draw(waveform_area_bt);
        gtk_widget_queue_draw(waveform_area_input);
        update_looper_status_ui();
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
        gtk_alert_dialog_set_default_button(alert, 1);

        gtk_alert_dialog_choose(alert, GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(gobject))), NULL, on_blank_canvas_confirm_response, gobject);
        g_object_unref(alert);
        return;
    }

    ui_state.config.looper_blank_canvas = active ? 1 : 0;
    save_qjams_config(ui_state.config_path, &ui_state.config);

    if (active) {
        ui_state.selected_track_path[0] = '\0';
        update_playlist_toggle_state();

        int loop_min = ui_state.config.looper_duration_min > 0 ? ui_state.config.looper_duration_min : 5;
        init_empty_loop_canvas(loop_min * 60);
        zoom_multiplier = 1.0;
        atomic_store_explicit(&active_layer_count, 1, memory_order_release);
        atomic_store_explicit(&current_recording_layer, 0, memory_order_release);

        char track_lbl[128];
        snprintf(track_lbl, sizeof(track_lbl), "Track: Blank Loop Canvas [%02d:00]", loop_min);
        gtk_label_set_text(GTK_LABEL(lbl_track), track_lbl);

        ui_state.session_is_dirty = false;
        invalidate_waveform_caches();
        gtk_widget_queue_draw(waveform_area_bt);
        gtk_widget_queue_draw(waveform_area_input);
        update_looper_status_ui();
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

void reset_looper_ui_states(void) {
    for (int i = 0; i < MAX_LOOPS; i++) {
        atomic_store_explicit(&layer_is_muted[i], false, memory_order_release);
        atomic_store_explicit(&layer_is_soloed[i], false, memory_order_release);
        if (i == 0) snprintf(user_layer_names[i], 64, "Base Track");
        else snprintf(user_layer_names[i], 64, "Overdub %d", i);
    }
    refresh_looper_layers_ui();
}

void ui_looper_init(GtkBuilder *builder) {
    mode_stack = GTK_WIDGET(gtk_builder_get_object(builder, "mode_stack"));
    lbl_looper_status = GTK_WIDGET(gtk_builder_get_object(builder, "lbl_looper_status"));
    btn_looper_undo = GTK_WIDGET(gtk_builder_get_object(builder, "btn_looper_undo"));
    btn_looper_next = GTK_WIDGET(gtk_builder_get_object(builder, "btn_looper_next"));
    sw_blank_canvas = GTK_WIDGET(gtk_builder_get_object(builder, "sw_blank_canvas"));

    g_signal_connect(btn_looper_undo, "clicked", G_CALLBACK(on_looper_undo_clicked), NULL);
    g_signal_connect(btn_looper_next, "clicked", G_CALLBACK(on_looper_next_clicked), NULL);

    g_signal_connect(sw_blank_canvas, "notify::active", G_CALLBACK(on_blank_canvas_toggled), NULL);
    g_signal_handlers_block_by_func(sw_blank_canvas, G_CALLBACK(on_blank_canvas_toggled), NULL);
    gtk_switch_set_active(GTK_SWITCH(sw_blank_canvas), ui_state.config.looper_blank_canvas != 0);
    g_signal_handlers_unblock_by_func(sw_blank_canvas, G_CALLBACK(on_blank_canvas_toggled), NULL);

    // FIX: Remove hardcoded 5min init. Let the dynamic resizer calculate it based on startup mode.
    adjust_blank_canvas_for_mode(ui_state.config.looper_mode_active != 0);
}

static void init_default_layer_names(void) {
    if (names_initialized) return;
    for (int i = 0; i < MAX_LOOPS; i++) {
        if (i == 0) snprintf(user_layer_names[i], 64, "Base Track");
        else snprintf(user_layer_names[i], 64, "Overdub %d", i);
    }
    names_initialized = true;
}

static void on_mute_layer_toggled(GtkToggleButton *btn, gpointer user_data) {
    int layer_idx = GPOINTER_TO_INT(user_data);
    bool is_muted = gtk_toggle_button_get_active(btn);
    atomic_store_explicit(&layer_is_muted[layer_idx], is_muted, memory_order_release);
    gtk_widget_set_opacity(GTK_WIDGET(btn), is_muted ? 0.4 : 1.0);
    invalidate_waveform_caches();
    gtk_widget_queue_draw(waveform_area_bt);
    gtk_widget_queue_draw(waveform_area_input);
}

static void on_solo_layer_toggled(GtkToggleButton *btn, gpointer user_data) {
    int layer_idx = GPOINTER_TO_INT(user_data);
    bool is_soloed = gtk_toggle_button_get_active(btn);
    atomic_store_explicit(&layer_is_soloed[layer_idx], is_soloed, memory_order_release);
    if (is_soloed && !atomic_load_explicit(&engine_is_playing, memory_order_acquire)) {
        if (btn_play && gtk_widget_is_sensitive(btn_play)) g_signal_emit_by_name(btn_play, "clicked");
    }
}

static void on_layer_name_editing_changed(GObject *object, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    int layer_idx = GPOINTER_TO_INT(user_data);
    if (!gtk_editable_label_get_editing(GTK_EDITABLE_LABEL(object))) {
        const char *new_text = gtk_editable_get_text(GTK_EDITABLE(object));
        strncpy(user_layer_names[layer_idx], new_text, 63);
        user_layer_names[layer_idx][63] = '\0';
        gtk_editable_set_editable(GTK_EDITABLE(object), FALSE);
    }
}

static gboolean deferred_start_rename(gpointer user_data) {
    GtkWidget *editor = GTK_WIDGET(user_data);
    if (editor && GTK_IS_EDITABLE_LABEL(editor)) {
        gtk_editable_set_editable(GTK_EDITABLE(editor), TRUE);
        gtk_widget_grab_focus(editor);
        gtk_editable_label_start_editing(GTK_EDITABLE_LABEL(editor));
    }
    return G_SOURCE_REMOVE;
}

static void on_rename_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    int layer_idx = GPOINTER_TO_INT(user_data);
    GtkWidget *editor = name_editors[layer_idx];
    if (editor) g_idle_add(deferred_start_rename, editor);
}

static void on_row_clicked(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)gesture; (void)n_press; (void)x; (void)y;
    int layer_idx = GPOINTER_TO_INT(user_data);
    int current = atomic_load_explicit(&current_recording_layer, memory_order_acquire);
    if (current != layer_idx && layer_idx >= 0 && layer_idx < MAX_LOOPS) {
        atomic_store_explicit(&current_recording_layer, layer_idx, memory_order_release);
        int active = atomic_load_explicit(&active_layer_count, memory_order_acquire);
        if (layer_idx >= active) atomic_store_explicit(&active_layer_count, layer_idx + 1, memory_order_release);
        update_looper_status_ui();
    }
}

static GdkContentProvider* on_layer_drag_prepare(GtkDragSource *source, double x, double y, gpointer user_data) {
    (void)source; (void)x; (void)y;
    int layer_idx = GPOINTER_TO_INT(user_data);
    GValue value = G_VALUE_INIT;
    g_value_init(&value, G_TYPE_INT);
    g_value_set_int(&value, layer_idx);
    return gdk_content_provider_new_for_value(&value);
}

static gboolean on_layer_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data) {
    (void)target; (void)x; (void)y;
    int src_idx = g_value_get_int(value);
    int dest_idx = GPOINTER_TO_INT(user_data);
    if (src_idx == dest_idx || src_idx < 0 || src_idx >= MAX_LOOPS || dest_idx < 0 || dest_idx >= MAX_LOOPS) return FALSE;

    int current_rec = atomic_load_explicit(&current_recording_layer, memory_order_acquire);
    int step = (src_idx < dest_idx) ? 1 : -1;
    for (int i = src_idx; i != dest_idx; i += step) {
        int next = i + step;
        swap_looper_layers(i, next);

        char temp_name[64];
        strncpy(temp_name, user_layer_names[i], 64);
        strncpy(user_layer_names[i], user_layer_names[next], 64);
        strncpy(user_layer_names[next], temp_name, 64);

        bool muted_i = atomic_load_explicit(&layer_is_muted[i], memory_order_acquire);
        bool muted_next = atomic_load_explicit(&layer_is_muted[next], memory_order_acquire);
        atomic_store_explicit(&layer_is_muted[i], muted_next, memory_order_release);
        atomic_store_explicit(&layer_is_muted[next], muted_i, memory_order_release);

        bool solo_i = atomic_load_explicit(&layer_is_soloed[i], memory_order_acquire);
        bool solo_next = atomic_load_explicit(&layer_is_soloed[next], memory_order_acquire);
        atomic_store_explicit(&layer_is_soloed[i], solo_next, memory_order_release);
        atomic_store_explicit(&layer_is_soloed[next], solo_i, memory_order_release);

        if (current_rec == i) current_rec = next;
        else if (current_rec == next) current_rec = i;
    }

    atomic_store_explicit(&current_recording_layer, current_rec, memory_order_release);
    invalidate_waveform_caches();
    gtk_widget_queue_draw(waveform_area_bt);
    gtk_widget_queue_draw(waveform_area_input);
    refresh_looper_layers_ui();
    return TRUE;
}

void get_looper_layer_name(int idx, char *out_name) {
    if (idx >= 0 && idx < MAX_LOOPS) strncpy(out_name, user_layer_names[idx], 64);
}

void set_looper_layer_name(int idx, const char *name) {
    if (idx >= 0 && idx < MAX_LOOPS) {
        strncpy(user_layer_names[idx], name, 63);
        user_layer_names[idx][63] = '\0';
    }
}

void refresh_looper_layers_ui(void) {
    init_default_layer_names();
    int rec_layer = atomic_load_explicit(&current_recording_layer, memory_order_acquire);

    for (int i = 0; i < MAX_LOOPS; i++) {
        if (!lbl_indicators[i]) continue;
        gtk_label_set_text(GTK_LABEL(lbl_indicators[i]), (i == rec_layer) ? "🔴" : "  ");
        if (i == rec_layer) gtk_widget_add_css_class(row_boxes[i], "selected");
        else gtk_widget_remove_css_class(row_boxes[i], "selected");

        g_signal_handlers_block_by_func(name_editors[i], G_CALLBACK(on_layer_name_editing_changed), GINT_TO_POINTER(i));
        gtk_editable_set_text(GTK_EDITABLE(name_editors[i]), user_layer_names[i]);
        g_signal_handlers_unblock_by_func(name_editors[i], G_CALLBACK(on_layer_name_editing_changed), GINT_TO_POINTER(i));

        g_signal_handlers_block_by_func(btn_solos[i], G_CALLBACK(on_solo_layer_toggled), GINT_TO_POINTER(i));
        bool is_soloed = atomic_load_explicit(&layer_is_soloed[i], memory_order_acquire);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn_solos[i]), is_soloed);
        g_signal_handlers_unblock_by_func(btn_solos[i], G_CALLBACK(on_solo_layer_toggled), GINT_TO_POINTER(i));

        g_signal_handlers_block_by_func(btn_clears[i], G_CALLBACK(on_mute_layer_toggled), GINT_TO_POINTER(i));
        bool is_muted = atomic_load_explicit(&layer_is_muted[i], memory_order_acquire);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn_clears[i]), is_muted);
        gtk_widget_set_opacity(btn_clears[i], is_muted ? 0.4 : 1.0);
        g_signal_handlers_unblock_by_func(btn_clears[i], G_CALLBACK(on_mute_layer_toggled), GINT_TO_POINTER(i));
    }
}

GtkWidget* create_looper_layers_widget(void) {
    init_default_layer_names();
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(main_box, TRUE);
    gtk_widget_set_hexpand(main_box, FALSE);
    gtk_widget_set_halign(main_box, GTK_ALIGN_FILL);
    gtk_widget_add_css_class(main_box, "looper-canvas");

    for (int i = 0; i < MAX_LOOPS; i++) {
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        row_boxes[i] = hbox;
        gtk_widget_add_css_class(hbox, "looper-row");
        gtk_widget_set_margin_start(hbox, 5);
        gtk_widget_set_margin_end(hbox, 5);

        GtkGesture *click_gesture = gtk_gesture_click_new();
        g_signal_connect(click_gesture, "pressed", G_CALLBACK(on_row_clicked), GINT_TO_POINTER(i));
        gtk_widget_add_controller(hbox, GTK_EVENT_CONTROLLER(click_gesture));

        GtkWidget *drag_icon = gtk_image_new_from_icon_name("list-drag-handle-symbolic");
        gtk_widget_set_opacity(drag_icon, 0.5);

        GtkDragSource *drag_source = gtk_drag_source_new();
        gtk_drag_source_set_actions(drag_source, GDK_ACTION_MOVE);
        g_signal_connect(drag_source, "prepare", G_CALLBACK(on_layer_drag_prepare), GINT_TO_POINTER(i));
        gtk_widget_add_controller(drag_icon, GTK_EVENT_CONTROLLER(drag_source));

        GtkDropTarget *drop_target = gtk_drop_target_new(G_TYPE_INT, GDK_ACTION_MOVE);
        g_signal_connect(drop_target, "drop", G_CALLBACK(on_layer_drop), GINT_TO_POINTER(i));
        gtk_widget_add_controller(hbox, GTK_EVENT_CONTROLLER(drop_target));

        lbl_indicators[i] = gtk_label_new("  ");
        gtk_widget_set_margin_end(lbl_indicators[i], 5);

        name_editors[i] = gtk_editable_label_new(user_layer_names[i]);
        gtk_widget_set_halign(name_editors[i], GTK_ALIGN_START);
        gtk_widget_set_hexpand(name_editors[i], TRUE);
        gtk_editable_set_editable(GTK_EDITABLE(name_editors[i]), FALSE);
        g_signal_connect(name_editors[i], "notify::editing", G_CALLBACK(on_layer_name_editing_changed), GINT_TO_POINTER(i));

        GtkWidget *color_dot = gtk_drawing_area_new();
        gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(color_dot), 14);
        gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(color_dot), 14);
        gtk_widget_set_valign(color_dot, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_end(color_dot, 5);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(color_dot), on_draw_layer_color_dot, GINT_TO_POINTER(i), NULL);

        GtkWidget *btn_rename = gtk_button_new_from_icon_name("document-edit-symbolic");
        gtk_widget_set_tooltip_text(btn_rename, "Rename Layer (F2)");
        gtk_widget_add_css_class(btn_rename, "flat");
        g_signal_connect(btn_rename, "clicked", G_CALLBACK(on_rename_clicked), GINT_TO_POINTER(i));

        btn_solos[i] = gtk_toggle_button_new();
        gtk_button_set_icon_name(GTK_BUTTON(btn_solos[i]), "audio-headphones-symbolic");
        gtk_widget_set_tooltip_text(btn_solos[i], "Solo this layer");
        g_signal_connect(btn_solos[i], "toggled", G_CALLBACK(on_solo_layer_toggled), GINT_TO_POINTER(i));

        btn_clears[i] = gtk_toggle_button_new();
        gtk_button_set_icon_name(GTK_BUTTON(btn_clears[i]), "edit-clear-symbolic");
        gtk_widget_set_tooltip_text(btn_clears[i], "Mute/Disable this layer");
        g_signal_connect(btn_clears[i], "toggled", G_CALLBACK(on_mute_layer_toggled), GINT_TO_POINTER(i));

        gtk_box_append(GTK_BOX(hbox), drag_icon);
        gtk_box_append(GTK_BOX(hbox), lbl_indicators[i]);
        gtk_box_append(GTK_BOX(hbox), name_editors[i]);
        gtk_box_append(GTK_BOX(hbox), color_dot);
        gtk_box_append(GTK_BOX(hbox), btn_rename);
        gtk_box_append(GTK_BOX(hbox), btn_solos[i]);
        gtk_box_append(GTK_BOX(hbox), btn_clears[i]);

        gtk_box_append(GTK_BOX(main_box), hbox);
    }
    refresh_looper_layers_ui();
    return main_box;
}

void looper_start_rename(void) {
    int selected_idx = atomic_load_explicit(&current_recording_layer, memory_order_acquire);
    if (selected_idx >= 0 && selected_idx < MAX_LOOPS) {
        GtkWidget *editor = name_editors[selected_idx];
        if (editor) g_idle_add(deferred_start_rename, editor);
    }
}
