#include "rauc_client/rauc_client.h"

#include <gio/gio.h>

#include <spdlog/spdlog.h>

#include <condition_variable>
#include <mutex>
#include <thread>

namespace rauc_client
{
namespace
{

constexpr const char* kBusName = "de.pengutronix.rauc";
constexpr const char* kObjectPath = "/";
constexpr const char* kInterface = "de.pengutronix.rauc.Installer";

// GVariant's refcounting does not mix with early returns; this keeps the
// unref honest without a macro.
struct VariantUnref
{
    void operator()(GVariant* v) const noexcept
    {
        if (v != nullptr) { g_variant_unref(v); }
    }
};
using VariantPtr = std::unique_ptr<GVariant, VariantUnref>;

std::string stringOr(GVariant* dict, const char* key, const std::string& fallback = {})
{
    if (dict == nullptr) { return fallback; }
    const char* text = nullptr;
    if (g_variant_lookup(dict, key, "&s", &text) && text != nullptr)
    {
        return std::string(text);
    }
    return fallback;
}

}  // namespace

struct Installer::Impl
{
    Impl(Bus b, Callbacks cb) : bus(b), callbacks(std::move(cb)) {}

    Bus bus;
    Callbacks callbacks;

    GMainContext* context { nullptr };
    GMainLoop* loop { nullptr };
    GDBusProxy* proxy { nullptr };
    std::thread thread;

    mutable std::mutex mutex;
    bool ready { false };

