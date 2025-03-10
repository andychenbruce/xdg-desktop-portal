/*
 * Copyright © 2018-2019 Red Hat, Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <glib/gi18n.h>
#include <gio/gunixfdlist.h>
#include <gio/gdesktopappinfo.h>
#include <stdio.h>

#include "xdp-request.h"
#include "xdp-permissions.h"
#include "pipewire.h"
#include "xdp-dbus.h"
#include "xdp-impl-dbus.h"
#include "xdp-utils.h"

#define PERMISSION_TABLE "devices"
#define PERMISSION_DEVICE_CAMERA "camera"

static XdpDbusImplLockdown *lockdown;
static XdpDbusImplAccess *access_impl;

typedef struct _Camera Camera;
typedef struct _CameraClass CameraClass;

struct _Camera
{
  XdpDbusCameraSkeleton parent_instance;

  PipeWireRemote *pipewire_remote;
  GSource *pipewire_source;
  GFileMonitor *pipewire_socket_monitor;
  int64_t connect_timestamps[10];
  int connect_timestamps_i;
  GHashTable *cameras;
};

struct _CameraClass
{
  XdpDbusCameraSkeletonClass parent_class;
};

static Camera *camera;

GType camera_get_type (void);
static void camera_iface_init (XdpDbusCameraIface *iface);

G_DEFINE_TYPE_WITH_CODE (Camera, camera, XDP_DBUS_TYPE_CAMERA_SKELETON,
                         G_IMPLEMENT_INTERFACE (XDP_DBUS_TYPE_CAMERA,
                                                camera_iface_init))

static gboolean
create_pipewire_remote (Camera *camera,
                        GError **error);

static gboolean
query_permission_sync (XdpRequest *request)
{
  XdpPermission permission;
  const char *app_id;
  gboolean allowed;

  app_id = (const char *)g_object_get_data (G_OBJECT (request), "app-id");
  permission = xdp_get_permission_sync (app_id, PERMISSION_TABLE, PERMISSION_DEVICE_CAMERA);
  if (permission == XDP_PERMISSION_ASK || permission == XDP_PERMISSION_UNSET)
    {
      g_auto(GVariantBuilder) opt_builder =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
      g_autofree char *title = NULL;
      g_autofree char *body = NULL;
      guint32 response = 2;
      g_autoptr(GVariant) results = NULL;
      g_autoptr(GError) error = NULL;
      g_autoptr(GAppInfo) info = NULL;
      g_autoptr(XdpDbusImplRequest) impl_request = NULL;

      if (app_id[0] != 0)
        {
          g_autofree char *desktop_id = g_strconcat (app_id, ".desktop", NULL);
          info = (GAppInfo*)g_desktop_app_info_new (desktop_id);
        }

      g_variant_builder_add (&opt_builder, "{sv}", "icon", g_variant_new_string ("camera-web-symbolic"));

      if (info)
        {
          title = g_strdup_printf (_("Allow %s to Use the Camera?"), g_app_info_get_display_name (info));
          body = g_strdup_printf (_("%s wants to access camera devices."), g_app_info_get_display_name (info));
        }
      else
        {
          title = g_strdup (_("Allow app to Use the Camera?"));
          body = g_strdup (_("An app wants to access camera devices."));
        }

      impl_request = xdp_dbus_impl_request_proxy_new_sync (g_dbus_proxy_get_connection (G_DBUS_PROXY (access_impl)),
                                                           G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                                           g_dbus_proxy_get_name (G_DBUS_PROXY (access_impl)),
                                                           request->id,
                                                           NULL, &error);
      if (!impl_request)
        return FALSE;

      xdp_request_set_impl_request (request, impl_request);

      g_debug ("Calling backend for device access to camera");

      if (!xdp_dbus_impl_access_call_access_dialog_sync (access_impl,
                                                         request->id,
                                                         app_id,
                                                         "",
                                                         title,
                                                         "",
                                                         body,
                                                         g_variant_builder_end (&opt_builder),
                                                         &response,
                                                         &results,
                                                         NULL,
                                                         &error))
        {
          g_warning ("A backend call failed: %s", error->message);
          /* We do not want to set the permission if there was an error and the
           * permission was UNSET. Setting a permission should only be done from
           * user input. */
          return FALSE;
        }

      allowed = response == 0;

      if (permission == XDP_PERMISSION_UNSET)
        {
          xdp_set_permission_sync (app_id, PERMISSION_TABLE, PERMISSION_DEVICE_CAMERA,
                               allowed ? XDP_PERMISSION_YES : XDP_PERMISSION_NO);
        }
    }
  else
    allowed = permission == XDP_PERMISSION_YES ? TRUE : FALSE;

  return allowed;
}

