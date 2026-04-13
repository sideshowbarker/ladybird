"""Custom script utilities for Ladybird browser.

Patches Orca’s Collection-based structural navigation to use
Ladybird’s private ATK bridge when Qt’s bridge returns empty
results. The private bridge provides full AT-SPI2 Collection
support via a single D-Bus call, replacing the slower Python DFS
fallback. When Qt fixes Collection (expected in 6.11+), the
private-bus path is bypassed because Qt’s Collection returns
results first."""

from __future__ import annotations

import logging

from orca import focus_manager
from orca.ax_object import AXObject
from orca.ax_utilities import AXUtilities
from orca.scripts import web

# Module logger; silent by default. Users or CI can enable with:
#     logging.getLogger("ladybird.orca").setLevel(logging.DEBUG)
_log = logging.getLogger("ladybird.orca")

# --- Private AT-SPI2 bus connection ---
# Ladybird runs a direct ATK bridge on a private D-Bus bus, providing full AT-SPI2 interface support (Collection,
# Hypertext, Document, Cache) that Qt's bridge lacks. This module connects to that bus and uses it as a fast oracle
# for operations the Qt bridge does poorly.

_private_bus_connection = None
_private_bus_name = None


def _connect_private_bus():
    """Connect to Ladybird's private AT-SPI2 bus.

    Reads the bus address from the well-known file written by PrivateAccessibilityBus on Ladybird startup."""

    global _private_bus_connection, _private_bus_name

    if _private_bus_connection is not None:
        return _private_bus_connection

    import os

    try:
        import gi

        gi.require_version("Gio", "2.0")
        from gi.repository import Gio
        from gi.repository import GLib
    except Exception:
        return None

    # Find the address file for the currently focused Ladybird instance.
    runtime_dir = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
    address = None

    # Search for any ladybird-a11y-*.address file.
    for entry in os.listdir(runtime_dir):
        if entry.startswith("ladybird-a11y-") and entry.endswith(".address"):
            try:
                with open(os.path.join(runtime_dir, entry)) as f:
                    address = f.read().strip()
                if address:
                    break
            except Exception:
                continue

    if not address:
        _log.debug("No private bus address file found")
        return None

    try:
        # Connect to the private bus.
        conn = Gio.DBusConnection.new_for_address_sync(
            address,
            Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
            None,  # observer
            None,  # cancellable
        )
        _private_bus_connection = conn

        # Find the bus name of the Ladybird ATK bridge on the private bus. Probe each unique name for the one
        # that has an application-role root — that’s the bridge (vs. registryd which has a desktop-frame root).
        result = conn.call_sync(
            "org.freedesktop.DBus",
            "/org/freedesktop/DBus",
            "org.freedesktop.DBus",
            "ListNames",
            None,
            GLib.VariantType.new("(as)"),
            Gio.DBusCallFlags.NONE,
            1000,
            None,
        )
        names = result.get_child_value(0).unpack()
        for name in names:
            if not name.startswith(":"):
                continue
            try:
                r = conn.call_sync(
                    name,
                    "/org/a11y/atspi/accessible/root",
                    "org.a11y.atspi.Accessible",
                    "GetRoleName",
                    None,
                    GLib.VariantType.new("(s)"),
                    Gio.DBusCallFlags.NONE,
                    500,
                    None,
                )
                if r.get_child_value(0).get_string() == "application":
                    _private_bus_name = name
                    break
            except Exception:
                continue

        _log.info("Connected to private bus at %s, name=%s", address, _private_bus_name)
        return conn
    except Exception as e:
        _log.warning("Failed to connect to private bus: %s", e)
        return None


def _tree_search_by_role(root, roles, pred=None):
    """Manual DFS tree search for objects matching any of the given roles.

    This is the fallback when Qt's Collection GetMatches returns empty."""

    if root is None:
        return []

    results = []

    def walk(obj, depth=0):
        if obj is None or depth > 50:
            return
        role = AXObject.get_role(obj)
        if role in roles:
            if pred is None or pred(obj):
                results.append(obj)
        for i in range(AXObject.get_child_count(obj)):
            walk(AXObject.get_child(obj, i), depth + 1)

    walk(root)
    return results