    // Property reads go through the proxy's cache, which GDBus keeps current
    // from PropertiesChanged. Reading from another thread is safe; calling
    // methods from one is not, which is why installBundle hops to the loop.
    std::string cachedString(const char* name) const
    {
        if (proxy == nullptr) { return {}; }
        VariantPtr value(g_dbus_proxy_get_cached_property(proxy, name));
        if (!value) { return {}; }
        const char* text = g_variant_get_string(value.get(), nullptr);
        return text != nullptr ? std::string(text) : std::string();
    }
};

namespace
{

void onPropertiesChanged(GDBusProxy*, GVariant* changed, const gchar* const*, gpointer user)
{
    auto* impl = static_cast<Installer::Impl*>(user);

    GVariant* progress = g_variant_lookup_value(changed, "Progress", G_VARIANT_TYPE("(isi)"));
    if (progress != nullptr && impl->callbacks.onProgress)
    {
        gint32 percentage = 0;
        const gchar* message = nullptr;
        gint32 nesting = 0;
        g_variant_get(progress, "(i&si)", &percentage, &message, &nesting);
        impl->callbacks.onProgress(Progress{
            .percentage = percentage,
            .message = message != nullptr ? std::string(message) : std::string(),
            .nesting = nesting,
        });
        g_variant_unref(progress);
    }
}

void onSignal(GDBusProxy*, const gchar*, const gchar* signal, GVariant* params, gpointer user)
{
    auto* impl = static_cast<Installer::Impl*>(user);
    if (g_strcmp0(signal, "Completed") != 0) { return; }

    gint32 result = -1;
    g_variant_get(params, "(i)", &result);
    if (impl->callbacks.onCompleted)
    {
        impl->callbacks.onCompleted(result, impl->cachedString("LastError"));
    }
}

}  // namespace

Installer::Installer(Bus bus, Callbacks callbacks)
    : impl_(std::make_unique<Impl>(bus, std::move(callbacks)))
{
}

Installer::~Installer()
{
    if (impl_->loop != nullptr)
    {
        g_main_loop_quit(impl_->loop);
    }
    if (impl_->thread.joinable())
    {
        impl_->thread.join();
    }
    if (impl_->proxy != nullptr) { g_object_unref(impl_->proxy); }
    if (impl_->loop != nullptr) { g_main_loop_unref(impl_->loop); }
    if (impl_->context != nullptr) { g_main_context_unref(impl_->context); }
}

bool Installer::connect(std::string& error)
{
    std::mutex startup;
    std::unique_lock<std::mutex> lock(startup);
    bool done = false;
    std::condition_variable started;

    impl_->thread = std::thread([this, &error, &done, &started, &startup] {
        // THE CONTEXT MUST BE THREAD-DEFAULT BEFORE THE PROXY IS CREATED.
        // GDBus binds the proxy's callbacks to whatever context is current at
        // construction, so creating it on the caller's thread and then running
        // a loop here delivers nothing -- silently.
        impl_->context = g_main_context_new();
        g_main_context_push_thread_default(impl_->context);
        impl_->loop = g_main_loop_new(impl_->context, FALSE);

        GError* gerror = nullptr;
        impl_->proxy = g_dbus_proxy_new_for_bus_sync(
            impl_->bus == Bus::Session ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM,
            G_DBUS_PROXY_FLAGS_NONE,
            nullptr,  // no interface info: the XML ships in rauc-dev, not on the image
            kBusName, kObjectPath, kInterface, nullptr, &gerror);

        {
            std::lock_guard<std::mutex> guard(startup);
            if (impl_->proxy == nullptr)
            {
                error = gerror != nullptr ? gerror->message : "could not create the RAUC proxy";
                if (gerror != nullptr) { g_error_free(gerror); }
            }
            else
            {
                g_signal_connect(impl_->proxy, "g-properties-changed",
                                 G_CALLBACK(onPropertiesChanged), impl_.get());
                g_signal_connect(impl_->proxy, "g-signal", G_CALLBACK(onSignal), impl_.get());
                impl_->ready = true;
            }
            done = true;
        }
        started.notify_one();

        if (impl_->ready)
        {
            g_main_loop_run(impl_->loop);
        }
        g_main_context_pop_thread_default(impl_->context);
    });

    started.wait(lock, [&done] { return done; });
    return impl_->ready;
}

bool Installer::connected() const
{
    std::lock_guard<std::mutex> guard(impl_->mutex);
    return impl_->ready;
}

bool Installer::serviceAvailable() const
{
    if (!connected()) { return false; }
    gchar* owner = g_dbus_proxy_get_name_owner(impl_->proxy);
    if (owner == nullptr) { return false; }
    g_free(owner);
    return true;
}

std::optional<Status> Installer::status() const
{
    if (!connected()) { return std::nullopt; }

    Status status;
    status.operation = impl_->cachedString("Operation");
    status.lastError = impl_->cachedString("LastError");
    status.compatible = impl_->cachedString("Compatible");
    status.variant = impl_->cachedString("Variant");
    status.bootSlot = impl_->cachedString("BootSlot");

    GError* gerror = nullptr;
    VariantPtr primary(g_dbus_proxy_call_sync(impl_->proxy, "GetPrimary", nullptr,
                                              G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &gerror));
    if (primary)
    {
        const gchar* text = nullptr;
        g_variant_get(primary.get(), "(&s)", &text);
        if (text != nullptr) { status.primary = text; }
    }
    else if (gerror != nullptr)
    {
        // Not fatal: a board that has never been marked good has no primary.
        SPDLOG_DEBUG("[rauc] GetPrimary: {}", gerror->message);
        g_error_free(gerror);
    }
    return status;
}

std::vector<SlotStatus> Installer::slots() const
{
    std::vector<SlotStatus> slots;
    if (!connected()) { return slots; }

    GError* gerror = nullptr;
    VariantPtr reply(g_dbus_proxy_call_sync(impl_->proxy, "GetSlotStatus", nullptr,
                                            G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &gerror));
    if (!reply)
    {
        if (gerror != nullptr)
        {
            SPDLOG_WARN("[rauc] GetSlotStatus: {}", gerror->message);
            g_error_free(gerror);
        }
        return slots;
    }

    // a(sa{sv}) -- the dictionary is open ended, so only the keys the console
    // shows are lifted out and the rest are left alone.
    VariantPtr array(g_variant_get_child_value(reply.get(), 0));
    GVariantIter iter;
    g_variant_iter_init(&iter, array.get());

    const gchar* name = nullptr;
    GVariant* dict = nullptr;
    while (g_variant_iter_next(&iter, "(&s@a{sv})", &name, &dict))
    {
        SlotStatus slot;
        slot.name = name != nullptr ? name : "";
        slot.device = stringOr(dict, "device");
        slot.bootname = stringOr(dict, "bootname");
        slot.state = stringOr(dict, "state");
        slot.bootStatus = stringOr(dict, "boot-status");
        slot.status = stringOr(dict, "status");
        slot.bundleVersion = stringOr(dict, "bundle.version");

        guint64 timestamp = 0;
        if (g_variant_lookup(dict, "installed.timestamp", "t", &timestamp))
        {
            slot.installedTimestamp = timestamp;
        }
        slots.push_back(std::move(slot));
        g_variant_unref(dict);
    }
    return slots;
}

bool Installer::installBundle(const std::string& path, std::string& error)
{
    if (!connected())
    {
        error = "not connected to RAUC";
        return false;
    }

    // Refused rather than queued: RAUC itself rejects a concurrent install, and
    // finding that out from a D-Bus error after the upload has already landed is
    // a worse experience than being told now.
    const std::string operation = impl_->cachedString("Operation");
    if (!operation.empty() && operation != "idle")
    {
        error = "RAUC is busy (" + operation + ")";
        return false;
    }

    GVariantBuilder args;
    g_variant_builder_init(&args, G_VARIANT_TYPE("a{sv}"));

    GError* gerror = nullptr;
    VariantPtr reply(g_dbus_proxy_call_sync(
        impl_->proxy, "InstallBundle",
        g_variant_new("(sa{sv})", path.c_str(), &args),
        G_DBUS_CALL_FLAGS_NONE, 30000, nullptr, &gerror));

    if (!reply)
    {
        error = gerror != nullptr ? gerror->message : "InstallBundle failed";
        if (gerror != nullptr) { g_error_free(gerror); }
        return false;
    }
    return true;
}

bool Installer::mark(const std::string& state, const std::string& slot, std::string& slotName,
                     std::string& message, std::string& error)
{
    if (!connected())
    {
        error = "not connected to RAUC";
        return false;
    }

    GError* gerror = nullptr;
    VariantPtr reply(g_dbus_proxy_call_sync(
        impl_->proxy, "Mark", g_variant_new("(ss)", state.c_str(), slot.c_str()),
        G_DBUS_CALL_FLAGS_NONE, 10000, nullptr, &gerror));

    if (!reply)
    {
        error = gerror != nullptr ? gerror->message : "Mark failed";
        if (gerror != nullptr) { g_error_free(gerror); }
        return false;
    }

    // Two out-args, which is easy to get wrong: (s slot_name, s message).
    const gchar* returnedSlot = nullptr;
    const gchar* returnedMessage = nullptr;
    g_variant_get(reply.get(), "(&s&s)", &returnedSlot, &returnedMessage);
    slotName = returnedSlot != nullptr ? returnedSlot : "";
    message = returnedMessage != nullptr ? returnedMessage : "";
    return true;
}

}  // namespace rauc_client
