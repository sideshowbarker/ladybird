/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AtkBridge.h"
#include "LadybirdAtkObject.h"

#include <AK/Array.h>
#include <AK/Format.h>

#include <atk/atk.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <glib.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Ladybird {

// --- Application root AtkObject subclass ---
// The application root is the permanent top-level accessible that the ATK bridge caches at init time. Its children
// are dynamically set by AtkBridge::update_tree() when the web content tree arrives.

struct LadybirdAppRoot {
    AtkObject parent;
};

struct LadybirdAppRootClass {
    AtkObjectClass parent_class;
};

GType ladybird_app_root_get_type();
G_DEFINE_TYPE(LadybirdAppRoot, ladybird_app_root, ATK_TYPE_OBJECT)

static gint ladybird_app_root_get_n_children(AtkObject*)
{
    if (AtkBridge::the().document_root())
        return 1;
    return 0;
}

static AtkObject* ladybird_app_root_ref_child(AtkObject*, gint index)
{
    if (index == 0 && AtkBridge::the().document_root()) {
        auto* child = AtkBridge::the().document_root();
        g_object_ref(child);
        return child;
    }
    return nullptr;
}

static void ladybird_app_root_init(LadybirdAppRoot*) { }

static void ladybird_app_root_class_init(LadybirdAppRootClass* klass)
{
    auto* atk_class = ATK_OBJECT_CLASS(klass);
    atk_class->get_n_children = ladybird_app_root_get_n_children;
    atk_class->ref_child = ladybird_app_root_ref_child;
}

// Custom AtkUtil subclass — overrides get_root() and get_toolkit_name() so the ATK bridge exposes our tree (not
// GTK/Qt's) and reports "Ladybird" as the toolkit to AT-SPI2 clients.
struct LadybirdAtkUtilClass {
    AtkUtilClass parent_class;
};

struct LadybirdAtkUtil {
    AtkUtil parent;
};

static AtkObject* ladybird_atk_util_get_root()
{
    return AtkBridge::the().root_object();
}

static char const* ladybird_atk_util_get_toolkit_name()
{
    return "Ladybird";
}

static char const* ladybird_atk_util_get_toolkit_version()
{
    return "0.1";
}

// Register the GType. G_DEFINE_TYPE generates the boilerplate.
GType ladybird_atk_util_get_type();
G_DEFINE_TYPE(LadybirdAtkUtil, ladybird_atk_util, ATK_TYPE_UTIL)

static void ladybird_atk_util_init(LadybirdAtkUtil*)
{
}

static void ladybird_atk_util_class_init(LadybirdAtkUtilClass*)
{
    auto* atk_class = ATK_UTIL_CLASS(g_type_class_peek(ATK_TYPE_UTIL));
    atk_class->get_root = ladybird_atk_util_get_root;
    atk_class->get_toolkit_name = ladybird_atk_util_get_toolkit_name;
    atk_class->get_toolkit_version = ladybird_atk_util_get_toolkit_version;
}

// Directly register AtkObjects with the bridge's SpiRegister so they get D-Bus paths. This bypasses
// the bridge's cache/idle-callback mechanism which requires a running GLib main loop on the right context.
using RegisterFunc = char* (*)(void*, GObject*);
static RegisterFunc s_register_object = nullptr;
static void** s_global_register = nullptr;

static void register_atk_tree_recursively(AtkObject* obj, int depth = 0)
{
    if (!obj || depth > 50 || !s_register_object || !s_global_register || !*s_global_register)
        return;

    char* path = s_register_object(*s_global_register, G_OBJECT(obj));
    if (path)
        g_free(path);

    int child_count = atk_object_get_n_accessible_children(obj);
    for (int i = 0; i < child_count; ++i) {
        AtkObject* child = atk_object_ref_accessible_child(obj, i);
        if (child) {
            register_atk_tree_recursively(child, depth + 1);
            g_object_unref(child);
        }
    }
}

static AtkBridge* s_the = nullptr;

AtkBridge& AtkBridge::the()
{
    VERIFY(s_the);
    return *s_the;
}

