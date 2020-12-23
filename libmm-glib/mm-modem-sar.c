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
 * Copyright TODO
 */

#include <gio/gio.h>

#include "mm-helpers.h"
#include "mm-errors-types.h"
#include "mm-modem-sar.h"
#include "mm-common-helpers.h"

/**
 * SECTION: mm-modem-sar
 * @title: MMModemSar
 * @short_description: The Sar interface
 *
 * The #MMModemSar is an object providing access to the methods, signals and
 * properties of the Sar interface.
 *
 * The Sar interface is exposed whenever a modem has sar capabilities.
 */

G_DEFINE_TYPE (MMModemSar, mm_modem_sar, MM_GDBUS_TYPE_MODEM_SAR_PROXY)

struct _MMModemSarPrivate {
    GMutex supported_levels_mutex;
    guint supported_levels_id;
    GArray *supported_levels;
};
/*****************************************************************************/

static void
supported_levels_updated (MMModemSar *self,
                          GParamSpec *pspec)
{
    g_mutex_lock (&self->priv->supported_levels_mutex);
    {
        GVariant *array;

        if (self->priv->supported_levels)
            g_array_unref (self->priv->supported_levels);

        array = mm_gdbus_modem_sar_get_supported_power_levels (MM_GDBUS_MODEM_SAR (self));
        self->priv->supported_levels = (array ?
                                          mm_common_power_levels_variant_to_garray (array) :
                                          NULL);
    }
    g_mutex_unlock (&self->priv->supported_levels_mutex);
}

static void
ensure_internal_supported_power_levels (MMModemSar *self,
                                        GArray    **dup)
{
    g_mutex_lock (&self->priv->supported_levels_mutex);
    {
        /* If this is the first time ever asking for the array, setup the
         * update listener and the initial array, if any. */
        if (!self->priv->supported_levels_id) {
            GVariant *array;

            array = mm_gdbus_modem_sar_dup_supported_power_levels (MM_GDBUS_MODEM_SAR (self));
            if (array) {
                self->priv->supported_levels = mm_common_power_levels_variant_to_garray (array);
                g_variant_unref (array);
            }

            /* No need to clear this signal connection when freeing self */
            self->priv->supported_levels_id =
                g_signal_connect (self,
                                  "notify::supported-power-levels",
                                  G_CALLBACK (supported_levels_updated),
                                  NULL);
        }

        if (dup && self->priv->supported_levels)
            *dup = g_array_ref (self->priv->supported_levels);
    }
    g_mutex_unlock (&self->priv->supported_levels_mutex);
}

/*****************************************************************************/

/**
 * mm_modem_sar_get_path:
 * @self: A #MMModemSar.
 *
 * Gets the DBus path of the #MMObject which implements this interface.
 *
 * Returns: (transfer none): The DBus path of the #MMObject object.
 *
 * Since: 1.15
 */
const gchar *
mm_modem_sar_get_path (MMModemSar *self)
{
    g_return_val_if_fail (MM_IS_MODEM_SAR (self), NULL);

    RETURN_NON_EMPTY_CONSTANT_STRING (
        g_dbus_proxy_get_object_path (G_DBUS_PROXY (self)));
}

/**
 * mm_modem_sar_dup_path:
 * @self: A #MMModemSar.
 *
 * Gets a copy of the DBus path of the #MMObject object which implements this
 * interface.
 *
 * Returns: (transfer full): The DBus path of the #MMObject. The returned value
 * should be freed with g_free().
 *
 * Since: 1.15
 */
gchar *
mm_modem_sar_dup_path (MMModemSar *self)
{
    gchar *value;

    g_return_val_if_fail (MM_IS_MODEM_SAR (self), NULL);

    g_object_get (G_OBJECT (self),
                  "g-object-path", &value,
                  NULL);
    RETURN_NON_EMPTY_STRING (value);
}
/*****************************************************************************/

/**
 * mm_modem_sar_setup_finish:
 * @self: A #MMModemSar.
 * @res: The #GAsyncResult obtained from the #GAsyncReadyCallback passed to
 *  mm_modem_sar_setup().
 * @error: Return location for error or %NULL.
 *
 * Finishes an operation started with mm_modem_sar_setup().
 *
 * Returns: %TRUE if the setup was successful, %FALSE if @error is set.
 *
 * Since: 1.15
 */