def _tree_search_by_role_and_states(root, roles, states, pred=None):
    """Manual DFS tree search for objects matching any of the given roles
    AND having ALL of the given states."""

    if root is None:
        return []

    results = []

    def walk(obj, depth=0):
        if obj is None or depth > 50:
            return
        role = AXObject.get_role(obj)
        if role in roles:
            try:
                state_set = obj.get_state_set()
                has_all = all(state_set.contains(s) for s in states)
            except Exception:
                has_all = False
            if has_all and (pred is None or pred(obj)):
                results.append(obj)
        for i in range(AXObject.get_child_count(obj)):
            walk(AXObject.get_child(obj, i), depth + 1)

    walk(root)
    return results


_collection_fallback_installed = False
_sayall_patched = False


def _install_sayall_patch():
    """Monkey-patch SayAllPresenter.say_all to move locus to document
    content before starting, if locus is outside the document."""

    global _sayall_patched
    if _sayall_patched:
        return
    _sayall_patched = True

    try:
        from orca import say_all_presenter

        presenter = say_all_presenter.get_presenter()
        _orig_say_all = presenter.say_all

        def _patched_say_all(script, event=None, notify_user=True, obj=None, offset=None):
            if obj is None and hasattr(script, "utilities"):
                # Always clear cached document and caret context so
                # Say All finds the CURRENT tab's content, not stale
                # objects from a previous tab.
                script.utilities._cached_active_document = None
                script.utilities._cache_warmed = False
                script.utilities.clear_cached_objects()

                doc = script.utilities.active_document()
                if doc is not None:
                    first = (
                        script.utilities._find_first_content_child(doc)
                        if hasattr(script.utilities, "_find_first_content_child")
                        else None
                    )
                    if first is not None:
                        script.utilities.set_caret_context(first, 0, doc)
                        obj = first
                        offset = 0

            return _orig_say_all(script, event, notify_user, obj, offset)

        # Patch both the instance method AND the stored handler reference
        presenter.say_all = _patched_say_all
        if "sayAllHandler" in presenter._handlers:
            presenter._handlers["sayAllHandler"].function = _patched_say_all
    except Exception as e:
        _log.warning("Failed to install Say All patch: %s", e)


_scroll_patched = False


def _install_scroll_patch():
    """Monkey-patch AXEventSynthesizer.scroll_to_center to also trigger
    ScrollSubstringTo (which Qt handles) in addition to
    ScrollSubstringToPoint (which Qt does not handle). This makes the
    focus ring appear during structural navigation (H/K/I/L keys)."""

    global _scroll_patched
    if _scroll_patched:
        return
    _scroll_patched = True

    try:
        from orca.ax_event_synthesizer import AXEventSynthesizer
        from orca.ax_text import AXText

        _orig_scroll_to_center = AXEventSynthesizer.scroll_to_center

        @staticmethod
        def _patched_scroll_to_center(obj, start_offset=None, end_offset=None):
            # Call the original (tries ScrollSubstringToPoint + ScrollToPoint)
            _orig_scroll_to_center(obj, start_offset, end_offset)
            # Also try ScrollSubstringTo which Qt actually handles,
            # triggering our scrollToSubstring → focus action → focus ring
            try:
                length = AXText.get_character_count(obj)
                if length:
                    if start_offset is None:
                        start_offset = 0
                    if end_offset is None:
                        end_offset = length - 1
                    AXText.scroll_substring_to_location(obj, 0, start_offset, end_offset)
            except Exception:
                pass

        AXEventSynthesizer.scroll_to_center = _patched_scroll_to_center
    except Exception as e:
        _log.warning("Failed to install scroll patch: %s", e)


_private_bus_doc_root = None


def _get_private_bus_doc_root():
    """Find the document root path on the private bus.

    The application root is at /org/a11y/atspi/accessible/root. Its first child is the document root. The path is
    non-deterministic (depends on object registration order), so we discover it dynamically and cache it."""

    global _private_bus_doc_root
    if _private_bus_doc_root is not None:
        return _private_bus_doc_root

    conn = _connect_private_bus()
    if conn is None or _private_bus_name is None:
        return None

    try:
        from gi.repository import Gio
        from gi.repository import GLib

        c = conn.call_sync(
            _private_bus_name,
            "/org/a11y/atspi/accessible/root",
            "org.a11y.atspi.Accessible",
            "GetChildren",
            None,
            GLib.VariantType.new("(a(so))"),
            Gio.DBusCallFlags.NONE,
            2000,
            None,
        )
        children = c.get_child_value(0)
        if children.n_children() > 0:
            _private_bus_doc_root = children.get_child_value(0).get_child_value(1).get_string()
            return _private_bus_doc_root
    except Exception:
        pass
    return None