AtkBridge::AtkBridge()
{
    VERIFY(!s_the);
    s_the = this;
}

AtkBridge::~AtkBridge()
{
    // Reap the registryd child first so it isn't left as a zombie when we tear down the bus it's connected to.
    if (m_registryd_pid > 0) {
        kill(m_registryd_pid, SIGTERM);
        waitpid(m_registryd_pid, nullptr, 0);
        m_registryd_pid = -1;
    }

    // Quit and join the GLib thread that drives the bridge's private D-Bus main context. The bridge's cleanup
    // function below may synchronously dispatch messages on that context, so we stop the thread after cleanup.
    if (m_atk_bridge_lib) {
        // Call atk_bridge_adaptor_cleanup if available.
        using CleanupFunc = void (*)();
        if (auto cleanup = reinterpret_cast<CleanupFunc>(dlsym(m_atk_bridge_lib, "atk_bridge_adaptor_cleanup")))
            cleanup();
        dlclose(m_atk_bridge_lib);
    }

    if (m_bridge_thread_running) {
        if (m_bridge_main_loop)
            g_main_loop_quit(m_bridge_main_loop);
        pthread_join(m_bridge_thread, nullptr);
        if (m_bridge_main_loop) {
            g_main_loop_unref(m_bridge_main_loop);
            m_bridge_main_loop = nullptr;
        }
        m_bridge_thread_running = false;
    }

    m_private_bus.stop();
    s_the = nullptr;
}

