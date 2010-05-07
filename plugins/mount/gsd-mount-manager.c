/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2009 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include <config.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <dbus/dbus-glib.h>

#ifdef HAVE_LIBNOTIFY
#include <libnotify/notify.h>
#endif

#include "gsd-mount-manager.h"

#define GSD_MOUNT_MANAGER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GSD_TYPE_MOUNT_MANAGER, GsdMountManagerPrivate))

struct GsdMountManagerPrivate
{
        GVolumeMonitor *monitor;
        gboolean coldplugging;
};

G_DEFINE_TYPE (GsdMountManager, gsd_mount_manager, G_TYPE_OBJECT)

static gpointer manager_object = NULL;

#if 0
static void
drive_connected_cb (GVolumeMonitor *monitor,
                    GDrive *drive,
                    GsdMountManager *manager)
{
        /* TODO: listen for the eject button */
}
#endif

static void
volume_mounted_cb (GObject *source_object, GAsyncResult *result, gpointer user_data)
{
        GsdMountManager *manager = GSD_MOUNT_MANAGER (user_data);
        GError *error = NULL;
        char *name;

        name = g_volume_get_name (G_VOLUME (source_object));

	if (!g_volume_mount_finish (G_VOLUME (source_object), result, &error)) {
                g_debug ("Failed to mount '%s': %s", name, error->message);

                /* Only display errors if we're hotplugging */
		if (!manager->priv->coldplugging && error->code != G_IO_ERROR_FAILED_HANDLED) {
                        char *primary;
                        GtkWidget *dialog;

			primary = g_strdup_printf (_("Unable to mount %s"), name);

                        dialog = gtk_message_dialog_new (NULL, 0,
                                                         GTK_MESSAGE_ERROR,
                                                         GTK_BUTTONS_CLOSE,
                                                         primary);

			g_free (primary);
                        gtk_message_dialog_format_secondary_text (GTK_MESSAGE_DIALOG (dialog), error->message);

                        gtk_dialog_run (GTK_DIALOG (dialog));
                        gtk_widget_destroy (dialog);
		}
		g_error_free (error);
	} else {
                g_debug ("Mounted '%s'", name);
        }

        g_free (name);
}

static void
volume_added_cb (GVolumeMonitor *monitor,
                 GVolume *volume,
                 GsdMountManager *manager)
{
        char *name;

        name = g_volume_get_name (volume);
        g_debug ("Volme '%s' added", name);

        if (g_volume_can_mount (volume) &&
            g_volume_should_automount (volume)) {
                GMountOperation *mount_op;

                g_debug ("Mounting '%s'", name);

                mount_op = gtk_mount_operation_new (NULL);
                g_volume_mount (volume, G_MOUNT_MOUNT_NONE,
                                mount_op, NULL,
                                volume_mounted_cb, manager);
        }

        g_free (name);
}

#ifdef HAVE_LIBNOTIFY

/* NOTE: This function needs to be synchroniced with what
 * moblin-devices-panel shows. */
static gboolean
_mount_is_wanted_device (GMount *mount)
{
        GDrive *drive;
        gboolean removable = TRUE;

        /* shadowed mounts are not supposed to be shown to user */
        if (g_mount_is_shadowed (mount)) {
                return FALSE;
        }

        /* we do not want to show non-removable drives */
        drive = g_mount_get_drive (mount);
        if (!drive) {
                return TRUE;
        }
        removable = g_drive_is_media_removable (drive);
        g_object_unref (drive);

        return removable;
}


static char*
_get_mount_name (GMount *mount)
{
        GVolume *volume;
        char *name = NULL;

        volume = g_mount_get_volume (mount);
        if (volume) {
                name = g_volume_get_identifier (volume,
                                                G_VOLUME_IDENTIFIER_KIND_LABEL);
                g_object_unref (volume);
        }
        if (!name) {
                name = g_mount_get_name (mount);
        }

        return name;
}

static void
_show_clicked_cb (NotifyNotification *notification,
                  gchar *action,
                  gpointer data)
{
        DBusGProxy *proxy;
        DBusGConnection *bus;

        bus = dbus_g_bus_get (DBUS_BUS_SESSION, NULL);
        if (!bus) {
                return;
        }
        proxy = dbus_g_proxy_new_for_name (bus,
                                           "org.moblin.UX.Shell.Toolbar",
                                           "/org/moblin/UX/Shell/Toolbar",
                                           "org.moblin.UX.Shell.Toolbar");
        dbus_g_proxy_call_no_reply (proxy, "ShowPanel",
                                    G_TYPE_STRING, "moblin-panel-devices",
                                    G_TYPE_INVALID,
                                    G_TYPE_INVALID);
        g_object_unref (proxy);
}

static void
_show_new_mount_notification (GMount *mount)
{
        char *name, *body, *summary;
        NotifyNotification *note;

        name = _get_mount_name (mount);
        /* TRANSLATORS: notification title */
        summary = _("USB plugged in");
        /* TRANSLATORS: notification text: placeholder is a mount or volume 
         * name, e.g. "Canon digital camera", "Kingston 32GB". */
        body = g_strdup_printf (_("%s has been plugged in. You can use "
                                  "the Devices panel to interact with it"),
                                name);

        note = notify_notification_new (summary, body, NULL, NULL);
        /* TRANSLATORS: notification action button */
        notify_notification_add_action (note,
                                        "show-panel", _("Show"),
                                        (NotifyActionCallback) _show_clicked_cb,
                                        NULL, NULL);

        notify_notification_show (note, NULL);

        g_object_set_data_full (G_OBJECT (mount),
                                "gsd-mount-note", note, g_object_unref);

        g_free (name);
        g_free (body);
}

