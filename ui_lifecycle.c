#include "ui_lifecycle.h"
#include "audio_core.h"
#include "audio_memory.h"
#include "config.h"
#include "ui_dashboard.h"
#include "ui_globals.h"
#include "ui_transport.h"
#include "video_engine.h"
#include <signal.h>
#include <string.h>

static void on_force_quit_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    int response = gtk_alert_dialog_choose_finish(alert, res, NULL);

    if (response == 0) {
        if (active_muxer_pid != 0) {
            kill(active_muxer_pid, SIGKILL);
            g_spawn_close_pid(active_muxer_pid);
            active_muxer_pid = 0;
        }
        GtkWidget *win = GTK_WIDGET(user_data);
        gtk_window_destroy(GTK_WINDOW(win));
    }
}

static bool force_quit_approved = false;
bool pending_quit_after_save = false;

static void on_unsaved_quit_response(GObject *source_object, GAsyncResult *res,
                                     gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    int response = gtk_alert_dialog_choose_finish(alert, res, NULL);
    GtkWidget *win = GTK_WIDGET(user_data);

    if (response == 0) {
        pending_quit_after_save = true;
        if (btn_save_session)
            g_signal_emit_by_name(btn_save_session, "clicked");
    } else if (response == 1) {
        char autosave_path[1024];
        snprintf(autosave_path, sizeof(autosave_path), "%s/.autosave.qjams",
                 ui_state.config.recordings_dir);
        remove(autosave_path);
        mark_session_saved();

        // Explicitly wipe un-exported video since the session is abandoned
        abort_active_video_preview(false);

        force_quit_approved = true;
        gtk_window_destroy(GTK_WINDOW(win));
    }
}

static void on_recording_quit_response(GObject *source_object, GAsyncResult *res,
                                       gpointer user_data) {
    GtkAlertDialog *alert = GTK_ALERT_DIALOG(source_object);
    int response = gtk_alert_dialog_choose_finish(alert, res, NULL);
    GtkWidget *win = GTK_WIDGET(user_data);

    if (response == 0) {
        on_stop_clicked(NULL, NULL);
        force_quit_approved = true;
        gtk_window_destroy(GTK_WINDOW(win));
    }
}

gboolean on_window_close(GtkWindow *window, gpointer user_data) {
    (void)user_data;

    if (force_quit_approved) {
        ui_state.config.window_width = gtk_widget_get_width(GTK_WIDGET(window));
        ui_state.config.window_height = gtk_widget_get_height(GTK_WIDGET(window));
        save_qjams_config(ui_state.config_path, &ui_state.config);
        return FALSE;
    }

    if (atomic_load_explicit(&get_transport_state()->is_recording, memory_order_acquire)) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Recording in Progress");
        gtk_alert_dialog_set_detail(
            alert,
            "A recording is currently active. Do you want to stop recording and quit, or cancel?");
        const char *buttons[] = {"Stop & Quit", "Cancel", NULL};
        gtk_alert_dialog_set_buttons(alert, buttons);
        gtk_alert_dialog_set_cancel_button(alert, 1);
        gtk_alert_dialog_set_default_button(alert, 1);

        gtk_alert_dialog_choose(alert, GTK_WINDOW(window), NULL, on_recording_quit_response,
                                window);
        g_object_unref(alert);
        return TRUE;
    }

    if (active_muxer_pid != 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Muxing in Progress");
        gtk_alert_dialog_set_detail(
            alert, "FFmpeg is currently processing the export in the background.\n\nDo you want to "
                   "wait for it to finish, or Force Quit and abandon the export?");
        const char *buttons[] = {"Force Quit", "Wait", NULL};
        gtk_alert_dialog_set_buttons(alert, buttons);
        gtk_alert_dialog_set_cancel_button(alert, 1);
        gtk_alert_dialog_set_default_button(alert, 1);

        gtk_alert_dialog_choose(alert, GTK_WINDOW(window), NULL, on_force_quit_response, window);
        g_object_unref(alert);
        return TRUE;
    }

    if (is_session_dirty() && pristine_frames > 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Unsaved Changes");
        gtk_alert_dialog_set_detail(alert, "You have unsaved audio overdubs on the canvas. If you "
                                           "quit now, your progress will be permanently lost.");

        const char *buttons[] = {"Save Session", "Discard & Quit", "Cancel", NULL};
        gtk_alert_dialog_set_buttons(alert, buttons);
        gtk_alert_dialog_set_cancel_button(alert, 2);
        gtk_alert_dialog_set_default_button(alert, 0);

        gtk_alert_dialog_choose(alert, GTK_WINDOW(window), NULL, on_unsaved_quit_response, window);
        g_object_unref(alert);
        return TRUE;
    }

    ui_state.config.window_width = gtk_widget_get_width(GTK_WIDGET(window));
    ui_state.config.window_height = gtk_widget_get_height(GTK_WIDGET(window));
    save_qjams_config(ui_state.config_path, &ui_state.config);
    return FALSE;
}
