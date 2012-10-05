/*** BEGIN file-header ***/

/*** END file-header ***/

/*** BEGIN file-production ***/
/* enumerations from "@filename@" */
/*** END file-production ***/

/*** BEGIN value-header ***/
static const G@Type@Value @enum_name@_values[] = {
/*** END value-header ***/
/*** BEGIN value-production ***/
    { @VALUENAME@, "@VALUENAME@", "@valuenick@" },
/*** END value-production ***/
/*** BEGIN value-tail ***/
    { 0, NULL, NULL }
};

/* Define type-specific symbols */
#undef IS_ENUM
#undef IS_FLAGS
#define IS_@TYPE@

GType
@enum_name@_get_type (void)
{
    static volatile gsize g_define_type_id__volatile = 0;

    if (g_once_init_enter (&g_define_type_id__volatile)) {
        GType g_define_type_id =
            g_@type@_register_static (g_intern_static_string ("@EnumName@"),
                                      @enum_name@_values);
        g_once_init_leave (&g_define_type_id__volatile, g_define_type_id);
    }

    return g_define_type_id__volatile;
}

/* Enum-specific method to get the value as a string.
 * We get the nick of the GEnumValue. Note that this will be
 * valid even if the GEnumClass is not referenced anywhere. */
#if defined IS_ENUM
const gchar *
@enum_name@_get_string (@EnumName@ val)
{
    guint i;

    for (i = 0; @enum_name@_values[i].value_nick; i++) {
        if (val == @enum_name@_values[i].value)
            return @enum_name@_values[i].value_nick;
    }

    return NULL;
}
#endif /* IS_ENUM */

/* Flags-specific method to build a string with the given mask.
 * We get a comma separated list of the nicks of the GFlagsValues.
 * Note that this will be valid even if the GFlagsClass is not referenced
 * anywhere. */
#if defined IS_FLAGS
gchar *
@enum_name@_build_string_from_mask (@EnumName@ mask)
{
    guint i;
    gboolean first = TRUE;
    GString *str = NULL;

    for (i = 0; @enum_name@_values[i].value_nick; i++) {
        /* We also look for exact matches */
        if (mask == @enum_name@_values[i].value) {
            if (str)
                g_string_free (str, TRUE);
            return g_strdup (@enum_name@_values[i].value_nick);
        }

        /* Build list with single-bit masks */
        if (mask & @enum_name@_values[i].value) {
            guint c;
            gulong number = @enum_name@_values[i].value;

            for (c = 0; number; c++)
                number &= number - 1;

            if (c == 1) {
                if (!str)
                    str = g_string_new ("");
                g_string_append_printf (str, "%s%s",
                                        first ? "" : ", ",
                                        @enum_name@_values[i].value_nick);
                if (first)
                    first = FALSE;
            }
        }
    }

    return (str ? g_string_free (str, FALSE) : NULL);
}
#endif /* IS_FLAGS */

/*** END value-tail ***/

/*** BEGIN file-tail ***/
/*** END file-tail ***/
