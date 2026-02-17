#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

typedef struct {
    gboolean is_image;
    gchar *text;
    GdkPixbuf *image;
    gchar *preview;
    gchar *tooltip;
    gchar *identity;
} ClipboardHistoryEntry;

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

static void clipboard_history_entry_free(ClipboardHistoryEntry *entry) {
    if (entry == NULL) {
        return;
    }

    g_free(entry->text);
    g_clear_object(&entry->image);
    g_free(entry->preview);
    g_free(entry->tooltip);
    g_free(entry->identity);
    g_free(entry);
}

static gchar *build_image_identity(GdkPixbuf *pixbuf) {
    gsize pixel_bytes;
    gchar *checksum;

    pixel_bytes = (gsize)gdk_pixbuf_get_rowstride(pixbuf) * (gsize)gdk_pixbuf_get_height(pixbuf);
    checksum = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256,
        gdk_pixbuf_get_pixels(pixbuf),
        pixel_bytes
    );

    gchar *identity = g_strdup_printf(
        "image:%dx%d:%d:%d:%s",
        gdk_pixbuf_get_width(pixbuf),
        gdk_pixbuf_get_height(pixbuf),
        gdk_pixbuf_get_n_channels(pixbuf),
        gdk_pixbuf_get_has_alpha(pixbuf),
        checksum
    );

    g_free(checksum);
    return identity;
}

static ClipboardHistoryEntry *create_text_entry(const gchar *text) {
    ClipboardHistoryEntry *entry;
    gchar *normalized;

    if (text == NULL || *text == '\0') {
        return NULL;
    }

    normalized = normalize_text_whitespace(text);
    if (*normalized == '\0') {
        g_free(normalized);
        return NULL;
    }

    entry = g_new0(ClipboardHistoryEntry, 1);
    entry->is_image = FALSE;
    entry->text = g_strdup(text);
    entry->preview = shorten_history_label(text);
    entry->tooltip = g_strdup(text);
    entry->identity = g_strconcat("text:", text, NULL);
    g_free(normalized);

    return entry;
}

static ClipboardHistoryEntry *create_image_entry(GdkPixbuf *pixbuf) {
    ClipboardHistoryEntry *entry;
    gint width;
    gint height;

    if (pixbuf == NULL) {
        return NULL;
    }

    width = gdk_pixbuf_get_width(pixbuf);
    height = gdk_pixbuf_get_height(pixbuf);

    entry = g_new0(ClipboardHistoryEntry, 1);
    entry->is_image = TRUE;
    entry->image = g_object_ref(pixbuf);
    entry->preview = g_strdup_printf("Image %dx%d", width, height);
    entry->tooltip = g_strdup_printf("Clipboard image (%d x %d)", width, height);
    entry->identity = build_image_identity(pixbuf);

    return entry;
}

static void update_applet_display(ClipboardMonitor *monitor, ClipboardHistoryEntry *entry) {
    if (entry == NULL) {
        gtk_label_set_text(GTK_LABEL(monitor->label), "Empty");
        gtk_widget_set_tooltip_text(monitor->label, NULL);
        return;
    }

    gtk_label_set_text(GTK_LABEL(monitor->label), entry->preview);
    gtk_widget_set_tooltip_text(monitor->label, entry->tooltip);
}

static gboolean queue_promote_entry_by_identity(ClipboardMonitor *monitor, ClipboardHistoryEntry *entry) {
    GList *iter;

    for (iter = monitor->history->head; iter != NULL; iter = iter->next) {
        ClipboardHistoryEntry *existing = (ClipboardHistoryEntry *)iter->data;

        if (g_strcmp0(existing->identity, entry->identity) == 0) {
            g_queue_unlink(monitor->history, iter);
            g_queue_push_head_link(monitor->history, iter);
            update_applet_display(monitor, existing);
            clipboard_history_entry_free(entry);
            return TRUE;
        }
    }

    return FALSE;
}

static void on_history_item_activate(GtkMenuItem *item, gpointer user_data) {
    ClipboardMonitor *monitor = (ClipboardMonitor *)user_data;
    ClipboardHistoryEntry *entry;

    entry = (ClipboardHistoryEntry *)g_object_get_data(G_OBJECT(item), "clipboard-entry");
    if (entry == NULL) {
        return;
    }

    if (entry->is_image && entry->image != NULL) {
        gtk_clipboard_set_image(monitor->clipboard, entry->image);
    } else if (entry->text != NULL) {
        gtk_clipboard_set_text(monitor->clipboard, entry->text, -1);
    }
}

