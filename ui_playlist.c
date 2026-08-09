#include "ui_playlist.h"
#include "ui_globals.h"
#include <libavformat/avformat.h>

// --- PLAYLIST UI GLOBALS ---
static GListStore *track_list_store = NULL;
static GtkSingleSelection *track_selection_model = NULL;

// --- TRACK LIST GOBJECT ---
#define QJ_TYPE_TRACK (qj_track_get_type())
G_DECLARE_FINAL_TYPE(QjTrack, qj_track, QJ, TRACK, GObject)

struct _QjTrack {
    GObject parent_instance;
    char *filepath;
    char *display_name;
    bool is_qjams_native;
};

G_DEFINE_TYPE(QjTrack, qj_track, G_TYPE_OBJECT)

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
static bool extract_track_metadata(const char *filepath, char *output_buffer, size_t max_len) {
    bool is_native = false;
    AVFormatContext *fmt_ctx = NULL;
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
    g_object_unref(next_track);

    trigger_track_load();
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

/**
 * @brief Loads track entries into the playlist from a given .m3u file.
 * @param filepath The absolute path to the playlist file.
 * @return void
 */
void load_playlist_from_file(const char *filepath) {
    FILE *f = fopen(filepath, "r");
    if (f) {
        g_list_store_remove_all(track_list_store);
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
        save_current_playlist();
        gtk_label_set_text(GTK_LABEL(lbl_status), "Status: Playlist Loaded");
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
    if (!btn_playlist_toggle || strlen(ui_state.selected_track_path) == 0) return;

    guint found_idx = find_track_index_by_path(ui_state.selected_track_path);

    if (found_idx != GTK_INVALID_LIST_POSITION) {
        gtk_button_set_label(GTK_BUTTON(btn_playlist_toggle), "-");
        gtk_widget_set_tooltip_text(btn_playlist_toggle, "Remove from Playlist");
        gtk_single_selection_set_selected(track_selection_model, found_idx);
    } else {
        gtk_button_set_label(GTK_BUTTON(btn_playlist_toggle), "+");
        gtk_widget_set_tooltip_text(btn_playlist_toggle, "Add to Playlist");
        gtk_single_selection_set_selected(track_selection_model, GTK_INVALID_LIST_POSITION);
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
        g_list_store_remove(track_list_store, existing_index);
    } else {
        char display_buf[512];
        bool is_native = extract_track_metadata(ui_state.selected_track_path, display_buf, sizeof(display_buf));
        QjTrack *new_track = qj_track_new(ui_state.selected_track_path, display_buf, is_native);
        g_list_store_append(track_list_store, new_track);
        g_object_unref(new_track);
    }

    save_current_playlist();
    update_playlist_toggle_state();
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

    strncpy(ui_state.selected_track_path, track->filepath, sizeof(ui_state.selected_track_path) - 1);
    g_object_unref(track);

    trigger_track_load();
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
    GtkWidget *label = gtk_label_new(NULL);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(label, 5);
    gtk_widget_set_margin_top(label, 5);
    gtk_widget_set_margin_bottom(label, 5);

    GtkDragSource *drag_source = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag_source, GDK_ACTION_MOVE);
    g_signal_connect(drag_source, "prepare", G_CALLBACK(on_drag_prepare), label);
    gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(drag_source));

    GtkDropTarget *drop_target = gtk_drop_target_new(QJ_TYPE_TRACK, GDK_ACTION_MOVE);
    g_signal_connect(drop_target, "drop", G_CALLBACK(on_drop), label);
    gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(drop_target));

    gtk_list_item_set_child(list_item, label);
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
    GtkWidget *label = gtk_list_item_get_child(list_item);
    g_object_set_data(G_OBJECT(label), "track", track);
    gtk_label_set_text(GTK_LABEL(label), track->display_name);
}

// --- QJAMS NATIVE RENAMING ---
typedef struct {
    QjTrack *track;
    GtkWidget *entry;
    GtkWidget *dialog;
} RenameData;

static void on_rename_confirm(GtkButton *btn, gpointer user_data) {
    (void)btn;
    RenameData *rd = (RenameData *)user_data;
    const char *new_name = gtk_editable_get_text(GTK_EDITABLE(rd->entry));

    if (strlen(new_name) > 0) {
        char *dir = g_path_get_dirname(rd->track->filepath);
        char *ext = strrchr(rd->track->filepath, '.');
        if (!ext) ext = "";

        char *new_path = g_strdup_printf("%s/%s%s", dir, new_name, ext);

        if (g_str_has_suffix(rd->track->filepath, ".flac")) {
            char *meta_title = g_strdup_printf("title=%s", new_name);
            char *argv[] = {"ffmpeg", "-y", "-i", rd->track->filepath, "-c", "copy", "-metadata", meta_title, new_path, NULL};
            g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
            g_free(meta_title);
            remove(rd->track->filepath); // Purge old physical FLAC
        } else {
            rename(rd->track->filepath, new_path); // Instant MKV file system rename
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
            gtk_single_selection_set_selected(track_selection_model, pos);
            g_object_unref(rd->track);
        }

        save_current_playlist();
        g_free(dir);

        // Ensure UI updates if the currently loaded track was just renamed
        if (g_strcmp0(ui_state.selected_track_path, rd->track->filepath) != 0) {
            strncpy(ui_state.selected_track_path, rd->track->filepath, sizeof(ui_state.selected_track_path) - 1);
            char ui_text[600];
            snprintf(ui_text, sizeof(ui_text), "Track: %s", display_buf);
            gtk_label_set_text(GTK_LABEL(lbl_track), ui_text);
        }
    }
    gtk_window_destroy(GTK_WINDOW(rd->dialog));
    free(rd);
}

