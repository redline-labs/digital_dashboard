// A fake de.pengutronix.rauc.Installer, for developing the reflash path off a
// board.
//
// WHY C RATHER THAN PYTHON. The first version of this was python3-gi, and its
// D-Bus SERVER side would not dispatch: registration succeeded through both
// register_object() and register_object_with_closures2(), and the method and
// property callbacks were simply never invoked, so every call timed out. Both
// signatures want a GObject.Closure and plain callables register something that
// never fires. Rather than keep guessing at the binding, this uses the same
// GDBus C API that libs/rauc_client already links -- no bindings in the way, and
// the vtable is explicit.
//
// The interface below is verbatim from rauc's
// src/de.pengutronix.rauc.Installer.xml. Being wrong in none of it is the entire
// point: every signature here fails at RUNTIME in the client, and a board is the
// worst place to discover that GetSlotStatus is a(sa{sv}).
//
//     rauc_stub            succeeds after a few seconds
//     rauc_stub --fail     fails the way a bad signature does
//     rauc_stub --busy     reports an install already in progress

#include <gio/gio.h>
#include <glib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const gchar kIntrospectionXml[] =
    "<node>"
    "  <interface name='de.pengutronix.rauc.Installer'>"
    "    <method name='Install'>"
    "      <arg type='s' name='source' direction='in'/>"
    "    </method>"
    "    <method name='InstallBundle'>"
    "      <arg type='s' name='source' direction='in'/>"
    "      <arg type='a{sv}' name='args' direction='in'/>"
    "    </method>"
    "    <method name='Info'>"
    "      <arg type='s' name='bundle' direction='in'/>"
    "      <arg type='s' name='compatible' direction='out'/>"
    "      <arg type='s' name='version' direction='out'/>"
    "    </method>"
    "    <method name='Mark'>"
    "      <arg type='s' name='state' direction='in'/>"
    "      <arg type='s' name='slot_identifier' direction='in'/>"
    "      <arg type='s' name='slot_name' direction='out'/>"
    "      <arg type='s' name='message' direction='out'/>"
    "    </method>"
    "    <method name='GetSlotStatus'>"
    "      <arg type='a(sa{sv})' name='slot_status_array' direction='out'/>"
    "    </method>"
    "    <method name='GetPrimary'>"
    "      <arg type='s' name='primary' direction='out'/>"
    "    </method>"
    "    <property name='Operation' type='s' access='read'/>"
    "    <property name='LastError' type='s' access='read'/>"
    "    <property name='Progress' type='(isi)' access='read'/>"
    "    <property name='Compatible' type='s' access='read'/>"
    "    <property name='Variant' type='s' access='read'/>"
    "    <property name='BootSlot' type='s' access='read'/>"
    "    <signal name='Completed'>"
    "      <arg type='i' name='result'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

static const gchar* kInterface = "de.pengutronix.rauc.Installer";

typedef enum
{
    MODE_OK,
    MODE_FAIL,
    MODE_BUSY
} StubMode;

static StubMode g_mode = MODE_OK;
static GDBusConnection* g_connection = NULL;
static GDBusNodeInfo* g_node = NULL;

static gchar* g_operation = NULL;
static gchar* g_last_error = NULL;
static gint32 g_percentage = 0;
static gchar* g_message = NULL;
static gint32 g_nesting = 0;

static void emit_properties_changed(const gchar* name, GVariant* value)
{
    GVariantBuilder changed;
    GVariantBuilder invalidated;
    g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&changed, "{sv}", name, value);
    g_variant_builder_init(&invalidated, G_VARIANT_TYPE("as"));

    g_dbus_connection_emit_signal(
        g_connection, NULL, "/", "org.freedesktop.DBus.Properties", "PropertiesChanged",
        g_variant_new("(sa{sv}as)", kInterface, &changed, &invalidated), NULL);
}

static void set_operation(const gchar* operation)
{
    g_free(g_operation);
    g_operation = g_strdup(operation);
    emit_properties_changed("Operation", g_variant_new_string(g_operation));
}

// REAL RAUC EMITS THIS AND THE FIRST VERSION OF THIS STUB DID NOT.
// rauc's object is a gdbus-codegen skeleton, which emits PropertiesChanged on
// every property set. Setting g_last_error without emitting left the client's
// property cache holding the previous (empty) value, so a failed install
// completed with a non-zero result and no reason -- the exact operator-hostile
// case the client is supposed to surface. The stub has to be faithful about
// this or it tests the wrong thing.
static void set_last_error(const gchar* message)
{
    g_free(g_last_error);
    g_last_error = g_strdup(message);
    emit_properties_changed("LastError", g_variant_new_string(g_last_error));
}

