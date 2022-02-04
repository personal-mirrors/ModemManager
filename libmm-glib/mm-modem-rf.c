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
 * Copyright (C) 2021 Fibocom Wireless Inc.
 */

#include <gio/gio.h>

#include "mm-helpers.h"
#include "mm-errors-types.h"
#include "mm-modem-rf.h"
#include "mm-common-helpers.h"

/**
 * SECTION: mm-modem-rf
 * @title: MMModemRf
 * @short_description: The RF interface
 *
 * The #MMModemRf is an object providing access to the methods, signals and
 * properties of the RF interface.
 *
 * The RF interface is exposed whenever a modem has RF capabilities.
 */

G_DEFINE_TYPE (MMModemRf, mm_modem_rf, MM_GDBUS_TYPE_MODEM_RF_PROXY)

/*****************************************************************************/

/**
 * mm_modem_rf_get_path:
 * @self: A #MMModemRf.
 *
 * Gets the DBus path of the #MMObject which implements this interface.
 *
 * Returns: (transfer none): The DBus path of the #MMObject object.
 *
 * Since: 1.20
 */
const gchar *
mm_modem_rf_get_path (MMModemRf *self)
{
    g_return_val_if_fail (MM_IS_MODEM_RF (self), NULL);

    RETURN_NON_EMPTY_CONSTANT_STRING (
        g_dbus_proxy_get_object_path (G_DBUS_PROXY (self)));
}

/**
 * mm_modem_rf_dup_path:
 * @self: A #MMModemRf.
 *
 * Gets a copy of the DBus path of the #MMObject object which implements this
 * interface.
 *
 * Returns: (transfer full): The DBus path of the #MMObject. The returned value
 * should be freed with g_free().
 *
 * Since: 1.20
 */
gchar *
mm_modem_rf_dup_path (MMModemRf *self)
{
    gchar *value;

    g_return_val_if_fail (MM_IS_MODEM_RF (self), NULL);

    g_object_get (G_OBJECT (self),
                  "g-object-path", &value,
                  NULL);
    RETURN_NON_EMPTY_STRING (value);
}

/*****************************************************************************/

struct _MMModemRfInfo {
    MMRfCellType  serving_cell_info;
    guint64       center_frequency;
    guint32       bandwidth;
    guint32       rsrp;
    guint32       rsrq;
    guint32       sinr;
    guint32       rssi;
    guint32       connection_status;
};

/**
 * mm_modem_rf_get_serving_cell_info:
 * @info: A #MMModemRfInfo.
 *
 * Get the serving cell info.
 *
 * Returns: A #MMRfCellType.
 *
 * Since: 1.20
 */
MMRfCellType
mm_modem_rf_get_serving_cell_info (const MMModemRfInfo *info)
{
    g_return_val_if_fail (info != NULL, MM_RF_CELL_TYPE_INVALID);

    return info->serving_cell_info;
}

/**
 * mm_modem_rf_get_center_frequency:
 * @info: A #MMModemRfInfo.
 *
 * Get the center frequency value.
 *
 * Returns: A #guint64.
 *
 * Since: 1.20
 */
guint64
mm_modem_rf_get_center_frequency (const MMModemRfInfo *info)
{
    g_return_val_if_fail (info != NULL, 0);

    return info->center_frequency;
}

/**
 * mm_modem_rf_get_bandwidth:
 * @info: A #MMModemRfInfo.
 *
 * Get the bandwidth value.
 *
 * Returns: A #guint32.
 *
 * Since: 1.20
 */
guint32
mm_modem_rf_get_bandwidth (const MMModemRfInfo *info)
{
    g_return_val_if_fail (info != NULL, 0);

    return info->bandwidth;
}

/**
 * mm_modem_rf_get_rsrp:
 * @info: A #MMModemRfInfo.
 *
 * Get the rsrp value.
 *
 * Returns: A #guint32.
 *
 * Since: 1.20
 */
guint32
mm_modem_rf_get_rsrp (const MMModemRfInfo *info)
{
    g_return_val_if_fail (info != NULL, 0);

    return info->rsrp;
}

/**
 * mm_modem_rf_get_rsrq:
 * @info: A #MMModemRfInfo.
 *
 * Get the rsrq value.
 *
 * Returns: A #guint32.
 *
 * Since: 1.20
 */
guint32
mm_modem_rf_get_rsrq (const MMModemRfInfo *info)
{
    g_return_val_if_fail (info != NULL, 0);

    return info->rsrq;
}

/**
 * mm_modem_rf_get_sinr:
 * @info: A #MMModemRfInfo.
 *
 * Get the sinr value.
 *
 * Returns: A #guint32.
 *
 * Since: 1.20
 */
guint32
mm_modem_rf_get_sinr (const MMModemRfInfo *info)
{
    g_return_val_if_fail (info != NULL, 0);

    return info->sinr;
}

