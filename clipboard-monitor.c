#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

typedef struct {
    XfcePanelPlugin *plugin;
    GtkWidget *label;
    GtkClipboard *clipboard;
    GQueue *history;
    guint max_items;
} ClipboardMonitor;

#define HISTORY_PREVIEW_MAX_CHARS 60

static gchar *normalize_text_whitespace(const gchar *text) {
    GString *normalized;
    const gchar *p;
    gboolean last_was_space = TRUE;

    if (text == NULL) {
        return g_strdup("");
    }

    normalized = g_string_sized_new(64);
    p = text;
    while (*p != '\0') {
        gunichar ch = g_utf8_get_char(p);

        if (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ') {
            if (!last_was_space) {
                g_string_append_c(normalized, ' ');
                last_was_space = TRUE;
            }
        } else {
            g_string_append_unichar(normalized, ch);
            last_was_space = FALSE;
        }

        p = g_utf8_next_char(p);
    }

    if (normalized->len > 0 && normalized->str[normalized->len - 1] == ' ') {
        g_string_truncate(normalized, normalized->len - 1);
    }

    return g_string_free(normalized, FALSE);
}

static gchar *shorten_history_label(const gchar *text) {
    gchar *single_line;
    glong char_count;
    const gchar *truncate_at;

    if (text == NULL || *text == '\0') {
        return g_strdup("(empty)");
    }

    single_line = normalize_text_whitespace(text);
    if (*single_line == '\0') {
        g_free(single_line);
        return g_strdup("(empty)");
    }

    char_count = g_utf8_strlen(single_line, -1);
    if (char_count > HISTORY_PREVIEW_MAX_CHARS) {
        truncate_at = g_utf8_offset_to_pointer(single_line, HISTORY_PREVIEW_MAX_CHARS);
        gchar *truncated = g_strndup(single_line, truncate_at - single_line);
        gchar *preview = g_strconcat(truncated, "…", NULL);

        g_free(truncated);
        g_free(single_line);
        return preview;
    }

    return single_line;
}

static void on_history_item_activate(GtkMenuItem *item, gpointer user_data) {
    ClipboardMonitor *monitor = (ClipboardMonitor *)user_data;
    const gchar *selected_text;

    selected_text = (const gchar *)g_object_get_data(G_OBJECT(item), "clipboard-text");
    if (selected_text != NULL) {
        gtk_clipboard_set_text(monitor->clipboard, selected_text, -1);
    }
}

static gboolean on_plugin_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    ClipboardMonitor *monitor = (ClipboardMonitor *)user_data;
    GtkWidget *menu;

    if (event->type != GDK_BUTTON_PRESS || event->button != GDK_BUTTON_PRIMARY) {
        return FALSE;
    }

    menu = gtk_menu_new();

    if (monitor->history == NULL || g_queue_is_empty(monitor->history)) {
        GtkWidget *empty_item = gtk_menu_item_new_with_label("Clipboard history is empty");
        gtk_widget_set_sensitive(empty_item, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), empty_item);
    } else {
        GList *iter;

        for (iter = monitor->history->head; iter != NULL; iter = iter->next) {
            const gchar *entry_text = (const gchar *)iter->data;
            GtkWidget *item;
            gchar *preview;

            preview = shorten_history_label(entry_text);
            item = gtk_menu_item_new_with_label(preview);
            gtk_widget_set_tooltip_text(item, entry_text);
            g_object_set_data_full(G_OBJECT(item), "clipboard-text", g_strdup(entry_text), g_free);
            g_signal_connect(item, "activate", G_CALLBACK(on_history_item_activate), monitor);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
            g_free(preview);
        }
    }

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);

    return TRUE;
}

static void on_text_received(GtkClipboard *clipboard, const gchar *text, gpointer user_data) {
    ClipboardMonitor *monitor = (ClipboardMonitor *)user_data;

    if (text == NULL || *text == '\0') {
        gtk_label_set_text(GTK_LABEL(monitor->label), "Empty");
        return;
    }

    gchar *display_text = normalize_text_whitespace(text);

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
    gtk_widget_add_events(GTK_WIDGET(plugin), GDK_BUTTON_PRESS_MASK);
    g_signal_connect(plugin, "button-press-event", G_CALLBACK(on_plugin_button_press), monitor);
    g_signal_connect(monitor->clipboard, "owner-change", G_CALLBACK(on_clipboard_owner_change), monitor);
    g_signal_connect(plugin, "free-data", G_CALLBACK(plugin_free), monitor);
    gtk_widget_show_all(GTK_WIDGET(plugin));
    gtk_clipboard_request_text(monitor->clipboard, on_text_received, monitor);
}

XFCE_PANEL_PLUGIN_REGISTER(plugin_construct)
