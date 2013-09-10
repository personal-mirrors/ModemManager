/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * libmm -- Access modem status & information from glib applications
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2013 Google, Inc.
 */

#include <gio/gio.h>
#include <string.h>

#include "mm-helpers.h"
#include "mm-errors-types.h"
#include "mm-modem-oma.h"
#include "mm-common-helpers.h"

/**
 * SECTION: mm-modem-oma
 * @title: MMModemOma
 * @short_description: The OMA interface
 *
 * The #MMModemOma is an object providing access to the methods, signals and
 * properties of the OMA interface.
 *
 * The OMA interface is exposed whenever a modem has OMA device management capabilities.
 */

G_DEFINE_TYPE (MMModemOma, mm_modem_oma, MM_GDBUS_TYPE_MODEM_OMA_PROXY)

struct _MMModemOmaPrivate {
    /* Supported Modes */
    GMutex pending_network_initiated_sessions_mutex;
    guint pending_network_initiated_sessions_id;
    GArray *pending_network_initiated_sessions;
};

/*****************************************************************************/

/**
 * mm_modem_oma_get_path:
 * @self: A #MMModemOma.
 *
 * Gets the DBus path of the #MMObject which implements this interface.
 *
 * Returns: (transfer none): The DBus path of the #MMObject object.
 */
const gchar *
mm_modem_oma_get_path (MMModemOma *self)
{
    g_return_val_if_fail (MM_IS_MODEM_OMA (self), NULL);

    RETURN_NON_EMPTY_CONSTANT_STRING (
        g_dbus_proxy_get_object_path (G_DBUS_PROXY (self)));
}

/**
 * mm_modem_oma_dup_path:
 * @self: A #MMModemOma.
 *
 * Gets a copy of the DBus path of the #MMObject object which implements this interface.
 *
 * Returns: (transfer full): The DBus path of the #MMObject. The returned value should be freed with g_free().
 */
gchar *
mm_modem_oma_dup_path (MMModemOma *self)
{
    gchar *value;

    g_return_val_if_fail (MM_IS_MODEM_OMA (self), NULL);

    g_object_get (G_OBJECT (self),
                  "g-object-path", &value,
                  NULL);
    RETURN_NON_EMPTY_STRING (value);
}

/*****************************************************************************/

/**
 * mm_modem_oma_setup_finish:
 * @self: A #MMModemOma.
 * @res: The #GAsyncResult obtained from the #GAsyncReadyCallback passed to mm_modem_oma_setup().
 * @error: Return location for error or %NULL.
 *
 * Finishes an operation started with mm_modem_oma_setup().
 *
 * Returns: %TRUE if the setup was successful, %FALSE if @error is set.
 */
gboolean
mm_modem_oma_setup_finish (MMModemOma *self,
                           GAsyncResult *res,
                           GError **error)
{
    g_return_val_if_fail (MM_IS_MODEM_OMA (self), FALSE);

    return mm_gdbus_modem_oma_call_setup_finish (MM_GDBUS_MODEM_OMA (self), res, error);
}

/**
 * mm_modem_oma_setup:
 * @self: A #MMModemOma.
 * @features: Mask of #MMOmaFeatures to enable.
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @callback: A #GAsyncReadyCallback to call when the request is satisfied or %NULL.
 * @user_data: User data to pass to @callback.
 *
 * Asynchronously sets up the OMA device management service.
 *
 * When the operation is finished, @callback will be invoked in the <link linkend="g-main-context-push-thread-default">thread-default main loop</link> of the thread you are calling this method from.
 * You can then call mm_modem_oma_setup_finish() to get the result of the operation.
 *
 * See mm_modem_oma_setup_sync() for the synchronous, blocking version of this method.
 */
void
mm_modem_oma_setup (MMModemOma *self,
                    MMOmaFeature features,
                    GCancellable *cancellable,
                    GAsyncReadyCallback callback,
                    gpointer user_data)
{
    g_return_if_fail (MM_IS_MODEM_OMA (self));

    mm_gdbus_modem_oma_call_setup (MM_GDBUS_MODEM_OMA (self), features, cancellable, callback, user_data);
}

/**
 * mm_modem_oma_setup_sync:
 * @self: A #MMModemOma.
 * @features: Mask of #MMOmaFeatures to enable.
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @error: Return location for error or %NULL.
 *
 * Synchronously sets up the OMA device management service.
 *
 * The calling thread is blocked until a reply is received. See mm_modem_oma_setup()
 * for the asynchronous version of this method.
 *
 * Returns: %TRUE if the setup was successful, %FALSE if @error is set.
 */