static void on_rename_track_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    gpointer item = gtk_single_selection_get_selected_item(track_selection_model);
    if (!item) return;
    QjTrack *track = QJ_TRACK(item);
    if (!track->is_qjams_native) return;

    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Rename QJams Track");
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

    g_signal_connect(btn_confirm, "clicked", G_CALLBACK(on_rename_confirm), rd);

    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Enter new track name:"));
    gtk_box_append(GTK_BOX(vbox), entry);
    gtk_box_append(GTK_BOX(vbox), btn_confirm);

    gtk_window_set_child(GTK_WINDOW(dialog), vbox);
    gtk_window_present(GTK_WINDOW(dialog));
}

static void on_playlist_selection_changed(GObject *gobject, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    GtkWidget *btn_rename = GTK_WIDGET(user_data);
    gpointer item = gtk_single_selection_get_selected_item(GTK_SINGLE_SELECTION(gobject));
    if (item) {
        QjTrack *track = QJ_TRACK(item);
        gtk_widget_set_sensitive(btn_rename, track->is_qjams_native);
    } else {
        gtk_widget_set_sensitive(btn_rename, FALSE);
    }
}

/**
 * @brief Creates and initializes the main GTK playlist widget structure.
 * @return A pointer to the created GtkWidget (scrolled window containing the list).
 */
GtkWidget* create_playlist_widget(void) {
    track_list_store = g_list_store_new(QJ_TYPE_TRACK);
    track_selection_model = gtk_single_selection_new(G_LIST_MODEL(track_list_store));

    // Explicitly allow the UI to have zero active selections
    gtk_single_selection_set_can_unselect(track_selection_model, TRUE);

    GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
    g_signal_connect(factory, "setup", G_CALLBACK(on_setup_list_item), NULL);
    g_signal_connect(factory, "bind", G_CALLBACK(on_bind_list_item), NULL);

    GtkWidget *track_list_view = gtk_list_view_new(GTK_SELECTION_MODEL(track_selection_model), factory);
    g_signal_connect(track_list_view, "activate", G_CALLBACK(on_track_activated), NULL);

    GtkWidget *scrolled_window = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled_window, TRUE); // RESTORED
    gtk_widget_set_valign(scrolled_window, GTK_ALIGN_FILL);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled_window), 250);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled_window), track_list_view);

    // NEW: Header Bar for Playlist
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_margin_bottom(header_box, 5);

    GtkWidget *lbl_title = gtk_label_new("<b>Playlist</b>");
    gtk_label_set_use_markup(GTK_LABEL(lbl_title), TRUE);
    gtk_widget_set_hexpand(lbl_title, TRUE);
    gtk_widget_set_halign(lbl_title, GTK_ALIGN_FILL);

    GtkWidget *btn_rename_track = gtk_button_new_from_icon_name("document-edit-symbolic");
    gtk_widget_set_tooltip_text(btn_rename_track, "Rename QJams Track");
    gtk_widget_add_css_class(btn_rename_track, "flat");
    gtk_widget_set_sensitive(btn_rename_track, FALSE); // Disabled by default
    g_signal_connect(btn_rename_track, "clicked", G_CALLBACK(on_rename_track_clicked), NULL);

    GtkWidget *btn_sort = gtk_button_new_from_icon_name("view-sort-descending-symbolic");
    gtk_widget_set_tooltip_text(btn_sort, "Sort Alphabetically");
    gtk_widget_add_css_class(btn_sort, "flat");
    g_signal_connect(btn_sort, "clicked", G_CALLBACK(on_sort_playlist_clicked), NULL);

    g_signal_connect(track_selection_model, "notify::selected-item", G_CALLBACK(on_playlist_selection_changed), btn_rename_track);

    gtk_box_append(GTK_BOX(header_box), lbl_title);
    gtk_box_append(GTK_BOX(header_box), btn_rename_track);
    gtk_box_append(GTK_BOX(header_box), btn_sort);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    // NEW: Add internal margin so text doesn't hit the border
    gtk_widget_set_margin_start(main_box, 8);
    gtk_widget_set_margin_end(main_box, 8);
    gtk_widget_set_margin_top(main_box, 8);
    gtk_widget_set_margin_bottom(main_box, 8);

    gtk_box_append(GTK_BOX(main_box), header_box);
    gtk_box_append(GTK_BOX(main_box), scrolled_window);

    // NEW: Wrap the entire construct in a frame before handing it off to gui.c
    GtkWidget *frame = gtk_frame_new(NULL);
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

    return frame; // Return the frame wrapper
}