static void
handle_access_camera_in_thread_func (GTask *task,
                                     gpointer source_object,
                                     gpointer task_data,
                                     GCancellable *cancellable)
{
  XdpRequest *request = XDP_REQUEST (task_data);
  gboolean allowed;

  allowed = query_permission_sync (request);

  REQUEST_AUTOLOCK (request);

  if (request->exported)
    {
      g_auto(GVariantBuilder) results =
        G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
      guint32 response;

      response = allowed ? XDG_DESKTOP_PORTAL_RESPONSE_SUCCESS
                         : XDG_DESKTOP_PORTAL_RESPONSE_CANCELLED;
      g_debug ("Camera: sending response %d", response);
      xdp_dbus_request_emit_response (XDP_DBUS_REQUEST (request),
                                      response,
                                      g_variant_builder_end (&results));
      xdp_request_unexport (request);
    }
}

static const char *
app_id_from_app_info (XdpAppInfo *app_info)
{
  /* Automatically grant camera access to unsandboxed apps. */
  if (xdp_app_info_is_host (app_info))
    return "";

  return xdp_app_info_get_id (app_info);
}

static gboolean
handle_access_camera (XdpDbusCamera *object,
                      GDBusMethodInvocation *invocation,
                      GVariant *arg_options)
{
  XdpRequest *request = xdp_request_from_invocation (invocation);
  const char *app_id;
  g_autoptr(GTask) task = NULL;

  if (xdp_dbus_impl_lockdown_get_disable_camera (lockdown))
    {
      g_debug ("Camera access disabled");
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Camera access disabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  REQUEST_AUTOLOCK (request);

  app_id = app_id_from_app_info (request->app_info);
  g_object_set_data_full (G_OBJECT (request), "app-id", g_strdup (app_id), g_free);

  xdp_request_export (request, g_dbus_method_invocation_get_connection (invocation));

  xdp_dbus_camera_complete_access_camera (object, invocation, request->id);

  task = g_task_new (object, NULL, NULL, NULL);
  g_task_set_task_data (task, g_object_ref (request), g_object_unref);
  g_task_run_in_thread (task, handle_access_camera_in_thread_func);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static PipeWireRemote *
open_pipewire_camera_remote (const char *app_id,
                             GError **error)
{
  PipeWireRemote *remote;
  struct pw_permission permission_items[3];
  struct pw_properties *pipewire_properties;

  pipewire_properties =
    pw_properties_new ("pipewire.access.portal.app_id", app_id,
                       "pipewire.access.portal.media_roles", "Camera",
                       NULL);
  remote = pipewire_remote_new_sync (pipewire_properties,
                                     NULL, NULL, NULL, NULL,
                                     error);
  if (!remote)
    return NULL;

  /*
   * Hide all existing and future nodes by default. PipeWire will use the
   * permission store to set up permissions.
   */
  permission_items[0] = PW_PERMISSION_INIT (PW_ID_CORE, PW_PERM_RWX);
  permission_items[1] = PW_PERMISSION_INIT (remote->node_factory_id, PW_PERM_R);
  permission_items[2] = PW_PERMISSION_INIT (PW_ID_ANY, 0);

  pw_client_update_permissions (pw_core_get_client(remote->core),
                                G_N_ELEMENTS (permission_items),
                                permission_items);

  pipewire_remote_roundtrip (remote);

  return remote;
}

static gboolean
handle_open_pipewire_remote (XdpDbusCamera *object,
                             GDBusMethodInvocation *invocation,
                             GUnixFDList *in_fd_list,
                             GVariant *arg_options)
{
  g_autoptr(XdpAppInfo) app_info = NULL;
  const char *app_id;
  XdpPermission permission;
  g_autoptr(GUnixFDList) out_fd_list = NULL;
  int fd;
  int fd_id;
  g_autoptr(GError) error = NULL;
  PipeWireRemote *remote;

  if (xdp_dbus_impl_lockdown_get_disable_camera (lockdown))
    {
      g_debug ("Camera access disabled");
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Camera access disabled");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  app_info = xdp_invocation_ensure_app_info_sync (invocation, NULL, &error);
  app_id = app_id_from_app_info (app_info);
  permission = xdp_get_permission_sync (app_id, PERMISSION_TABLE, PERMISSION_DEVICE_CAMERA);
  if (permission != XDP_PERMISSION_YES)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                             "Permission denied");
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  remote = open_pipewire_camera_remote (app_id, &error);
  if (!remote)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Failed to open PipeWire remote: %s",
                                             error->message);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  out_fd_list = g_unix_fd_list_new ();
  fd = pw_core_steal_fd (remote->core);
  fd_id = g_unix_fd_list_append (out_fd_list, fd, &error);
  close (fd);
  pipewire_remote_destroy (remote);

  if (fd_id == -1)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             XDG_DESKTOP_PORTAL_ERROR,
                                             XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                             "Failed to append fd: %s",
                                             error->message);
      return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

  xdp_dbus_camera_complete_open_pipewire_remote (object, invocation,
                                                 out_fd_list,
                                                 g_variant_new_handle (fd_id));
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
camera_iface_init (XdpDbusCameraIface *iface)
{
  iface->handle_access_camera = handle_access_camera;
  iface->handle_open_pipewire_remote = handle_open_pipewire_remote;
}

static void
global_added_cb (PipeWireRemote *remote,
                 uint32_t id,
                 const char *type,
                 const struct spa_dict *props,
                 gpointer user_data)
{
  Camera *camera = user_data;
  const struct spa_dict_item *media_class;
  const struct spa_dict_item *media_role;

  if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
    return;

  if (!props)
    return;

  media_class = spa_dict_lookup_item (props, PW_KEY_MEDIA_CLASS);
  if (!media_class)
    return;

  if (g_strcmp0 (media_class->value, "Video/Source") != 0)
    return;

  media_role = spa_dict_lookup_item (props, PW_KEY_MEDIA_ROLE);
  if (!media_role)
    return;

  if (g_strcmp0 (media_role->value, "Camera") != 0)
    return;

  g_hash_table_add (camera->cameras, GINT_TO_POINTER (id));

  xdp_dbus_camera_set_is_camera_present (XDP_DBUS_CAMERA (camera),
                                         g_hash_table_size (camera->cameras) > 0);
}

static void global_removed_cb (PipeWireRemote *remote,
                               uint32_t id,
                               gpointer user_data)
{
  Camera *camera = user_data;

  g_hash_table_remove (camera->cameras, GINT_TO_POINTER (id));

  xdp_dbus_camera_set_is_camera_present (XDP_DBUS_CAMERA (camera),
                                         g_hash_table_size (camera->cameras) > 0);
}

static void
pipewire_remote_error_cb (gpointer data,
                          gpointer user_data)
{
  Camera *camera = user_data;
  g_autoptr(GError) error = NULL;

  g_hash_table_remove_all (camera->cameras);
  xdp_dbus_camera_set_is_camera_present (XDP_DBUS_CAMERA (camera), FALSE);

  g_clear_pointer (&camera->pipewire_source, g_source_destroy);
  g_clear_pointer (&camera->pipewire_remote, pipewire_remote_destroy);

  if (!create_pipewire_remote (camera, &error))
    g_warning ("Failed connect to PipeWire: %s", error->message);
}

static gboolean
create_pipewire_remote (Camera *camera,
                        GError **error)
{
  struct pw_properties *pipewire_properties;
  const int n_connect_retries = G_N_ELEMENTS (camera->connect_timestamps);
  int64_t now;
  int max_retries_ago_i;
  int64_t max_retries_ago;

  now = g_get_monotonic_time ();
  camera->connect_timestamps[camera->connect_timestamps_i] = now;

  max_retries_ago_i = (camera->connect_timestamps_i + 1) % n_connect_retries;
  max_retries_ago = camera->connect_timestamps[max_retries_ago_i];

  camera->connect_timestamps_i =
    (camera->connect_timestamps_i + 1) % n_connect_retries;

  if (max_retries_ago &&
      now - max_retries_ago < G_USEC_PER_SEC * 10)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Tried to reconnect to PipeWire too often, giving up");
      return FALSE;
    }

  pipewire_properties = pw_properties_new ("pipewire.access.portal.is_portal", "true",
                                           "portal.monitor", "Camera",
                                           NULL);
  camera->pipewire_remote = pipewire_remote_new_sync (pipewire_properties,
                                                      global_added_cb,
                                                      global_removed_cb,
                                                      pipewire_remote_error_cb,
                                                      camera,
                                                      error);
  if (!camera->pipewire_remote)
    return FALSE;

  camera->pipewire_source =
    pipewire_remote_create_source (camera->pipewire_remote);

  return TRUE;
}