static void set_progress(gint32 percentage, const gchar* message)
{
    g_percentage = percentage;
    g_free(g_message);
    g_message = g_strdup(message);
    emit_properties_changed("Progress",
                            g_variant_new("(isi)", g_percentage, g_message, g_nesting));
}

typedef struct
{
    const gchar* message;
    gint32 percentage;
} Step;

static const Step kSteps[] = {
    {"determining slot states", 10},
    {"checking bundle", 30},
    {"writing slot rootfs.1", 60},
    {"updating boot order", 90},
};

static gboolean advance(gpointer user_data)
{
    const gsize index = GPOINTER_TO_SIZE(user_data);

    if (index < G_N_ELEMENTS(kSteps))
    {
        set_progress(kSteps[index].percentage, kSteps[index].message);
        g_timeout_add(700, advance, GSIZE_TO_POINTER(index + 1));
        return G_SOURCE_REMOVE;
    }

    gint32 result = 0;
    if (g_mode == MODE_FAIL)
    {
        // What a rejected bundle actually looks like: LastError set, a non-zero
        // result, and the boot order untouched.
        set_last_error("signature verification failed: bundle is not signed by a trusted key");
        set_progress(0, "installation failed");
        result = 1;
    }
    else
    {
        set_last_error("");
        set_progress(100, "installing done.");
    }

    set_operation("idle");
    g_dbus_connection_emit_signal(g_connection, NULL, "/", kInterface, "Completed",
                                  g_variant_new("(i)", result), NULL);
    return G_SOURCE_REMOVE;
}

// Shaped like the real board: two ext4 slots by bootname, A booted and good.
static void add_slot(GVariantBuilder* slots, const gchar* name, const gchar* partuuid,
                     const gchar* bootname, const gchar* state, const gchar* version,
                     guint64 timestamp)
{
    GVariantBuilder fields;
    g_variant_builder_init(&fields, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&fields, "{sv}", "device", g_variant_new_string(partuuid));
    g_variant_builder_add(&fields, "{sv}", "type", g_variant_new_string("ext4"));
    g_variant_builder_add(&fields, "{sv}", "bootname", g_variant_new_string(bootname));
    g_variant_builder_add(&fields, "{sv}", "state", g_variant_new_string(state));
    g_variant_builder_add(&fields, "{sv}", "boot-status", g_variant_new_string("good"));
    g_variant_builder_add(&fields, "{sv}", "status", g_variant_new_string("ok"));
    g_variant_builder_add(&fields, "{sv}", "bundle.version", g_variant_new_string(version));
    g_variant_builder_add(&fields, "{sv}", "installed.timestamp",
                          g_variant_new_uint64(timestamp));
    g_variant_builder_add(slots, "(sa{sv})", name, &fields);
}

static void handle_method_call(GDBusConnection* connection, const gchar* sender,
                               const gchar* object_path, const gchar* interface_name,
                               const gchar* method_name, GVariant* parameters,
                               GDBusMethodInvocation* invocation, gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    if (g_strcmp0(method_name, "InstallBundle") == 0 || g_strcmp0(method_name, "Install") == 0)
    {
        if (g_strcmp0(g_operation, "idle") != 0)
        {
            g_dbus_method_invocation_return_dbus_error(
                invocation, "de.pengutronix.rauc.Installer.Error",
                "already processing a different method");
            return;
        }
        const gchar* source = NULL;
        if (g_strcmp0(method_name, "InstallBundle") == 0)
        {
            g_variant_get(parameters, "(&s@a{sv})", &source, NULL);
        }
        else
        {
            g_variant_get(parameters, "(&s)", &source);
        }
        g_printerr("rauc-stub: InstallBundle(%s)\n", source != NULL ? source : "?");

        g_dbus_method_invocation_return_value(invocation, NULL);
        set_operation("installing");
        g_timeout_add(300, advance, GSIZE_TO_POINTER(0));
        return;
    }

    if (g_strcmp0(method_name, "GetSlotStatus") == 0)
    {
        GVariantBuilder slots;
        g_variant_builder_init(&slots, G_VARIANT_TYPE("a(sa{sv})"));
        add_slot(&slots, "rootfs.0", "/dev/disk/by-partuuid/52441003-0001", "A", "booted",
                 "1.0-20260917013010", 1758067810u);
        add_slot(&slots, "rootfs.1", "/dev/disk/by-partuuid/52441003-0002", "B", "inactive",
                 "1.0-20260916094501", 1757980101u);
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(a(sa{sv}))", &slots));
        return;
    }

    if (g_strcmp0(method_name, "GetPrimary") == 0)
    {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", "rootfs.0"));
        return;
    }

    if (g_strcmp0(method_name, "Info") == 0)
    {
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(ss)", "redline-lattepanda-mu", "1.0-20260917013010"));
        return;
    }

    if (g_strcmp0(method_name, "Mark") == 0)
    {
        const gchar* state = NULL;
        const gchar* slot = NULL;
        g_variant_get(parameters, "(&s&s)", &state, &slot);
        gchar* message = g_strdup_printf("marked slot rootfs.0 as %s (asked for '%s')",
                                         state != NULL ? state : "", slot != NULL ? slot : "");
        // Two out-args, which the client is easy to get wrong about.
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(ss)", "rootfs.0", message));
        g_free(message);
        return;
    }

    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.freedesktop.DBus.Error.UnknownMethod", method_name);
}