gboolean
mm_modem_sar_enable_finish (MMModemSar   *self,
                            GAsyncResult *res,
                            GError      **error)
{
    g_return_val_if_fail (MM_IS_MODEM_SAR (self), FALSE);

    return mm_gdbus_modem_sar_call_enable_finish (MM_GDBUS_MODEM_SAR (self), res, error);
}

/**
 * mm_modem_sar_setup:
 * @self: A #MMModemSar.
 * @enable: %TRUE to enable dynamic SAR and %FALSE to disable it.
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @callback: A #GAsyncReadyCallback to call when the request is satisfied or
 *  %NULL.
 * @user_data: User data to pass to @callback.
 *
 * Asynchronously enable or disable dynamic SAR.
 *
 * When the operation is finished, @callback will be invoked in the
 * <link linkend="g-main-context-push-thread-default">thread-default main loop</link>
 * of the thread you are calling this method from. You can then call
 * mm_modem_sar_setup_finish() to get the result of the operation.
 *
 * See mm_modem_sar_setup_sync() for the synchronous, blocking version of
 * this method.
 *
 * Since: 1.15
 */
void
mm_modem_sar_enable (MMModemSar          *self,
                     gboolean             enable,
                     GCancellable        *cancellable,
                     GAsyncReadyCallback  callback,
                     gpointer             user_data)
{
    g_return_if_fail (MM_IS_MODEM_SAR (self));

    mm_gdbus_modem_sar_call_enable (MM_GDBUS_MODEM_SAR (self), enable, cancellable, callback, user_data);
}

/**
 * mm_modem_sar_setup_sync:
 * @self: A #MMModemSar.
 * @enable: %TRUE to enable dynamic SAR and %FALSE to disable it.
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @error: Return location for error or %NULL.
 *
 * Synchronously enable or disable dynamic SAR.
 *
 * The calling thread is blocked until a reply is received. See
 * mm_modem_sar_setup() for the asynchronous version of this method.
 *
 * Returns: %TRUE if the setup was successful, %FALSE if @error is set.
 *
 * Since: 1.15
 */
gboolean
mm_modem_sar_enable_sync (MMModemSar   *self,
                          gboolean      enable,
                          GCancellable *cancellable,
                          GError      **error)
{
    g_return_val_if_fail (MM_IS_MODEM_SAR (self), FALSE);

    return mm_gdbus_modem_sar_call_enable_sync (MM_GDBUS_MODEM_SAR (self), enable, cancellable, error);
}

/*****************************************************************************/

/**
 * mm_modem_sar_setup_finish:
 * @self: A #MMModemSar.
 * @res: The #GAsyncResult obtained from the #GAsyncReadyCallback passed to
 *  mm_modem_sar_setup().
 * @error: Return location for error or %NULL.
 *
 * Finishes an operation started with mm_modem_sar_set_power_level().
 *
 * Returns: %TRUE if the setup was successful, %FALSE if @error is set.
 *
 * Since: 1.15
 */
gboolean
mm_modem_sar_set_power_level_finish (MMModemSar   *self,
                                     GAsyncResult *res,
                                     GError      **error)
{
    g_return_val_if_fail (MM_IS_MODEM_SAR (self), FALSE);

    return mm_gdbus_modem_sar_call_set_power_level_finish (MM_GDBUS_MODEM_SAR (self), res, error);
}

/**
 * mm_modem_sar_setup:
 * @self: A #MMModemSar.
 * @level: the "default" power level (0) and then each modem could report
 *  the amount of additional power levels it supports
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @callback: A #GAsyncReadyCallback to call when the request is satisfied or
 *  %NULL.
 * @user_data: User data to pass to @callback.
 *
 * Asynchronously set current dynamic SAR power level.
 *
 * When the operation is finished, @callback will be invoked in the
 * <link linkend="g-main-context-push-thread-default">thread-default main loop</link>
 * of the thread you are calling this method from. You can then call
 * mm_modem_sar_setup_finish() to get the result of the operation.
 *
 * See mm_modem_sar_setup_sync() for the synchronous, blocking version of
 * this method.
 *
 * Since: 1.15
 */
void
mm_modem_sar_set_power_level (MMModemSar         *self,
                              guint               level,
                              GCancellable       *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer            user_data)
{
    g_return_if_fail (MM_IS_MODEM_SAR (self));

    mm_gdbus_modem_sar_call_set_power_level (MM_GDBUS_MODEM_SAR (self), level, cancellable, callback, user_data);
}