static void
on_pipewire_socket_changed (GFileMonitor *monitor,
                            GFile *file,
                            GFile *other_file,
                            GFileMonitorEvent event_type,
                            Camera *camera)
{
  g_autoptr(GError) error = NULL;

  if (event_type != G_FILE_MONITOR_EVENT_CREATED)
    return;

  if (camera->pipewire_remote)
    {
      g_debug ("PipeWire socket created after remote was created");
      return;
    }

  g_debug ("PipeWireSocket created, tracking cameras");

  if (!create_pipewire_remote (camera, &error))
    g_warning ("Failed connect to PipeWire: %s", error->message);
}

static gboolean
init_camera_tracker (Camera *camera,
                     GError **error)
{
  g_autofree char *pipewire_socket_path = NULL;
  g_autoptr(GFile) pipewire_socket = NULL;
  g_autoptr(GError) local_error = NULL;

  pipewire_socket_path = g_strdup_printf ("%s/pipewire-0",
                                          g_get_user_runtime_dir ());
  pipewire_socket = g_file_new_for_path (pipewire_socket_path);
  camera->pipewire_socket_monitor =
    g_file_monitor_file (pipewire_socket, G_FILE_MONITOR_NONE, NULL, error);
  if (!camera->pipewire_socket_monitor)
    return FALSE;

  g_signal_connect (camera->pipewire_socket_monitor,
                    "changed",
                    G_CALLBACK (on_pipewire_socket_changed),
                    camera);

  camera->cameras = g_hash_table_new (NULL, NULL);

  if (!create_pipewire_remote (camera, &local_error))
    g_warning ("Failed connect to PipeWire: %s", local_error->message);

  return TRUE;
}