bool AtkBridge::initialize()
{
    if (m_initialized)
        return true;

    // Step 1: Start the private D-Bus daemon.
    auto address = m_private_bus.start();
    if (!address.has_value()) {
        dbgln("AtkBridge: Failed to start private D-Bus daemon");
        return false;
    }

    // Step 2: Point libatspi at the private bus. Qt's bridge already connected to the normal bus (using QDBus, a
    // completely separate D-Bus stack), so this env var only affects the next atspi_get_a11y_bus() call — which is
    // our ATK bridge's call.
    setenv("AT_SPI_BUS_ADDRESS", address.value().characters(), 1);

    // Create a placeholder root AtkObject so the bridge has a non-NULL root at init time. The real tree root
    // replaces this when the first accessibility tree arrives via update_tree(). g_object_new returns a
    // full reference that GObjectPtr adopts; we don't add another one.
    m_root_object = GObjectPtr<AtkObject>::adopt(ATK_OBJECT(g_object_new(ladybird_app_root_get_type(), nullptr)));
    atk_object_set_role(m_root_object.get(), ATK_ROLE_APPLICATION);
    atk_object_set_name(m_root_object.get(), "Ladybird");

    // Step 3: Register our custom AtkUtil class. This overrides the global ATK root object and toolkit name, so the
    // bridge exposes our tree instead of any toolkit default.
    g_type_class_unref(g_type_class_ref(ladybird_atk_util_get_type()));

    // Step 4: Load libatk-bridge-2.0 via dlopen (following Firefox's pattern in accessible/atk/Platform.cpp).
    m_atk_bridge_lib = dlopen("libatk-bridge-2.0.so.0", RTLD_LAZY);
    if (!m_atk_bridge_lib) {
        dbgln("AtkBridge: Failed to dlopen libatk-bridge-2.0.so.0: {}", dlerror());
        unsetenv("AT_SPI_BUS_ADDRESS");
        return false;
    }

    // Get the direct object registration function to bypass the cache.
    s_register_object = reinterpret_cast<RegisterFunc>(dlsym(m_atk_bridge_lib, "spi_register_object_to_path"));
    s_global_register = reinterpret_cast<void**>(dlsym(m_atk_bridge_lib, "spi_global_register"));
    if (s_register_object && s_global_register)
        dbgln("AtkBridge: Got spi_register_object_to_path for direct registration");

    using InitFunc = int (*)(int*, char***);
    auto init = reinterpret_cast<InitFunc>(dlsym(m_atk_bridge_lib, "atk_bridge_adaptor_init"));
    if (!init) {
        dbgln("AtkBridge: Failed to find atk_bridge_adaptor_init: {}", dlerror());
        dlclose(m_atk_bridge_lib);
        m_atk_bridge_lib = nullptr;
        unsetenv("AT_SPI_BUS_ADDRESS");
        return false;
    }

    // Call the bridge init. This connects to the private bus via atspi_get_a11y_bus() (which reads
    // AT_SPI_BUS_ADDRESS) and registers our AtkUtil's root object on it.
    int result = init(nullptr, nullptr);
    dbgln("AtkBridge: atk_bridge_adaptor_init returned {}", result);

    // The bridge creates a private GMainContext for its D-Bus connection (spi_global_app_data->main_context) and
    // uses spi_context (initially NULL = default context) for idle callbacks. We need to pump BOTH:
    // 1. A dedicated thread for the bridge's private context (D-Bus message dispatch)
    // 2. A QTimer for the default context (cache idle callbacks via spi_idle_add)

    // Get the bridge's private GMainContext from the exported spi_global_app_data symbol.
    struct SpiBridge {
        GObject parent;
        AtkObject* root;
        void* bus;
        void* droute;
        GMainContext* main_context;
    };
    auto** app_data_ptr = reinterpret_cast<SpiBridge**>(dlsym(m_atk_bridge_lib, "spi_global_app_data"));
    if (app_data_ptr && *app_data_ptr && (*app_data_ptr)->main_context) {
        GMainContext* bridge_ctx = (*app_data_ptr)->main_context;
        m_bridge_main_loop = g_main_loop_new(bridge_ctx, FALSE);
        if (pthread_create(&m_bridge_thread, nullptr, [](void* loop) -> void* {
                g_main_loop_run(static_cast<GMainLoop*>(loop));
                return nullptr; }, m_bridge_main_loop) == 0) {
            m_bridge_thread_running = true;
            dbgln("AtkBridge: Started GLib thread for bridge D-Bus context");
        } else {
            g_main_loop_unref(m_bridge_main_loop);
            m_bridge_main_loop = nullptr;
            dbgln("AtkBridge: pthread_create failed for bridge D-Bus context");
        }
    } else {
        dbgln("AtkBridge: Could not get bridge main_context from spi_global_app_data");
    }

    // Step 5: Restore the environment so any future code that checks AT_SPI_BUS_ADDRESS isn't affected.
    unsetenv("AT_SPI_BUS_ADDRESS");

    // Step 5b: Spawn the AT-SPI2 registry daemon on the private bus. The bridge made async calls to the registry
    // during init (GetRegisteredEvents, GetKeystrokeListeners) which got error replies because there was no
    // registryd yet. Starting it now means subsequent calls (Collection, Cache, etc.) will work. The GLib thread
    // will process the registryd's responses when they arrive.
    auto bus_address = address.value();

    // Build the child's environment in the parent, before fork. Calling setenv() in the child is unsafe
    // in a multi-threaded Qt process (POSIX only allows async-signal-safe functions between fork and
    // exec, and setenv is not in that set). Instead, we construct a replacement envp here and pass it
    // directly to execve in the child.
    auto at_spi_env = ByteString::formatted("AT_SPI_BUS_ADDRESS={}", bus_address);
    auto dbus_env = ByteString::formatted("DBUS_SESSION_BUS_ADDRESS={}", bus_address);
    Vector<char const*> registryd_envp;
    // Inherit every entry from the parent's environment except the two we're overriding.
    for (char** entry = environ; *entry; ++entry) {
        StringView view { *entry, strlen(*entry) };
        if (view.starts_with("AT_SPI_BUS_ADDRESS="sv) || view.starts_with("DBUS_SESSION_BUS_ADDRESS="sv))
            continue;
        registryd_envp.append(*entry);
    }
    registryd_envp.append(at_spi_env.characters());
    registryd_envp.append(dbus_env.characters());
    registryd_envp.append(nullptr);

    m_registryd_pid = fork();
    if (m_registryd_pid == 0) {
        // Redirect stdout/stderr to /dev/null to suppress "SpiRegistry daemon is running..." message.
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        // If our parent dies (including SIGKILL from a crash), the kernel delivers SIGTERM to this
        // registryd. at-spi2-registryd handles SIGTERM and shuts down cleanly, so we don't leak
        // orphaned daemons.
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        // at-spi2-registryd is a libexec helper and is typically not on $PATH. Probe the known install locations in
        // order of commonality across distributions.
        static constexpr Array<char const*, 4> registryd_paths {
            "/usr/libexec/at-spi2-registryd",          // Fedora, RHEL, Debian, Arch
            "/usr/lib/at-spi2-core/at-spi2-registryd", // Some older Debian/Ubuntu layouts
            "/usr/local/libexec/at-spi2-registryd",    // User-local installs
            "/app/libexec/at-spi2-registryd",          // Flatpak runtime
        };
        char* const argv[] = { const_cast<char*>("at-spi2-registryd"), nullptr };
        for (auto const* path : registryd_paths)
            execve(path, argv, const_cast<char* const*>(registryd_envp.data()));
        _exit(127);
    }
    if (m_registryd_pid > 0)
        dbgln("AtkBridge: Started at-spi2-registryd (PID {}) on private bus", m_registryd_pid);

    m_initialized = true;
    dbgln("AtkBridge: Initialized on private bus {}", address.value());
    return true;
}