gboolean
mm_modem_oma_setup_sync (MMModemOma *self,
                         MMOmaFeature features,
                         GCancellable *cancellable,
                         GError **error)
{
    g_return_val_if_fail (MM_IS_MODEM_OMA (self), FALSE);

    return mm_gdbus_modem_oma_call_setup_sync (MM_GDBUS_MODEM_OMA (self), features, cancellable, error);
}

/*****************************************************************************/

/**
 * mm_modem_oma_start_client_initiated_session_finish:
 * @self: A #MMModemOma.
 * @res: The #GAsyncResult obtained from the #GAsyncReadyCallback passed to mm_modem_oma_start_client_initiated_session().
 * @error: Return location for error or %NULL.
 *
 * Finishes an operation started with mm_modem_oma_start_client_initiated_session().
 *
 * Returns: %TRUE if the session was started, %FALSE if @error is set.
 */
gboolean
mm_modem_oma_start_client_initiated_session_finish (MMModemOma *self,
                                                    GAsyncResult *res,
                                                    GError **error)
{
    g_return_val_if_fail (MM_IS_MODEM_OMA (self), FALSE);

    return mm_gdbus_modem_oma_call_start_client_initiated_session_finish (MM_GDBUS_MODEM_OMA (self), res, error);
}

/**
 * mm_modem_oma_start_client_initiated_session:
 * @self: A #MMModemOma.
 * @session_type: A #MMOmaSessionType.
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @callback: A #GAsyncReadyCallback to call when the request is satisfied or %NULL.
 * @user_data: User data to pass to @callback.
 *
 * Asynchronously starts a client-initiated OMA device management session.
 *
 * When the operation is finished, @callback will be invoked in the <link linkend="g-main-context-push-thread-default">thread-default main loop</link> of the thread you are calling this method from.
 * You can then call mm_modem_oma_start_client_initiated_session_finish() to get the result of the operation.
 *
 * See mm_modem_oma_start_client_initiated_session_sync() for the synchronous, blocking version of this method.
 */
void
mm_modem_oma_start_client_initiated_session (MMModemOma *self,
                                             MMOmaSessionType session_type,
                                             GCancellable *cancellable,
                                             GAsyncReadyCallback callback,
                                             gpointer user_data)
{
    g_return_if_fail (MM_IS_MODEM_OMA (self));

    mm_gdbus_modem_oma_call_start_client_initiated_session (MM_GDBUS_MODEM_OMA (self), session_type, cancellable, callback, user_data);
}

/**
 * mm_modem_oma_start_client_initiated_session_sync:
 * @self: A #MMModemOma.
 * @session_type: A #MMOmaSessionType.
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @error: Return location for error or %NULL.
 *
 * Synchronously starts a client-initiated OMA device management session.
 *
 * The calling thread is blocked until a reply is received. See mm_modem_oma_start_client_initiated_session()
 * for the asynchronous version of this method.
 *
 * Returns: %TRUE if the session was started, %FALSE if @error is set.
 */
gboolean
mm_modem_oma_start_client_initiated_session_sync (MMModemOma *self,
                                                  MMOmaSessionType session_type,
                                                  GCancellable *cancellable,
                                                  GError **error)
{
    g_return_val_if_fail (MM_IS_MODEM_OMA (self), FALSE);

    return mm_gdbus_modem_oma_call_start_client_initiated_session_sync (MM_GDBUS_MODEM_OMA (self), session_type, cancellable, error);
}

/*****************************************************************************/

/**
 * mm_modem_oma_accept_network_initiated_session_finish:
 * @self: A #MMModemOma.
 * @res: The #GAsyncResult obtained from the #GAsyncReadyCallback passed to mm_modem_oma_accept_network_initiated_session().
 * @error: Return location for error or %NULL.
 *
 * Finishes an operation started with mm_modem_oma_accept_network_initiated_session().
 *
 * Returns: %TRUE if the session was started, %FALSE if @error is set.
 */
gboolean
mm_modem_oma_accept_network_initiated_session_finish (MMModemOma *self,
                                                      GAsyncResult *res,
                                                      GError **error)
{
    g_return_val_if_fail (MM_IS_MODEM_OMA (self), FALSE);

    return mm_gdbus_modem_oma_call_accept_network_initiated_session_finish (MM_GDBUS_MODEM_OMA (self), res, error);
}