static GVariant* handle_get_property(GDBusConnection* connection, const gchar* sender,
                                     const gchar* object_path, const gchar* interface_name,
                                     const gchar* property_name, GError** error,
                                     gpointer user_data)
{
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)error;
    (void)user_data;

    if (g_strcmp0(property_name, "Operation") == 0)
    {
        return g_variant_new_string(g_operation);
    }
    if (g_strcmp0(property_name, "LastError") == 0)
    {
        return g_variant_new_string(g_last_error);
    }
    if (g_strcmp0(property_name, "Progress") == 0)
    {
        return g_variant_new("(isi)", g_percentage, g_message, g_nesting);
    }
    if (g_strcmp0(property_name, "Compatible") == 0)
    {
        return g_variant_new_string("redline-lattepanda-mu");
    }
    if (g_strcmp0(property_name, "Variant") == 0)
    {
        return g_variant_new_string("");
    }
    if (g_strcmp0(property_name, "BootSlot") == 0)
    {
        return g_variant_new_string("A");
    }
    return NULL;
}

static const GDBusInterfaceVTable kVTable = {
    handle_method_call,
    handle_get_property,
    NULL,  // no writable properties on this interface
    {NULL}
};

static void on_bus_acquired(GDBusConnection* connection, const gchar* name, gpointer user_data)
{
    (void)name;
    (void)user_data;

    g_connection = connection;
    GError* error = NULL;
    const guint id = g_dbus_connection_register_object(
        connection, "/", g_node->interfaces[0], &kVTable, NULL, NULL, &error);
    if (id == 0)
    {
        g_printerr("rauc-stub: cannot register the object: %s\n",
                   error != NULL ? error->message : "unknown error");
        if (error != NULL) { g_error_free(error); }
    }
}

static void on_name_acquired(GDBusConnection* connection, const gchar* name, gpointer user_data)
{
    (void)connection;
    (void)user_data;
    g_printerr("rauc-stub: owning %s (mode=%s)\n", name,
               g_mode == MODE_FAIL ? "fail" : g_mode == MODE_BUSY ? "busy" : "ok");
}

static void on_name_lost(GDBusConnection* connection, const gchar* name, gpointer user_data)
{
    (void)connection;
    (void)user_data;
    g_printerr("rauc-stub: lost %s -- is something else owning it?\n", name);
    exit(1);
}

int main(int argc, char** argv)
{
    gboolean use_system_bus = FALSE;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--fail") == 0) { g_mode = MODE_FAIL; }
        else if (strcmp(argv[i], "--busy") == 0) { g_mode = MODE_BUSY; }
        else if (strcmp(argv[i], "--system") == 0) { use_system_bus = TRUE; }
        else
        {
            g_printerr("usage: %s [--fail] [--busy] [--system]\n", argv[0]);
            return 2;
        }
    }

    g_operation = g_strdup(g_mode == MODE_BUSY ? "installing" : "idle");
    g_last_error = g_strdup("");
    g_message = g_strdup("");

    GError* error = NULL;
    g_node = g_dbus_node_info_new_for_xml(kIntrospectionXml, &error);
    if (g_node == NULL)
    {
        g_printerr("rauc-stub: bad introspection XML: %s\n",
                   error != NULL ? error->message : "unknown error");
        return 1;
    }

    // The session bus by default: the point is to develop without root and
    // without touching anything a real RAUC would own.
    g_bus_own_name(use_system_bus ? G_BUS_TYPE_SYSTEM : G_BUS_TYPE_SESSION,
                   "de.pengutronix.rauc", G_BUS_NAME_OWNER_FLAGS_NONE, on_bus_acquired,
                   on_name_acquired, on_name_lost, NULL, NULL);

    g_main_loop_run(g_main_loop_new(NULL, FALSE));
    return 0;
}