/**
 * mm_modem_sar_setup_sync:
 * @self: A #MMModemSar.
 * @level: the "default" power level (0) and then each modem could report
 *  the amount of additional power levels it supports
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @error: Return location for error or %NULL.
 *
 * Synchronously set current dynamic SAR power level.
 *
 * The calling thread is blocked until a reply is received. See
 * mm_modem_sar_set_power_level() for the asynchronous version of this method.
 *
 * Returns: %TRUE if the setup was successful, %FALSE if @error is set.
 *
 * Since: 1.15
 */
gboolean
mm_modem_sar_set_power_level_sync (MMModemSar   *self,
                                   guint         level,
                                   GCancellable *cancellable,
                                   GError      **error)
{
    g_return_val_if_fail (MM_IS_MODEM_SAR (self), FALSE);

    return mm_gdbus_modem_sar_call_set_power_level_sync (MM_GDBUS_MODEM_SAR (self), level, cancellable, error);
}

/*****************************************************************************/

/**
 * mm_modem_sar_setup_finish:
 * @self: A #MMModemSar.
 * @res: The #GAsyncResult obtained from the #GAsyncReadyCallback passed to
 *  mm_modem_sar_setup().
 * @error: Return location for error or %NULL.
 *
 * Finishes an operation started with mm_modem_sar_setup().
 *
 * Returns: %TRUE if the setup was successful, %FALSE if @error is set.
 *
 * Since: 1.15
 */
gboolean
mm_modem_sar_set_supported_power_levels_finish (MMModemSar   *self,
                                                GAsyncResult *res,
                                                GError       **error)
{
    g_return_val_if_fail (MM_IS_MODEM_SAR (self), FALSE);

    return mm_gdbus_modem_sar_call_set_supported_power_levels_finish (MM_GDBUS_MODEM_SAR (self), res, error);
}

/**
 * mm_modem_sar_setup:
 * @self: A #MMModemSar.
 * @levels: The array of supported power levels
 * @n_levels: The number of values in @levels.
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @callback: A #GAsyncReadyCallback to call when the request is satisfied or
 *  %NULL.
 * @user_data: User data to pass to @callback.
 *
 * Asynchronously setups the extended sar quality retrieval.
 *
 * When the operation is finished, @callback will be invoked in the
 * <link linkend="g-main-context-push-thread-default">thread-default main loop</link>
 * of the thread you are calling this method from. You can then call
 * mm_modem_sar_setup_finish() to get the result of the operation.
 *
 * See mm_modem_sar_setup_sync() for the synchronous, blocking version of
 * this method.
 *
 * Since: 1.15
 */
void
mm_modem_sar_set_supported_power_levels (MMModemSar          *self,
                                         guint               *levels,
                                         guint                n_levels,
                                         GCancellable        *cancellable,
                                         GAsyncReadyCallback  callback,
                                         gpointer             user_data)
{
    g_return_if_fail (MM_IS_MODEM_SAR (self));
    g_return_if_fail (levels != NULL);
    g_return_if_fail (n_levels >= 0);

    GVariant *variant = mm_common_power_levels_array_to_variant(levels,n_levels);
    mm_gdbus_modem_sar_call_set_supported_power_levels (MM_GDBUS_MODEM_SAR (self), variant, cancellable, callback, user_data);
}

/**
 * mm_modem_sar_setup_sync:
 * @self: A #MMModemSar.
 * @levels: The array of supported power levels
 * @n_levels: The number of values in @levels.
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @error: Return location for error or %NULL.
 *
 * Synchronously setups the extended sar quality retrieval.
 *
 * The calling thread is blocked until a reply is received. See
 * mm_modem_sar_setup() for the asynchronous version of this method.
 *
 * Returns: %TRUE if the setup was successful, %FALSE if @error is set.
 *
 * Since: 1.15
 */
gboolean
mm_modem_sar_set_supported_power_levels_sync (MMModemSar   *self,
                                              guint        *levels,
                                              guint         n_levels,
                                              GCancellable *cancellable,
                                              GError      **error)
{
    g_return_val_if_fail (MM_IS_MODEM_SAR (self), FALSE);
    g_return_val_if_fail (levels != NULL, FALSE);
    g_return_val_if_fail (n_levels >= 0, FALSE);

    GVariant *variant = mm_common_power_levels_array_to_variant(levels,n_levels);

    return mm_gdbus_modem_sar_call_set_supported_power_levels_sync (MM_GDBUS_MODEM_SAR (self), variant, cancellable, error);
}