/**
 * mm_modem_oma_accept_network_initiated_session:
 * @self: A #MMModemOma.
 * @session_id: The unique ID of the network-initiated session.
 * @accept: %TRUE if the session is to be accepted, %FALSE otherwise.
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @callback: A #GAsyncReadyCallback to call when the request is satisfied or %NULL.
 * @user_data: User data to pass to @callback.
 *
 * Asynchronously accepts a nework-initiated OMA device management session.
 *
 * When the operation is finished, @callback will be invoked in the <link linkend="g-main-context-push-thread-default">thread-default main loop</link> of the thread you are calling this method from.
 * You can then call mm_modem_oma_accept_network_initiated_session_finish() to get the result of the operation.
 *
 * See mm_modem_oma_accept_network_initiated_session_sync() for the synchronous, blocking version of this method.
 */
void
mm_modem_oma_accept_network_initiated_session (MMModemOma *self,
                                               guint session_id,
                                               gboolean accept,
                                               GCancellable *cancellable,
                                               GAsyncReadyCallback callback,
                                               gpointer user_data)
{
    g_return_if_fail (MM_IS_MODEM_OMA (self));

    mm_gdbus_modem_oma_call_accept_network_initiated_session (MM_GDBUS_MODEM_OMA (self), session_id, accept, cancellable, callback, user_data);
}

/**
 * mm_modem_oma_accept_network_initiated_session_sync:
 * @self: A #MMModemOma.
 * @session_id: The unique ID of the network-initiated session.
 * @accept: %TRUE if the session is to be accepted, %FALSE otherwise.
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @error: Return location for error or %NULL.
 *
 * Synchronously accepts a nework-initiated OMA device management session.
 *
 * The calling thread is blocked until a reply is received. See mm_modem_oma_accept_network_initiated_session()
 * for the asynchronous version of this method.
 *
 * Returns: %TRUE if the session was started, %FALSE if @error is set.
 */
gboolean
mm_modem_oma_accept_network_initiated_session_sync (MMModemOma *self,
                                                    guint session_id,
                                                    gboolean accept,
                                                    GCancellable *cancellable,
                                                    GError **error)
{
    g_return_val_if_fail (MM_IS_MODEM_OMA (self), FALSE);

    return mm_gdbus_modem_oma_call_accept_network_initiated_session_sync (MM_GDBUS_MODEM_OMA (self), session_id, accept, cancellable, error);
}

/*****************************************************************************/

/**
 * mm_modem_oma_cancel_session_finish:
 * @self: A #MMModemOma.
 * @res: The #GAsyncResult obtained from the #GAsyncReadyCallback passed to mm_modem_oma_cancel_session().
 * @error: Return location for error or %NULL.
 *
 * Finishes an operation started with mm_modem_oma_cancel_session().
 *
 * Returns: %TRUE if the session was started, %FALSE if @error is set.
 */
gboolean
mm_modem_oma_cancel_session_finish (MMModemOma *self,
                                    GAsyncResult *res,
                                    GError **error)
{
    g_return_val_if_fail (MM_IS_MODEM_OMA (self), FALSE);

    return mm_gdbus_modem_oma_call_cancel_session_finish (MM_GDBUS_MODEM_OMA (self), res, error);
}

/**
 * mm_modem_oma_cancel_session:
 * @self: A #MMModemOma.
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @callback: A #GAsyncReadyCallback to call when the request is satisfied or %NULL.
 * @user_data: User data to pass to @callback.
 *
 * Asynchronously cancels the current OMA device management session.
 *
 * When the operation is finished, @callback will be invoked in the <link linkend="g-main-context-push-thread-default">thread-default main loop</link> of the thread you are calling this method from.
 * You can then call mm_modem_oma_cancel_session_finish() to get the result of the operation.
 *
 * See mm_modem_oma_cancel_session_sync() for the synchronous, blocking version of this method.
 */
void
mm_modem_oma_cancel_session (MMModemOma *self,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback,
                             gpointer user_data)
{
    g_return_if_fail (MM_IS_MODEM_OMA (self));

    mm_gdbus_modem_oma_call_cancel_session (MM_GDBUS_MODEM_OMA (self), cancellable, callback, user_data);
}