static gboolean on_plugin_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
    (void)widget;
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
            ClipboardHistoryEntry *entry = (ClipboardHistoryEntry *)iter->data;
            GtkWidget *item;

            if (entry->is_image && entry->image != NULL) {
                GtkWidget *box;
                GtkWidget *image;
                GtkWidget *label;
                GdkPixbuf *thumb;
                gint width;
                gint height;

                item = gtk_menu_item_new();
                box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

                width = gdk_pixbuf_get_width(entry->image);
                height = gdk_pixbuf_get_height(entry->image);

                if (width > height) {
                    thumb = gdk_pixbuf_scale_simple(entry->image, 32, MAX(1, 32 * height / width), GDK_INTERP_BILINEAR);
                } else {
                    thumb = gdk_pixbuf_scale_simple(entry->image, MAX(1, 32 * width / height), 32, GDK_INTERP_BILINEAR);
                }

                image = gtk_image_new_from_pixbuf(thumb);
                g_object_unref(thumb);
                label = gtk_label_new(entry->preview);
                gtk_label_set_xalign(GTK_LABEL(label), 0.0);

                gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
                gtk_container_add(GTK_CONTAINER(item), box);
            } else {
                item = gtk_menu_item_new_with_label(entry->preview);
            }

            gtk_widget_set_tooltip_text(item, entry->tooltip);
            g_object_set_data(G_OBJECT(item), "clipboard-entry", entry);
            g_signal_connect(item, "activate", G_CALLBACK(on_history_item_activate), monitor);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        }
    }

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);

    return TRUE;
}

static void on_text_received(GtkClipboard *clipboard, const gchar *text, gpointer user_data) {
    (void)clipboard;
    ClipboardMonitor *monitor = (ClipboardMonitor *)user_data;
    ClipboardHistoryEntry *entry;

    entry = create_text_entry(text);
    if (entry == NULL) {
        update_applet_display(monitor, NULL);
        return;
    }

    if (monitor->history != NULL && monitor->history->head != NULL) {
        ClipboardHistoryEntry *latest = (ClipboardHistoryEntry *)g_queue_peek_head(monitor->history);
        if (g_strcmp0(latest->identity, entry->identity) == 0) {
            update_applet_display(monitor, latest);
            clipboard_history_entry_free(entry);
            return;
        }
    }

    if (queue_promote_entry_by_identity(monitor, entry)) {
        return;
    }

    if (monitor->history != NULL) {
        g_queue_push_head(monitor->history, entry);

        while (g_queue_get_length(monitor->history) > monitor->max_items) {
            clipboard_history_entry_free((ClipboardHistoryEntry *)g_queue_pop_tail(monitor->history));
        }

        update_applet_display(monitor, (ClipboardHistoryEntry *)g_queue_peek_head(monitor->history));
    } else {
        clipboard_history_entry_free(entry);
        update_applet_display(monitor, NULL);
    }
}

static void on_image_received(GtkClipboard *clipboard, GdkPixbuf *pixbuf, gpointer user_data) {
    (void)clipboard;
    ClipboardMonitor *monitor = (ClipboardMonitor *)user_data;
    ClipboardHistoryEntry *entry;

    entry = create_image_entry(pixbuf);
    if (entry == NULL) {
        update_applet_display(monitor, NULL);
        return;
    }

    if (monitor->history != NULL && monitor->history->head != NULL) {
        ClipboardHistoryEntry *latest = (ClipboardHistoryEntry *)g_queue_peek_head(monitor->history);
        if (g_strcmp0(latest->identity, entry->identity) == 0) {
            update_applet_display(monitor, latest);
            clipboard_history_entry_free(entry);
            return;
        }
    }

    if (queue_promote_entry_by_identity(monitor, entry)) {
        return;
    }

    if (monitor->history != NULL) {
        g_queue_push_head(monitor->history, entry);

        while (g_queue_get_length(monitor->history) > monitor->max_items) {
            clipboard_history_entry_free((ClipboardHistoryEntry *)g_queue_pop_tail(monitor->history));
        }

        update_applet_display(monitor, (ClipboardHistoryEntry *)g_queue_peek_head(monitor->history));
    } else {
        clipboard_history_entry_free(entry);
        update_applet_display(monitor, NULL);
    }
}

static void on_clipboard_targets_received(GtkClipboard *clipboard, GdkAtom *targets, gint n_targets, gpointer user_data) {
    if (targets == NULL || n_targets <= 0) {
        update_applet_display((ClipboardMonitor *)user_data, NULL);
        return;
    }

    if (gtk_targets_include_image(targets, n_targets, FALSE)) {
        gtk_clipboard_request_image(clipboard, on_image_received, user_data);
    } else if (gtk_targets_include_text(targets, n_targets)) {
        gtk_clipboard_request_text(clipboard, on_text_received, user_data);
    } else {
        update_applet_display((ClipboardMonitor *)user_data, NULL);
    }
}

static void on_clipboard_owner_change(GtkClipboard *clipboard, GdkEvent *event, gpointer user_data) {
    (void)event;
    gtk_clipboard_request_targets(clipboard, on_clipboard_targets_received, user_data);
}

static void plugin_free(XfcePanelPlugin *plugin, ClipboardMonitor *monitor) {
    (void)plugin;
    if (monitor->history != NULL) {
        g_queue_free_full(monitor->history, (GDestroyNotify)clipboard_history_entry_free);
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
    gtk_clipboard_request_targets(monitor->clipboard, on_clipboard_targets_received, monitor);
}

XFCE_PANEL_PLUGIN_REGISTER(plugin_construct)