/**
 * mm_modem_rf_get_rssi:
 * @info: A #MMModemRfInfo.
 *
 * Get the rssi value.
 *
 * Returns: A #guint32.
 *
 * Since: 1.20
 */
guint32
mm_modem_rf_get_rssi (const MMModemRfInfo *info)
{
    g_return_val_if_fail (info != NULL, 0);

    return info->rssi;
}


/**
 * mm_modem_rf_get_connection_status:
 * @info: A #MMModemRfInfo.
 *
 * Get the connection status value.
 *
 * Returns: A #guint32.
 *
 * Since: 1.20
 */
guint32
mm_modem_rf_get_connection_status (const MMModemRfInfo *info)
{
    g_return_val_if_fail (info != NULL, 0);

    return info->connection_status;
}

/**
 * mm_modem_thermal_ext_rfim_info_free:
 * @info: A #MMModemRfInfo.
 *
 * Frees a #MMModemRfInfo.
 *
 * Since: 1.20
 */
 
void
mm_modem_rf_rf_info_free (MMModemRfInfo *info)
{
    if (!info)
        return;

    g_free (info);
}

static GList *
create_rfim_info_list (GVariant *variant)
{
    GList *list = NULL;
    GVariantIter dict_iter;
    GVariant *dict;

    /* Input is aa{sv} */
    g_variant_iter_init (&dict_iter, variant);
    while ((dict = g_variant_iter_next_value (&dict_iter))) {
        GVariantIter iter;
        gchar *key;
        GVariant *value;
        MMModemRfInfo *info;

        info = g_slice_new0 (MMModemRfInfo);

        g_variant_iter_init (&iter, dict);
        while (g_variant_iter_next (&iter, "{sv}", &key, &value)) {
            if (g_str_equal (key, "serving-cell-info")) {
                info->serving_cell_info =g_variant_get_uint32 (value);
            } else if (g_str_equal (key, "center-frequency")) {
                info->center_frequency =g_variant_get_uint64 (value);
            } else if (g_str_equal (key, "bandwidth")) {
                info->bandwidth =g_variant_get_uint32 (value);
            } else if (g_str_equal (key, "rsrp")) {
                info->rsrp =g_variant_get_uint32 (value);
            }  else if (g_str_equal (key, "rsrq")) {
                info->rsrq =g_variant_get_uint32 (value);
            }  else if (g_str_equal (key, "sinr")) {
                info->sinr =g_variant_get_uint32 (value);
            }  else if (g_str_equal (key, "rssi")) {
                info->rssi =g_variant_get_uint32 (value);
            }  else if (g_str_equal (key, "connection-status")) {
                info->connection_status =g_variant_get_uint32 (value);
            } else
                g_warning ("Unexpected property '%s' found in RFIM info", key);

            g_free (key);
            g_variant_unref (value);
        }

        list = g_list_prepend (list, info);
        g_variant_unref (dict);
    }

    return list;
}

/****************************************************************************/
/**
 * mm_modem_rf_get_rf_inf:
 * @self: A #MMModemRf.
 *
 * Get the radio frequency specific information.
 *
 * Returns: (transfer full): A list of RF info data, or #NULL if @error is 
 * set. The returned value should be freed with g_list_free_full() using
 * g_object_unref() as #GDestroyNotify function.
 *
 * Since: 1.20
 */

GList*
mm_modem_rf_get_rf_inf (MMModemRf *self)
{
    GVariant     *dictionary;

    dictionary = mm_gdbus_modem_rf_get_rf_inf(MM_GDBUS_MODEM_RF (self));
    g_return_val_if_fail (g_variant_is_of_type (dictionary, G_VARIANT_TYPE ("aa{sv}")), NULL);

    return create_rfim_info_list(dictionary);
}

/**
 * mm_modem_rf_get_rf_info_finish:
 * @self: A #MMModemRf.
 * @res: The #GAsyncResult obtained from the #GAsyncReadyCallback passed to
 *  mm_modem_rf_enable().
 * @error: Return location for error or %NULL.
 *
 * Finishes an operation started with mm_modem_rf_get_rf_info().
 *
 * Returns: %TRUE if get rf info was successful, %FALSE if @error is set.
 *
 * Since: 1.20
 */
gboolean
mm_modem_rf_get_rf_info_finish (MMModemRf   *self,
                                GAsyncResult *res,
                                GError      **error)
{
    g_return_val_if_fail (MM_IS_MODEM_RF (self), FALSE);

    return mm_gdbus_modem_rf_call_get_rf_info_finish (MM_GDBUS_MODEM_RF (self), res, error);
}

/**
 * mm_modem_rf_get_rf_info:
 * @self: A #MMModemRf.
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @callback: A #GAsyncReadyCallback to call when the request is satisfied or
 *  %NULL.
 * @user_data: User data to pass to @callback.
 *
 * Asynchronously requests to get RFIM frequency info.
 *
 * When the operation is finished, @callback will be invoked in the
 * <link linkend="g-main-context-push-thread-default">thread-default main loop</link>
 * of the thread you are calling this method from. You can then call
 * mm_modem_rf_get_rf_info_finish() to get the result of the operation.
 *
 * See mm_modem_rf_get_rf_info_sync() for the synchronous, blocking version of
 * this method.
 *
 * Since: 1.20
 */