/**
 * mm_modem_oma_cancel_session_sync:
 * @self: A #MMModemOma.
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @error: Return location for error or %NULL.
 *
 * Synchronously cancels the current OMA device management session.
 *
 * The calling thread is blocked until a reply is received. See mm_modem_oma_cancel_session()
 * for the asynchronous version of this method.
 *
 * Returns: %TRUE if the session was started, %FALSE if @error is set.
 */
gboolean
mm_modem_oma_cancel_session_sync (MMModemOma *self,
                                  GCancellable *cancellable,
                                  GError **error)
{
    g_return_val_if_fail (MM_IS_MODEM_OMA (self), FALSE);

    return mm_gdbus_modem_oma_call_cancel_session_sync (MM_GDBUS_MODEM_OMA (self), cancellable, error);
}

/*****************************************************************************/

/**
 * mm_modem_oma_get_features:
 * @self: A #MMModemOma.
 *
 * Gets the currently enabled OMA features.
 *
 * Returns: a bitmask of #MMOmaFeature values.
 */
MMOmaFeature
mm_modem_oma_get_features  (MMModemOma *self)
{
    g_return_val_if_fail (MM_IS_MODEM_OMA (self), MM_OMA_SESSION_TYPE_UNKNOWN);

    return (MMOmaFeature) mm_gdbus_modem_oma_get_features (MM_GDBUS_MODEM_OMA (self));
}

/*****************************************************************************/

/**
 * mm_modem_oma_get_session_type:
 * @self: A #MMModemOma.
 *
 * Gets the type of the current OMA device management session.
 *
 * Returns: a #MMOmaSessionType.
 */
MMOmaSessionType
mm_modem_oma_get_session_type  (MMModemOma *self)
{
    g_return_val_if_fail (MM_IS_MODEM_OMA (self), MM_OMA_SESSION_TYPE_UNKNOWN);

    return (MMOmaSessionType) mm_gdbus_modem_oma_get_session_type (MM_GDBUS_MODEM_OMA (self));
}

/*****************************************************************************/

/**
 * mm_modem_oma_get_session_state:
 * @self: A #MMModemOma.
 *
 * Gets the state of the current OMA device management session.
 *
 * Returns: a #MMOmaSessionState.
 */
MMOmaSessionState
mm_modem_oma_get_session_state (MMModemOma *self)
{
    g_return_val_if_fail (MM_IS_MODEM_OMA (self), MM_OMA_SESSION_STATE_UNKNOWN);

    return (MMOmaSessionState) mm_gdbus_modem_oma_get_session_state (MM_GDBUS_MODEM_OMA (self));
}

/*****************************************************************************/

static void
pending_network_initiated_sessions_updated (MMModemOma *self,
                                            GParamSpec *pspec)
{
    g_mutex_lock (&self->priv->pending_network_initiated_sessions_mutex);
    {
        GVariant *dictionary;

        if (self->priv->pending_network_initiated_sessions)
            g_array_unref (self->priv->pending_network_initiated_sessions);

        dictionary = mm_gdbus_modem_oma_get_pending_network_initiated_sessions (MM_GDBUS_MODEM_OMA (self));
        self->priv->pending_network_initiated_sessions = (dictionary ?
                                                          mm_common_oma_pending_network_initiated_sessions_variant_to_garray (dictionary) :
                                                          NULL);
    }
    g_mutex_unlock (&self->priv->pending_network_initiated_sessions_mutex);
}

