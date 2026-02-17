#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

typedef struct {
    XfcePanelPlugin *plugin;
    GtkWidget *label;
    GtkClipboard *clipboard;
    GQueue *history;
    guint max_items;
} ClipboardMonitor;

static void on_text_received(GtkClipboard *clipboard, const gchar *text, gpointer user_data) {
    ClipboardMonitor *monitor = (ClipboardMonitor *)user_data;

    if (text == NULL || *text == '\0') {
        gtk_label_set_text(GTK_LABEL(monitor->label), "Empty");
        return;
    }

    gchar *display_text = g_strdup(text);
    gchar *p = display_text;
    while (*p) {
        if (*p == '\n' || *p == '\r' || *p == '\t') {
            *p = ' ';
        }
        p++;
    }

    if (*display_text == '\0') {
        g_free(display_text);
        gtk_label_set_text(GTK_LABEL(monitor->label), "Empty");
        return;
    }

    if (monitor->history != NULL && monitor->history->head != NULL) {
        const gchar *latest = (const gchar *)g_queue_peek_head(monitor->history);
        if (g_strcmp0(latest, text) == 0) {
            gtk_label_set_text(GTK_LABEL(monitor->label), display_text);
            gtk_widget_set_tooltip_text(monitor->label, text);
            g_free(display_text);
            return;
        }
    }

    if (monitor->history != NULL) {
        GList *existing = g_queue_find_custom(monitor->history, text, (GCompareFunc)g_strcmp0);
        if (existing != NULL) {
            gchar *existing_text = (gchar *)existing->data;
            g_queue_unlink(monitor->history, existing);
            g_queue_push_head(monitor->history, existing_text);
        } else {
            g_queue_push_head(monitor->history, g_strdup(text));
        }

        while (g_queue_get_length(monitor->history) > monitor->max_items) {
            g_free(g_queue_pop_tail(monitor->history));
        }
    }

    gtk_label_set_text(GTK_LABEL(monitor->label), display_text);
    gtk_widget_set_tooltip_text(monitor->label, text);
    g_free(display_text);
}

static void on_clipboard_owner_change(GtkClipboard *clipboard, GdkEvent *event, gpointer user_data) {
    gtk_clipboard_request_text(clipboard, on_text_received, user_data);
}

static void plugin_free(XfcePanelPlugin *plugin, ClipboardMonitor *monitor) {
    if (monitor->history != NULL) {
        g_queue_free_full(monitor->history, g_free);
    }
    g_free(monitor);
}

static void plugin_construct(XfcePanelPlugin *plugin) {
    ClipboardMonitor *monitor = g_new0(ClipboardMonitor, 1);
    monitor->plugin = plugin;
    monitor->label = gtk_label_new("Waiting...");
    gtk_label_set_xalign(GTK_LABEL(monitor->label), 0.0);
    gtk_label_set_yalign(GTK_LABEL(monitor->label), 0.5);
    gtk_label_set_width_chars(GTK_LABEL(monitor->label), 30);
    gtk_label_set_max_width_chars(GTK_LABEL(monitor->label), 90);
    gtk_label_set_ellipsize(GTK_LABEL(monitor->label), PANGO_ELLIPSIZE_END);
    gtk_container_add(GTK_CONTAINER(plugin), monitor->label);
    monitor->clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    monitor->history = g_queue_new();
    monitor->max_items = 30;
    g_signal_connect(monitor->clipboard, "owner-change", G_CALLBACK(on_clipboard_owner_change), monitor);
    g_signal_connect(plugin, "free-data", G_CALLBACK(plugin_free), monitor);
    gtk_widget_show_all(GTK_WIDGET(plugin));
    gtk_clipboard_request_text(monitor->clipboard, on_text_received, monitor);
}

XFCE_PANEL_PLUGIN_REGISTER(plugin_construct)