void
mm_modem_rf_get_rf_info (MMModemRf          *self,
                         GCancellable        *cancellable,
                         GAsyncReadyCallback  callback,
                         gpointer             user_data)
{
    g_return_if_fail (MM_IS_MODEM_RF (self));

    mm_gdbus_modem_rf_call_get_rf_info (MM_GDBUS_MODEM_RF (self), cancellable, callback, user_data);
}

/**
 * mm_modem_rf_get_rf_info_sync:
 * @self: A #MMModemRf.
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @error: Return location for error or %NULL.
 *
 * Synchronously requests to get RFIM frequency info.
 *
 * The calling thread is blocked until a reply is received. See
 * mm_modem_rf_get_rf_info() for the asynchronous version of this method.
 *
 * Returns: %TRUE if get rf info was successful, %FALSE if @error is set.
 *
 * Since: 1.20
 */
gboolean
mm_modem_rf_get_rf_info_sync (MMModemRf   *self,
                              GCancellable *cancellable,
                              GError      **error)
{
    g_return_val_if_fail (MM_IS_MODEM_RF (self), FALSE);

    return mm_gdbus_modem_rf_call_get_rf_info_sync (MM_GDBUS_MODEM_RF (self),
                                                                cancellable,
                                                                error);
}

/**
 * mm_modem_rf_setup_rf_info_finish:
 * @self: A #MMModemRf.
 * @res: The #GAsyncResult obtained from the #GAsyncReadyCallback passed to
 *  mm_modem_rf_enable().
 * @error: Return location for error or %NULL.
 *
 * Finishes an operation started with mm_modem_rf_setup_rf_info().
 *
 * Returns: %TRUE if set rf info was successful, %FALSE if @error is set.
 *
 * Since: 1.20
 */
gboolean
mm_modem_rf_setup_rf_info_finish (MMModemRf   *self,
                                  GAsyncResult *res,
                                  GError      **error)
{
    g_return_val_if_fail (MM_IS_MODEM_RF (self), FALSE);

    return mm_gdbus_modem_rf_call_setup_rf_info_finish (MM_GDBUS_MODEM_RF (self), res, error);
}

/**
 * mm_modem_rf_setup_rf_info:
 * @self: A #MMModemRf.
 * @enable: A boolean value when set to TRUE will enable RF info notification else
 *  will disable when set to FALSE
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @callback: A #GAsyncReadyCallback to call when the request is satisfied or
 *  %NULL.
 * @user_data: User data to pass to @callback.
 *
 * Asynchronously requests to get RFIM frequency info.
 *
 * When the operation is finished, @callback will be invoked in the
 * <link linkend="g-main-context-push-thread-default">thread-default main loop</link>
 * of the thread you are calling this method from. You can then call
 * mm_modem_rf_setup_rf_info_finish() to get the result of the operation.
 *
 * See mm_modem_rf_setup_rf_info_sync() for the synchronous, blocking version of
 * this method.
 *
 * Since: 1.20
 */
void
mm_modem_rf_setup_rf_info (MMModemRf          *self,
                           gboolean     enable,
                           GCancellable        *cancellable,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
    g_return_if_fail (MM_IS_MODEM_RF (self));

    mm_gdbus_modem_rf_call_setup_rf_info (MM_GDBUS_MODEM_RF (self), enable, cancellable, callback, user_data);
}

/**
 * mm_modem_rf_setup_rf_info_sync:
 * @self: A #MMModemRf.
 * @enable: A boolean value when set to TRUE will enable RF info notification else
 *  will disable when set to FALSE
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @error: Return location for error or %NULL.
 *
 * Synchronously requests to get RFIM frequency info.
 *
 * The calling thread is blocked until a reply is received. See
 * mm_modem_rf_setup_rf_info() for the asynchronous version of this method.
 *
 * Returns: %TRUE if set rf info was successful, %FALSE if @error is set.
 *
 * Since: 1.20
 */
gboolean
mm_modem_rf_setup_rf_info_sync (MMModemRf   *self,
                                gboolean     enable,
                                GCancellable *cancellable,
                                GError      **error)
{

    g_return_val_if_fail (MM_IS_MODEM_RF (self), FALSE);

    return mm_gdbus_modem_rf_call_setup_rf_info_sync (MM_GDBUS_MODEM_RF (self),
                                                      enable,
                                                      cancellable,
                                                      error);
}


/*****************************************************************************/
static void
mm_modem_rf_init (MMModemRf *self)
{
}

static void
mm_modem_rf_class_init (MMModemRfClass *modem_class)
{
    /* Virtual methods */
}