static gboolean
ensure_internal_pending_network_initiated_sessions (MMModemOma *self,
                                                    MMOmaPendingNetworkInitiatedSession **dup_sessions,
                                                    guint *dup_sessions_n)
{
    gboolean ret;

    g_mutex_lock (&self->priv->pending_network_initiated_sessions_mutex);
    {
        /* If this is the first time ever asking for the array, setup the
         * update listener and the initial array, if any. */
        if (!self->priv->pending_network_initiated_sessions_id) {
            GVariant *dictionary;

            dictionary = mm_gdbus_modem_oma_dup_pending_network_initiated_sessions (MM_GDBUS_MODEM_OMA (self));
            if (dictionary) {
                self->priv->pending_network_initiated_sessions = mm_common_oma_pending_network_initiated_sessions_variant_to_garray (dictionary);
                g_variant_unref (dictionary);
            }

            /* No need to clear this signal connection when freeing self */
            self->priv->pending_network_initiated_sessions_id =
                g_signal_connect (self,
                                  "notify::pending-network-initiated-sessions",
                                  G_CALLBACK (pending_network_initiated_sessions_updated),
                                  NULL);
        }

        if (!self->priv->pending_network_initiated_sessions)
            ret = FALSE;
        else {
            ret = TRUE;

            if (dup_sessions && dup_sessions_n) {
                *dup_sessions_n = self->priv->pending_network_initiated_sessions->len;
                if (self->priv->pending_network_initiated_sessions->len > 0) {
                    *dup_sessions = g_malloc (sizeof (MMOmaPendingNetworkInitiatedSession) * self->priv->pending_network_initiated_sessions->len);
                    memcpy (*dup_sessions, self->priv->pending_network_initiated_sessions->data, sizeof (MMOmaPendingNetworkInitiatedSession) * self->priv->pending_network_initiated_sessions->len);
                } else
                    *dup_sessions = NULL;
            }
        }
    }
    g_mutex_unlock (&self->priv->pending_network_initiated_sessions_mutex);

    return ret;
}

/**
 * mm_modem_get_pending_network_initiated_sessions:
 * @self: A #MMModem.
 * @sessions: (out) (array length=n_sessions): Return location for the array of #MMOmaPendingNetworkInitiatedSession structs. The returned array should be freed with g_free() when no longer needed.
 * @n_sessions: (out): Return location for the number of values in @sessions.
 *
 * Gets the list of pending network-initiated OMA sessions.
 *
 * Returns: %TRUE if @sessions and @n_sessions are set, %FALSE otherwise.
 */
gboolean
mm_modem_get_pending_network_initiated_sessions (MMModemOma *self,
                                                 MMOmaPendingNetworkInitiatedSession **sessions,
                                                 guint *n_sessions)
{
    g_return_val_if_fail (MM_IS_MODEM_OMA (self), FALSE);
    g_return_val_if_fail (sessions != NULL, FALSE);
    g_return_val_if_fail (n_sessions != NULL, FALSE);

    return ensure_internal_pending_network_initiated_sessions (self, sessions, n_sessions);
}

/**
 * mm_modem_peek_pending_network_initiated_sessions:
 * @self: A #MMModem.
 * @sessions: (out) (array length=n_sessions): Return location for the array of #MMOmaPendingNetworkInitiatedSession values. Do not free the returned array, it is owned by @self.
 * @n_sessions: (out): Return location for the number of values in @sessions.
 *
 * Gets the list of pending network-initiated OMA sessions.
 *
 * Returns: %TRUE if @sessions and @n_sessions are set, %FALSE otherwise.
 */
gboolean
mm_modem_peek_pending_network_initiated_sessions (MMModemOma *self,
                                                  const MMOmaPendingNetworkInitiatedSession **sessions,
                                                  guint *n_sessions)
{
    g_return_val_if_fail (MM_IS_MODEM_OMA (self), FALSE);
    g_return_val_if_fail (sessions != NULL, FALSE);
    g_return_val_if_fail (n_sessions != NULL, FALSE);

    if (!ensure_internal_pending_network_initiated_sessions (self, NULL, NULL))
        return FALSE;

    *n_sessions = self->priv->pending_network_initiated_sessions->len;
    *sessions = (MMOmaPendingNetworkInitiatedSession *)self->priv->pending_network_initiated_sessions->data;
    return TRUE;
}

/*****************************************************************************/

static void
mm_modem_oma_init (MMModemOma *self)
{
    /* Setup private data */
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
                                              MM_TYPE_MODEM_OMA,
                                              MMModemOmaPrivate);
    g_mutex_init (&self->priv->pending_network_initiated_sessions_mutex);
}

static void
finalize (GObject *object)
{
    MMModemOma *self = MM_MODEM_OMA (object);

    g_mutex_clear (&self->priv->pending_network_initiated_sessions_mutex);

    if (self->priv->pending_network_initiated_sessions)
        g_array_unref (self->priv->pending_network_initiated_sessions);

    G_OBJECT_CLASS (mm_modem_oma_parent_class)->finalize (object);
}

static void
mm_modem_oma_class_init (MMModemOmaClass *modem_class)
{
    GObjectClass *object_class = G_OBJECT_CLASS (modem_class);

    g_type_class_add_private (object_class, sizeof (MMModemOmaPrivate));

    /* Virtual methods */
    object_class->finalize = finalize;
}