static void
camera_finalize (GObject *object)
{
  Camera *camera = (Camera *)object;

  g_clear_pointer (&camera->pipewire_source, g_source_destroy);
  g_clear_pointer (&camera->pipewire_remote, pipewire_remote_destroy);
  g_clear_pointer (&camera->cameras, g_hash_table_unref);

  G_OBJECT_CLASS (camera_parent_class)->finalize (object);
}

static void
camera_init (Camera *camera)
{
  g_autoptr(GError) error = NULL;

  xdp_dbus_camera_set_version (XDP_DBUS_CAMERA (camera), 1);

  if (!init_camera_tracker (camera, &error))
    g_warning ("Failed to track cameras: %s", error->message);
}

static void
camera_class_init (CameraClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = camera_finalize;
}

GDBusInterfaceSkeleton *
camera_create (GDBusConnection *connection,
               const char      *access_impl_dbus_name,
               gpointer         lockdown_proxy)
{
  g_autoptr(GError) error = NULL;

  lockdown = lockdown_proxy;

  camera = g_object_new (camera_get_type (), NULL);

  access_impl = xdp_dbus_impl_access_proxy_new_sync (connection,
                                                     G_DBUS_PROXY_FLAGS_NONE,
                                                     access_impl_dbus_name,
                                                     DESKTOP_PORTAL_OBJECT_PATH,
                                                     NULL,
                                                     &error);
  if (access_impl == NULL)
    {
      g_warning ("Failed to create access proxy: %s", error->message);
      return NULL;
    }

  g_dbus_proxy_set_default_timeout (G_DBUS_PROXY (access_impl), G_MAXINT);

  return G_DBUS_INTERFACE_SKELETON (camera);
}