/*****************************************************************************/
/**
 * mm_modem_sar_get_state:
 * @self: A #MMModem.
 * Returns: %TRUE if dynamic sar are enabled, %FALSE otherwise.
 *
 * Since: 1.15
 */
gboolean
mm_modem_sar_get_state (MMModemSar *self)
{
    g_return_val_if_fail (MM_IS_MODEM_SAR (self), FALSE);

    return (gboolean) mm_gdbus_modem_sar_get_state (MM_GDBUS_MODEM_SAR (self));
}

/*****************************************************************************/
/**
 * mm_modem_sar_get_power_level:
 * @self: A #MMModem.
 *
 * Returns: 0 if default level, other supported power level.
 *
 * Since: 1.15
 */
guint
mm_modem_sar_get_power_level (MMModemSar *self)
{
    g_return_val_if_fail (MM_IS_MODEM_SAR (self), FALSE);

    return (guint) mm_gdbus_modem_sar_get_power_level(MM_GDBUS_MODEM_SAR (self));
}



/*****************************************************************************/
/**
 * mm_modem_sar_get_supported_power_levels:
 * @self: A #MMModem.
 * @levels: (out) (array length=n_levels): Return location for the array of
 *  power levels. The returned array should be freed with g_free() when
 *  no longer needed.
 * @n_levels: (out): Return location for the number of values in @levels.
 *
 * Gets the list of power level supported by the #MMModem.
 *
 * Returns: %TRUE if @levels and @n_levels are set, %FALSE otherwise.
 *
 * Since: 1.15
 */
gboolean
mm_modem_sar_get_supported_power_levels (MMModemSar *self,
                                         guint     **levels,
                                         guint      *n_levels)
{
    GArray *array = NULL;

    g_return_val_if_fail (MM_IS_MODEM_SAR (self), FALSE);
    g_return_val_if_fail (levels != NULL, FALSE);
    g_return_val_if_fail (n_levels != NULL, FALSE);

    ensure_internal_supported_power_levels (self, &array);
    if (!array)
        return FALSE;

    *n_levels = array->len;
    *levels = (MMSmsStorage *)g_array_free (array, FALSE);
    return TRUE;
}

/**
 * mm_modem_sar_peek_supported_power_levels:
 * @self: A #MMModem.
 * @levels: (out): Return location for the array of power levels. Do
 *  not free the returned array, it is owned by @self.
 * @n_levels: (out): Return location for the number of values in @levels.
 *
 * Gets the list of power level supported by the #MMModem.
 *
 * Returns: %TRUE if @levels and @n_levels are set, %FALSE otherwise.
 *
 * Since: 1.15
 */
gboolean
mm_modem_sar_peek_supported_power_levels (MMModemSar *self,
                                          guint     **levels,
                                          guint      *n_levels)
{
    g_return_val_if_fail (MM_IS_MODEM_SAR (self), FALSE);
    g_return_val_if_fail (levels != NULL, FALSE);
    g_return_val_if_fail (n_levels != NULL, FALSE);

    ensure_internal_supported_power_levels (self, NULL);
    if (!self->priv->supported_levels)
        return FALSE;

    *n_levels = self->priv->supported_levels->len;
    *levels = (MMSmsStorage *)self->priv->supported_levels->data;
    return TRUE;
}

/*****************************************************************************/

static void
mm_modem_sar_init (MMModemSar *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, MM_TYPE_MODEM_SAR, MMModemSarPrivate);
    g_mutex_init (&self->priv->supported_levels_mutex);
}

static void
finalize (GObject *object)
{
    MMModemSar *self = MM_MODEM_SAR (object);

    g_mutex_clear (&self->priv->supported_levels_mutex);

    if (self->priv->supported_levels)
        g_array_unref (self->priv->supported_levels);
    G_OBJECT_CLASS (mm_modem_sar_parent_class)->finalize (object);
}

static void
mm_modem_sar_class_init (MMModemSarClass *modem_class)
{
    GObjectClass *object_class = G_OBJECT_CLASS (modem_class);

    g_type_class_add_private (object_class, sizeof (MMModemSarPrivate));

    /* Virtual methods */
    object_class->finalize = finalize;
}