static void
mount_changed_cb (GMount *mount,
                  gpointer data)
{
        NotifyNotification *note = g_object_get_data (G_OBJECT (mount),
                                                      "gsd-mount-note");

        if (note && !_mount_is_wanted_device (mount)) {
                /* mount is no longer wanted, possibly became shadowed.*/
                notify_notification_close (note, NULL);
                /* This will unref the note. */
                g_object_set_data (G_OBJECT (mount),
                                   "gsd-mount-note", NULL);
        } else if (!note && _mount_is_wanted_device (mount)) {
                /* not sure if this ever happens... */
                _show_new_mount_notification (mount);
        }
}

static void
mount_unmounted_cb (GMount *mount,
                    GsdMountManager *manager)
{
        NotifyNotification *note = g_object_get_data (G_OBJECT (mount),
                                                      "gsd-mount-note");
        if (note) {
                notify_notification_close (note, NULL);
                g_object_set_data (G_OBJECT (mount),
                                   "gsd-mount-note", NULL);
        }

        g_signal_handlers_disconnect_by_func (mount,
                                              mount_changed_cb,
                                              manager);
}

static void
mount_added_cb (GVolumeMonitor *monitor,
                GMount *mount,
                GsdMountManager *manager)
{
        g_signal_connect (mount, "changed",
                          G_CALLBACK (mount_changed_cb), manager);
        g_signal_connect (mount, "unmounted",
                          G_CALLBACK (mount_unmounted_cb), manager);
        if (_mount_is_wanted_device (mount)) {
                _show_new_mount_notification (mount);
        }
}

#else

static void
mount_added_cb (GVolumeMonitor *monitor,
                GMount *mount,
                GsdMountManager *manager)
{
        GFile *file;
        char *uri;

        file = g_mount_get_root (mount);
        uri = g_file_get_uri (file);

        g_debug ("%s mounted, starting file manager", uri);

        /* TODO: some sort of dialog */
        /* gtk_show_uri (NULL, uri, GDK_CURRENT_TIME, NULL); */

        g_free (uri);
        g_object_unref (file);
}

#endif

static void
mount_existing_volumes (GsdMountManager *manager)
{
        /* TODO: iterate over drives to hook up eject */
        GList *l;

        g_debug ("Mounting existing volumes");

        manager->priv->coldplugging = TRUE;

        l = g_volume_monitor_get_volumes (manager->priv->monitor);
        while (l) {
                GVolume *volume = l->data;
                GMount *mount;

                mount = g_volume_get_mount (volume);

                if (mount == NULL) {
                        volume_added_cb (manager->priv->monitor, volume, manager);
                } else {
                        g_object_unref (mount);
                }

                g_object_unref (volume);
                l = g_list_delete_link (l, l);
        }

        manager->priv->coldplugging = FALSE;
}

gboolean
gsd_mount_manager_start (GsdMountManager *manager,
                               GError               **error)
{
        g_debug ("Starting mount manager");

#if HAVE_LIBNOTIFY
        if (!notify_is_initted ())
                notify_init (PACKAGE);
#endif

        manager->priv->monitor = g_volume_monitor_get ();

#if 0
	g_signal_connect_object (manager->priv->monitor, "drive-connected",
				 G_CALLBACK (drive_connected_cb), manager, 0);
#endif
	g_signal_connect_object (manager->priv->monitor, "volume-added",
				 G_CALLBACK (volume_added_cb), manager, 0);
	g_signal_connect_object (manager->priv->monitor, "mount-added",
				 G_CALLBACK (mount_added_cb), manager, 0);

        /* TODO: handle eject buttons */

        mount_existing_volumes (manager);

        return TRUE;
}

void
gsd_mount_manager_stop (GsdMountManager *manager)
{
        g_debug ("Stopping mount manager");
}

static void
gsd_mount_manager_dispose (GObject *object)
{
        GsdMountManager *manager = GSD_MOUNT_MANAGER (object);

        if (manager->priv->monitor) {
                g_signal_handlers_disconnect_by_func
                        (manager->priv->monitor, volume_added_cb, manager);
                g_signal_handlers_disconnect_by_func
                        (manager->priv->monitor, mount_added_cb, manager);
                g_object_unref (manager->priv->monitor);
                manager->priv->monitor = NULL;
        }

        G_OBJECT_CLASS (gsd_mount_manager_parent_class)->dispose (object);
}

static void
gsd_mount_manager_init (GsdMountManager *manager)
{
        manager->priv = GSD_MOUNT_MANAGER_GET_PRIVATE (manager);
}

static void
gsd_mount_manager_finalize (GObject *object)
{
        GsdMountManager *mount_manager;

        g_return_if_fail (object != NULL);
        g_return_if_fail (GSD_IS_MOUNT_MANAGER (object));

        mount_manager = GSD_MOUNT_MANAGER (object);

        g_return_if_fail (mount_manager->priv != NULL);

        G_OBJECT_CLASS (gsd_mount_manager_parent_class)->finalize (object);
}

static void
gsd_mount_manager_class_init (GsdMountManagerClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);

        object_class->dispose = gsd_mount_manager_dispose;
        object_class->finalize = gsd_mount_manager_finalize;

        g_type_class_add_private (klass, sizeof (GsdMountManagerPrivate));
}

GsdMountManager *
gsd_mount_manager_new (void)
{
        if (manager_object != NULL) {
                g_object_ref (manager_object);
        } else {
                manager_object = g_object_new (GSD_TYPE_MOUNT_MANAGER, NULL);
                g_object_add_weak_pointer (manager_object,
                                           (gpointer *) &manager_object);
        }

        return GSD_MOUNT_MANAGER (manager_object);
}