def _private_bus_collection_get_matches(atspi_role_values):
    """Call Collection.GetMatches on the private bus to find objects matching any of the given AT-SPI2 roles.

    Uses the AT-SPI2 Collection interface which walks the entire tree server-side in a single D-Bus call, instead of
    making N round-trips per node. Returns a list of object paths, or an empty list on failure.

    The role bitfield encoding: each AT-SPI2 role value R maps to bit (R % 32) of int[R // 32] in an int array."""

    conn = _connect_private_bus()
    if conn is None or _private_bus_name is None:
        return []

    try:
        from gi.repository import Gio
        from gi.repository import GLib

        # Build the role bitfield. AT-SPI2 Collection expects an array of int32 where role R sets bit (R%32) of
        # element [R//32]. We need enough elements to cover the highest role value.
        max_role = max(atspi_role_values) if atspi_role_values else 0
        n_ints = (max_role // 32) + 1
        role_bits = [0] * n_ints
        for role_val in atspi_role_values:
            role_bits[role_val // 32] |= 1 << (role_val % 32)

        MATCH_ALL = 1
        MATCH_ANY = 2
        SORT_ORDER_CANONICAL = 1

        # MatchRule: (aiia{ss}iaiiasib)
        #   states: ai (empty), statematchtype: i (MATCH_ALL),
        #   attributes: a{ss} (empty), attributematchtype: i (MATCH_ALL),
        #   roles: ai (bitfield), rolematchtype: i (MATCH_ANY),
        #   interfaces: as (empty), interfacematchtype: i (MATCH_ALL),
        #   invert: b (False)
        # GetMatches(rule, sortby, count, traverse) -> a(so)
        args = GLib.Variant(
            "((aiia{ss}iaiiasib)uib)",
            (
                ([], MATCH_ALL, {}, MATCH_ALL, role_bits, MATCH_ANY, [], MATCH_ALL, False),
                SORT_ORDER_CANONICAL,
                0,  # count=0 means unlimited
                True,  # traverse
            ),
        )

        doc_path = _get_private_bus_doc_root()
        if not doc_path:
            return []
        result = conn.call_sync(
            _private_bus_name,
            doc_path,
            "org.a11y.atspi.Collection",
            "GetMatches",
            args,
            GLib.VariantType.new("(a(so))"),
            Gio.DBusCallFlags.NONE,
            5000,
            None,
        )

        matches = result.get_child_value(0)
        paths = []
        for i in range(matches.n_children()):
            match = matches.get_child_value(i)
            path = match.get_child_value(1).get_string()
            paths.append(path)
        return paths
    except Exception as e:
        _log.debug("Collection.GetMatches failed: %s", e)
        return []


def _private_bus_collection_get_matches_with_states(atspi_role_values, atspi_state_values):
    """Call Collection.GetMatches on the private bus, filtering by roles AND required states.

    Same as _private_bus_collection_get_matches but also encodes a state bitfield so that only objects with ALL of the
    specified states are returned."""

    conn = _connect_private_bus()
    if conn is None or _private_bus_name is None:
        return []

    try:
        from gi.repository import Gio
        from gi.repository import GLib

        # Build the role bitfield.
        max_role = max(atspi_role_values) if atspi_role_values else 0
        n_ints = (max_role // 32) + 1
        role_bits = [0] * n_ints
        for role_val in atspi_role_values:
            role_bits[role_val // 32] |= 1 << (role_val % 32)

        # Build the state bitfield.
        max_state = max(atspi_state_values) if atspi_state_values else 0
        n_state_ints = (max_state // 32) + 1
        state_bits = [0] * n_state_ints
        for state_val in atspi_state_values:
            state_bits[state_val // 32] |= 1 << (state_val % 32)

        MATCH_ALL = 1
        MATCH_ANY = 2
        SORT_ORDER_CANONICAL = 1

        args = GLib.Variant(
            "((aiia{ss}iaiiasib)uib)",
            (
                (state_bits, MATCH_ALL, {}, MATCH_ALL, role_bits, MATCH_ANY, [], MATCH_ALL, False),
                SORT_ORDER_CANONICAL,
                0,
                True,
            ),
        )

        doc_path = _get_private_bus_doc_root()
        if not doc_path:
            return []
        result = conn.call_sync(
            _private_bus_name,
            doc_path,
            "org.a11y.atspi.Collection",
            "GetMatches",
            args,
            GLib.VariantType.new("(a(so))"),
            Gio.DBusCallFlags.NONE,
            5000,
            None,
        )

        matches = result.get_child_value(0)
        paths = []
        for i in range(matches.n_children()):
            match = matches.get_child_value(i)
            path = match.get_child_value(1).get_string()
            paths.append(path)
        return paths
    except Exception as e:
        _log.debug("Collection.GetMatches (with states) failed: %s", e)
        return []


# Cached mapping from node-id to Qt-bridge Atspi.Accessible. Rebuilt when the Qt tree root changes (indicating a
# new accessibility tree arrived). This avoids walking the Qt tree on every structural navigation keypress.
_node_id_cache = {}  # node-id string -> Atspi.Accessible
_node_id_cache_root = None  # Qt root used to build the cache


def _rebuild_node_id_cache(qt_root):
    """Walk the Qt bridge tree and build a node-id -> Atspi.Accessible index."""

    global _node_id_cache, _node_id_cache_root
    _node_id_cache = {}
    _node_id_cache_root = qt_root

    def walk(obj, depth=0):
        if obj is None or depth > 50:
            return
        try:
            attrs = obj.get_attributes()
            if attrs:
                for key, val in attrs.items():
                    if key == "node-id":
                        _node_id_cache[val] = obj
                        break
        except Exception:
            pass
        for i in range(AXObject.get_child_count(obj)):
            try:
                walk(AXObject.get_child(obj, i), depth + 1)
            except Exception:
                pass

    walk(qt_root)


def _map_private_bus_to_qt_bridge(private_bus_paths, qt_root):
    """Map private bus object paths to Qt-bridge Atspi.Accessible objects via the shared node-id attribute.

    Uses a cached node-id index to avoid re-walking the Qt tree on every structural navigation keypress.
    The cache is rebuilt when the Qt tree root changes (new page load or tab switch)."""

    global _node_id_cache_root

    if not private_bus_paths:
        return []

    conn = _connect_private_bus()
    if conn is None or _private_bus_name is None:
        return []

    # Rebuild the node-id cache if the Qt root has changed.
    if qt_root is not _node_id_cache_root:
        _rebuild_node_id_cache(qt_root)

    try:
        from gi.repository import Gio
        from gi.repository import GLib

        results = []
        for path in private_bus_paths:
            try:
                a = conn.call_sync(
                    _private_bus_name,
                    path,
                    "org.a11y.atspi.Accessible",
                    "GetAttributes",
                    None,
                    GLib.VariantType.new("(a{ss})"),
                    Gio.DBusCallFlags.NONE,
                    2000,
                    None,
                )
                attrs = a.get_child_value(0).unpack()
                node_id = attrs.get("node-id")
                if node_id and node_id in _node_id_cache:
                    qt_obj = _node_id_cache[node_id]
                    if not AXObject.is_dead(qt_obj):
                        results.append(qt_obj)
                    else:
                        # Object became stale — rebuild cache and retry.
                        _rebuild_node_id_cache(qt_root)
                        qt_obj = _node_id_cache.get(node_id)
                        if qt_obj:
                            results.append(qt_obj)
            except Exception:
                pass
        return results
    except Exception:
        return []


def _install_collection_fallback_patch():
    """Wrap AXUtilitiesCollection.find_all_with_role and
    find_all_with_role_and_all_states so they fall back to a manual
    DFS tree search when Qt's Collection GetMatches returns empty.

    Forward-compatible: when Qt fixes Collection, the fallback path is
    never taken because Collection returns results first."""

    global _collection_fallback_installed
    if _collection_fallback_installed:
        return
    _collection_fallback_installed = True

    try:
        from orca.ax_utilities_collection import AXUtilitiesCollection

        _original_find_all_with_role = AXUtilitiesCollection.find_all_with_role

        @staticmethod
        def _patched_find_all_with_role(root, role_list, pred=None):
            try:
                results = _original_find_all_with_role(root, role_list, pred)
                if results:
                    return results
            except Exception:
                pass
            # Try Collection.GetMatches on the private bus — a single D-Bus call that walks the
            # tree server-side, instead of N round-trips per node.
            try:
                paths = _private_bus_collection_get_matches(role_list)
                if paths:
                    mapped = _map_private_bus_to_qt_bridge(paths, root)
                    if mapped:
                        return mapped
            except Exception:
                pass
            return _tree_search_by_role(root, role_list, pred)

        AXUtilitiesCollection.find_all_with_role = _patched_find_all_with_role

        _original_find_all_with_role_and_all_states = AXUtilitiesCollection.find_all_with_role_and_all_states

        @staticmethod
        def _patched_find_all_with_role_and_all_states(root, role_list, state_list, pred=None):
            try:
                results = _original_find_all_with_role_and_all_states(root, role_list, state_list, pred)
                if results:
                    return results
            except Exception:
                pass
            # Try Collection.GetMatches on the private bus with state filtering.
            try:
                paths = _private_bus_collection_get_matches_with_states(role_list, state_list)
                if paths:
                    mapped = _map_private_bus_to_qt_bridge(paths, root)
                    if mapped:
                        return mapped
            except Exception:
                pass
            return _tree_search_by_role_and_states(root, role_list, state_list, pred)

        AXUtilitiesCollection.find_all_with_role_and_all_states = _patched_find_all_with_role_and_all_states
    except Exception as e:
        _log.warning("Failed to install Collection fallback patch: %s", e)


class Utilities(web.Utilities):
    def __init__(self, script):
        super().__init__(script)
        self._cached_active_document = None
        self._cache_warmed = False
        self._private_bus = _connect_private_bus()
        _install_collection_fallback_patch()
        _install_sayall_patch()
        _install_scroll_patch()

    def clear_cached_objects(self):
        """Clear cached objects, including the node-id mapping and document root caches."""
        global _node_id_cache, _node_id_cache_root, _private_bus_doc_root
        super().clear_cached_objects()
        _node_id_cache = {}
        _node_id_cache_root = None
        _private_bus_doc_root = None

    def prewarm_cache(self):
        """Walk the document tree and query each object's key properties.

        If the private bus is available, fetch tree data from it (the ATK bridge answers from in-process data, so it's
        faster than the Qt bridge's D-Bus round-trips). Then walk the Qt bridge tree to populate Orca's Python-side
        caches. Called from activate() via GLib.idle_add so it runs in the background without blocking."""

        if self._cache_warmed:
            return

        doc = self.active_document()
        if doc is None:
            return

        self._cache_warmed = True

        # Try using the private bus for fast tree data prefetch.
        tree_data = self._fetch_private_bus_tree()
        if tree_data:
            _log.debug("Private bus prewarm: %d nodes", len(tree_data))

        # Standard warm-up: walk the Qt bridge tree to populate Orca's caches.
        def walk(obj, depth=0):
            if obj is None or depth > 30:
                return
            try:
                AXObject.get_role(obj)
                AXObject.get_name(obj)
                AXObject.get_child_count(obj)
                obj.get_state_set()
                obj.get_attributes()
            except Exception:
                pass
            for i in range(AXObject.get_child_count(obj)):
                try:
                    walk(AXObject.get_child(obj, i), depth + 1)
                except Exception:
                    pass

        walk(doc)

        # Pre-compute the first caret context so Say All doesn't need to call first_context() (which makes many D-Bus
        # calls).
        first_obj, first_offset = self.first_context(doc, 0)
        if first_obj is not None:
            self.set_caret_context(first_obj, first_offset, doc)

    def _fetch_private_bus_tree(self):
        """Fetch the accessibility tree from the private bus in a single pass.

        Returns a list of (path, role_name, name, attributes) tuples, or None if the private bus is unavailable."""

        conn = _connect_private_bus()
        if conn is None or _private_bus_name is None:
            return None

        try:
            from gi.repository import Gio
            from gi.repository import GLib

            results = []
            doc_root = _get_private_bus_doc_root()
            if not doc_root:
                return None

            def fetch(path, depth=0):
                if depth > 50:
                    return
                try:
                    # Fetch role.
                    r = conn.call_sync(
                        _private_bus_name,
                        path,
                        "org.a11y.atspi.Accessible",
                        "GetRoleName",
                        None,
                        GLib.VariantType.new("(s)"),
                        Gio.DBusCallFlags.NONE,
                        2000,
                        None,
                    )
                    role_name = r.get_child_value(0).get_string()

                    # Fetch name.
                    try:
                        n = conn.call_sync(
                            _private_bus_name,
                            path,
                            "org.a11y.atspi.Text",
                            "GetText",
                            GLib.Variant("(ii)", (0, 9999)),
                            GLib.VariantType.new("(s)"),
                            Gio.DBusCallFlags.NONE,
                            2000,
                            None,
                        )
                        name = n.get_child_value(0).get_string()
                    except Exception:
                        name = ""

                    # Fetch attributes.
                    a = conn.call_sync(
                        _private_bus_name,
                        path,
                        "org.a11y.atspi.Accessible",
                        "GetAttributes",
                        None,
                        GLib.VariantType.new("(a{ss})"),
                        Gio.DBusCallFlags.NONE,
                        2000,
                        None,
                    )
                    attrs = a.get_child_value(0).unpack()

                    results.append((path, role_name, name, attrs))

                    # Get children and recurse.
                    c = conn.call_sync(
                        _private_bus_name,
                        path,
                        "org.a11y.atspi.Accessible",
                        "GetChildren",
                        None,
                        GLib.VariantType.new("(a(so))"),
                        Gio.DBusCallFlags.NONE,
                        2000,
                        None,
                    )
                    children = c.get_child_value(0)
                    for i in range(children.n_children()):
                        child_path = children.get_child_value(i).get_child_value(1).get_string()
                        fetch(child_path, depth + 1)
                except Exception:
                    pass

            fetch(doc_root)
            return results if results else None
        except Exception:
            return None

    def active_document(self):
        """Returns the active document for the currently visible tab.

        Try super() first (uses EMBEDS relation, fast). Fall back to
        tree search if EMBEDS fails or returns a stale document."""

        # Try the standard EMBEDS-based approach first
        doc = super().active_document()
        if doc is not None:
            self._cached_active_document = doc
            return doc

        # Return cached document if still valid
        if self._cached_active_document is not None:
            if not AXObject.is_dead(self._cached_active_document) and AXUtilities.is_showing(
                self._cached_active_document
            ):
                return self._cached_active_document
            self._cached_active_document = None

        window = focus_manager.get_manager().get_active_window()
        if window is None:
            return None

        # Find ALL document_web nodes, prefer the one whose parent
        # panel is showing (active tab). Both documents report
        # showing=True, but the inactive tab's parent panel is hidden.
        documents = []

        def find_documents(obj, depth=0):
            if obj is None or depth > 15:
                return
            if AXUtilities.is_document_web(obj):
                documents.append(obj)
                return
            for i in range(AXObject.get_child_count(obj)):
                find_documents(AXObject.get_child(obj, i), depth + 1)

        find_documents(window)

        # Pick the document whose parent is showing (active tab)
        doc = None
        for d in documents:
            parent = AXObject.get_parent(d)
            if parent and AXUtilities.is_showing(parent):
                doc = d
                break
        # Fallback: if none matched, use the last one
        if doc is None and documents:
            doc = documents[-1]
        if doc is not None:
            self._cached_active_document = doc
        return doc

    def _get_text_for_obj(self, obj):
        """Get text content for an accessible object.

        Tries name first, then text interface, then recurses into
        children. This mirrors what our QAccessibleTextInterface's
        build_hypertext() does on the C++ side."""

        if obj is None:
            return ""

        # Text leaves and named elements: use the name
        name = AXObject.get_name(obj)
        if name:
            return name

        # Try text interface
        try:
            ti = obj.get_text_iface()
            if ti:
                cc = ti.get_character_count()
                if cc > 0:
                    return ti.get_text(0, cc)
        except Exception:
            pass

        # Container: collect text from children
        parts = []
        for i in range(AXObject.get_child_count(obj)):
            child = AXObject.get_child(obj, i)
            if child is not None:
                child_text = self._get_text_for_obj(child)
                if child_text:
                    parts.append(child_text)
        return " ".join(parts)

    def get_line_contents_at_offset(self, obj, offset, layout_mode=True, use_cache=True):
        """Build line contents with fewer D-Bus calls than the default.

        The default web script implementation scans left and right in
        layout mode, checking geometry for every neighboring object.
        This makes ~75-115 D-Bus round-trips per line. Our override
        gets the text directly from the accessible tree, avoiding the
        expensive geometry-based line scanning.

        Falls back to the default for non-document content."""

        _in_doc = self.in_document_content(obj) if obj else False
        if not _in_doc:
            _log.debug("get_line_contents NOT in doc, falling through")
            return super().get_line_contents_at_offset(obj, offset, layout_mode, use_cache)

        try:
            text = self._get_text_for_obj(obj)
            if text:
                _log.debug("get_line_contents fast: %r", text[:60])
                return [(obj, 0, len(text), text)]
            _log.debug("get_line_contents fast: empty text for obj")
        except Exception as _e:
            _log.debug("get_line_contents fast: exception %s", _e)

        # Fall back to default if our approach didn't produce anything
        _log.debug("get_line_contents falling through to super")
        return super().get_line_contents_at_offset(obj, offset, layout_mode, use_cache)

    def get_caret_context(self, document=None, get_replicant=False, search_if_needed=True):
        """Returns the current caret context.

        If there's no existing context in document content, find the
        document and return its first content element so Say All starts
        from web content instead of the window title."""

        obj, offset = super().get_caret_context(document, get_replicant, search_if_needed)

        if obj is not None:
            try:
                _r = AXObject.get_role(obj)
                _n = AXObject.get_name(obj) or ""
                _in = self.in_document_content(obj)
                _log.debug("get_caret_context super: role=%s name=%r in_doc=%s offset=%s", _r, _n[:40], _in, offset)
            except Exception:
                pass
        else:
            _log.debug("get_caret_context super returned None")

        if obj is not None and self.in_document_content(obj):
            # If the caret context is the document root itself (which has
            # no text), redirect to the first content child so Say All
            # reads web content instead of saying "Blank".
            if AXUtilities.is_document_web(obj):
                doc = obj
                first_obj, first_offset = self.first_context(doc, 0)
                if first_obj is not None:
                    self.set_caret_context(first_obj, first_offset, doc)
                    return first_obj, first_offset
            return obj, offset

        # Fall back: find the document and locate the first content element
        doc = self.active_document()
        if doc is not None:
            # Try first_context first (Orca's standard approach)
            first_obj, first_offset = self.first_context(doc, 0)
            # If first_context returned the document itself (meaning it
            # couldn't descend), manually find the first child with content
            if first_obj is None or first_obj == doc or AXUtilities.is_document_web(first_obj):
                first_obj = self._find_first_content_child(doc)
                first_offset = 0
            if first_obj is not None and not AXUtilities.is_document_web(first_obj):
                _log.debug(
                    "get_caret_context fallback: found role=%s name=%r",
                    AXObject.get_role(first_obj),
                    (AXObject.get_name(first_obj) or "")[:40],
                )
                self.set_caret_context(first_obj, first_offset, doc)
                return first_obj, first_offset
            _log.debug("get_caret_context fallback: using doc (no content child found)")
            self.set_caret_context(doc, 0, doc)
            return doc, 0
        else:
            _log.debug("get_caret_context fallback: no active_document found")

        return obj, offset

    def _find_first_content_child(self, obj, depth=0):
        """Find the first descendant that has text content."""
        if obj is None or depth > 15:
            return None
        for i in range(AXObject.get_child_count(obj)):
            child = AXObject.get_child(obj, i)
            if child is None:
                continue
            name = AXObject.get_name(child)
            if name:
                return child
            # Try text interface
            try:
                ti = child.get_text_iface()
                if ti and ti.get_character_count() > 0:
                    return child
            except Exception:
                pass
            # Recurse
            result = self._find_first_content_child(child, depth + 1)
            if result is not None:
                return result
        return None