void AtkBridge::set_active_tree(WebView::AccessibilityTreeManager const* manager,
    AccessibilityActionCallback* action_callback, AccessibilityTextActionCallback* text_action_callback)
{
    m_manager = manager;
    m_action_callback = action_callback;
    m_text_action_callback = text_action_callback;
}

void AtkBridge::update_tree()
{
    if (!m_manager)
        return;

    auto root_id = m_manager->root_id();
    if (root_id < 0)
        return;

    // Drop cached AtkObject entries for node_ids that no longer exist in the tree. The ATK bridge's
    // own SpiRegister may still hold references, which keeps deregistration consistent; what we
    // avoid is the per-node entry in our own cache accumulating forever across page navigations.
    ladybird_atk_object_prune_cache(*m_manager);

    // Create the document root from the tree's root node.
    m_document_root = ladybird_atk_object_new(root_id, m_manager, m_action_callback, m_text_action_callback);

    // Set the document root's parent to our application root so the bridge can walk the tree upward.
    if (m_document_root)
        atk_object_set_parent(m_document_root, m_root_object.get());

    // Register every AtkObject in the tree directly with the bridge's SpiRegister so they get D-Bus paths.
    if (m_document_root)
        register_atk_tree_recursively(m_document_root);

    // Also signal children-changed so any listeners (cache, etc.) pick up the new tree.
    if (m_root_object)
        g_signal_emit_by_name(m_root_object.get(), "children-changed::add", 0, m_document_root, nullptr);

    // Immediately pump the default context to process the cache's idle callback (add_pending_items) which was
    // just scheduled by the children-changed signal handler (toplevel_added_listener → add_subtree).
    for (int i = 0; i < 50; ++i)
        g_main_context_iteration(g_main_context_default(), FALSE);
}

void AtkBridge::notify_focus_changed(i64 node_id)
{
    if (!m_manager || !m_manager->node(node_id))
        return;

    auto* obj = ladybird_atk_object_new(node_id, m_manager, m_action_callback, m_text_action_callback);
    if (!obj)
        return;

    // Fire both state-change and focus-event signals — Orca listens for both.
    atk_object_notify_state_change(obj, ATK_STATE_FOCUSED, TRUE);
    g_signal_emit_by_name(obj, "focus-event", TRUE);
}

void AtkBridge::pump_default_context()
{
    while (g_main_context_iteration(g_main_context_default(), FALSE))
        ;
}

void AtkBridge::activate_cache()
{
    if (!m_atk_bridge_lib)
        return;
    using ActivateFunc = void (*)();
    if (auto activate = reinterpret_cast<ActivateFunc>(dlsym(m_atk_bridge_lib, "spi_atk_activate")))
        activate();
}

}
