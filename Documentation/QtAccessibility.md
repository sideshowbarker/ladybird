# Qt accessibility support

Ladybird's Qt UI exposes the accessibility tree to screen readers via Qt's `QAccessibleInterface` abstraction. On Linux, Qt bridges to AT-SPI2 (used by Orca). On macOS, Qt bridges to NSAccessibility (used by VoiceOver). The same `QAccessibleInterface` code serves both platforms. On macOS, runtime method swizzling extends Qt's bridge with web content capabilities it lacks natively.

## Architecture

```
WebContent process                    UI process (Qt)
-------------------                   ----------------
DOM + ARIA tree
    |
serialize_tree_as_node_data()
    |
    +--- IPC: did_get_accessibility_tree --->  AccessibilityTreeManager
                                              (C++, in LibWebView, shared
                                              with AppKit implementation)
                                                   |
                                              AccessibilityInterface
                                              (QAccessibleInterface subclass)
                                                   |
                                              Qt platform bridge
                                              (QMacAccessibilityElement on macOS,
                                               AT-SPI2 adapter on Linux)
                                                   |
                                              [macOS only: runtime swizzling adds
                                               AXWebArea, landmark subroles,
                                               search predicates, role descriptions]
                                                   |
                                              Orca / VoiceOver
```

The platform-agnostic parts (`AccessibilityNodeData`, `AccessibilityTreeManager`, IPC endpoints, WebContent serialization) are shared with the AppKit implementation. Only the presentation layer differs.

## Key files

`UI/Qt/AccessibilityInterface.h` / `AccessibilityInterface.cpp`
- `AccessibilityInterface`: wraps one `AccessibilityNodeData` node, implementing `QAccessibleInterface`, `QAccessibleActionInterface`, `QAccessibleTextInterface`, and `QAccessibleTableCellInterface`. Sets `_qt_mac_subrole` and `_qt_mac_roleDescription` dynamic properties on its backing `QObject` for landmark roles.
- `WebContentViewAccessible`: a `QAccessibleWidget` subclass for `WebContentView` (role `Grouping`, not `Pane` which Qt's bridge ignores) that returns the document root `AccessibilityInterface` as its child.
- `accessibility_factory()`: registered via `QAccessible::installFactory()`, tells Qt to use `WebContentViewAccessible` for `WebContentView` widgets.

`UI/Qt/WebContentView.h` / `WebContentView.cpp`
- Owns the `AccessibilityTreeManager` and element cache (`QHash<i64, AccessibilityInterface*>`)
- Sets up IPC callbacks: `on_load_finish`, `on_url_change`, `on_title_change`, `on_accessibility_tree_received`, `on_accessibility_focus_changed`
- `accessibility_interface_for_node(node_id)`: lazily creates and caches `AccessibilityInterface` instances

`UI/Qt/WebContentViewAccessibilityMac.h` / `WebContentViewAccessibilityMac.mm` (macOS only)
- Runtime swizzling of `QMacAccessibilityElement` for AXWebArea role, landmark subroles, role descriptions, search predicates, and focused element
- `install_native_accessibility()`: installs the swizzles (called once from the constructor)
- `notify_accessibility_tree_loaded()`: makes the NSView the Cocoa first responder, posts `AXLoadComplete` on the document root's `QMacAccessibilityElement`, and calls `NSAccessibilityHandleFocusChanged()` to direct VoiceOver to the web content
- `post_accessibility_focus_changed()`: ensures the `QMacAccessibilityElement` exists for a node, then posts `NSAccessibilityFocusedUIElementChangedNotification` natively (bypassing Qt's event system which causes "Invalid child" errors)


## Critical design constraints discovered through testing

### Element cache must never be cleared

When a new accessibility tree arrives via IPC, the tree manager is updated but the element cache (`m_accessibility_elements`) must NOT be cleared. Existing `AccessibilityInterface` objects must stay alive because:

1. Qt's macOS bridge (`QMacAccessibilityElement`) holds references to our interfaces. If the interfaces are deleted or orphaned, the bridge's references dangle and VoiceOver's navigation breaks.
2. Each `AccessibilityInterface` queries the `AccessibilityTreeManager` dynamically (via `node_data()`), so it automatically reflects updated tree data without being recreated.
3. Clearing the cache causes VoiceOver to lose its navigation position and snap back to the beginning.

After each tree update, interfaces for nodes that no longer exist in the new tree are pruned: `isValid()` is checked on each cached interface, and stale ones are deregistered via `QAccessible::deleteAccessibleInterface()` and removed from the cache. This prevents unbounded memory growth across page navigations while keeping live interfaces intact.

### Qt accessibility events cannot be used; native notifications must be posted directly

Posting `QAccessibleEvent` objects via `QAccessible::updateAccessibility()` causes "Invalid child in QAccessibleEvent" errors from Qt's cocoa bridge, because the bridge tries to look up the `QMacAccessibilityElement` by `QAccessible::Id` but the element may not have been created yet (they are created lazily by VoiceOver's queries).

The workaround: `post_accessibility_focus_changed()` in `WebContentViewAccessibilityMac.mm` ensures the `QMacAccessibilityElement` exists by calling `+[QMacAccessibilityElement elementWithId:]` before posting `NSAccessibilityFocusedUIElementChangedNotification` directly via `NSAccessibilityPostNotification`. This bypasses Qt's event system entirely.

Focus tracking and live region announcements are both enabled via this approach — bypassing Qt’s event system and posting native `NSAccessibilityPostNotification` directly. Live region announcements use `NSAccessibilityAnnouncementRequestedNotification` on `[NSApp mainWindow]` with priority based on assertive/polite.

### Each interface needs a backing QObject

Qt's macOS bridge requires `QAccessibleInterface::object()` to return a non-null `QObject*`. Without it, the bridge can't track elements for VoiceOver navigation. Each `AccessibilityInterface` creates a dummy `QObject` parented to the `WebContentView` widget. This mirrors what QtWebEngine's `BrowserAccessibilityInterface` does.

### WebContentViewAccessible must use Grouping role, not Pane

Qt's cocoa bridge marks `QAccessible::Pane` as ignored in its `shouldBeIgnored()` function. If the `WebContentViewAccessible` uses `Pane`, VoiceOver can't focus it, and the web content is unreachable. `Grouping` is not ignored by the bridge and allows VoiceOver to discover and focus the web content.

### Runtime swizzling of Qt's cocoa bridge (macOS only)

Qt's cocoa bridge has several limitations for web content accessibility:
- Maps `QAccessible::WebDocument` to `NSAccessibilityGroupRole` (not `AXWebArea`)
- `macSubrole()` only handles search fields, password fields, tabs, and switches — no landmark subroles
- `NSAccessibilityRoleDescription()` returns "group" for all landmark subroles
- Does not implement `AXUIElementsForSearchPredicate` (required for VoiceOver's depth-first web content navigation)

`WebContentViewAccessibilityMac.mm` swizzles seven methods on `QMacAccessibilityElement` at runtime:

1. **`accessibilityRole`**: Returns `@"AXWebArea"` for `WebDocument` role, and `NSAccessibilityGroupRole` for `ListItem` role (Qt maps `ListItem` to `StaticText`, which prevents VoiceOver from counting list items and descending into lists).

2. **`accessibilitySubRole`**: Checks the element's `QObject` for a `_qt_mac_subrole` dynamic property. Returns that string for landmarks (e.g., `@"AXLandmarkNavigation"`).

3. **`accessibilityRoleDescription`**: Checks for a `_qt_mac_roleDescription` dynamic property. Returns that string for landmarks (e.g., `"navigation"`).

4. **`accessibilityParameterizedAttributeNames`**: Adds `AXUIElementsForSearchPredicate` and `AXUIElementCountForSearchPredicate` to every element.

5. **`accessibilityAttributeValue:forParameter:`**: Handles search predicate queries with pre-order DFS traversal, leaf-role skipping, and container-descendant skipping — matching the AppKit implementation's behavior.

6. **`accessibilityFocusedUIElement`**: For AXWebArea elements (and their parent containers), returns the first leaf-like child in DFS order so VoiceOver focuses the first web content element.

7. **`accessibilityIsIgnored`**: Returns YES for `ListItem` role. Qt’s bridge does not ignore listitems, so VoiceOver stops on both the `<li>` container and the link/button inside it, causing double-reading and focus ring lag. Making listitems ignored causes macOS to promote their children to the parent level, so VoiceOver navigates directly to the content inside.

The swizzles are installed once via `install_cocoa_swizzles()`, called from `install_native_accessibility()` in the `WebContentView` constructor.

### ListItem must be ignored via `accessibilityIsIgnored` swizzle

Qt’s `shouldBeIgnored()` function does not include `ListItem` in its ignore list. Without the swizzle, VoiceOver visits both the `<li>` container (role: Group) and the link/button inside it, causing double-reading and focus ring confusion — the focus ring appears on the wrong element because VoiceOver announces the listitem, then immediately auto-descends to the child.

The fix swizzles `accessibilityIsIgnored` on `QMacAccessibilityElement` to return YES for `ListItem` role. macOS’s `NSAccessibilityUnignoredChildren` then promotes the listitem’s children to the parent (List) level. This also applies to the search predicate, where `ListItem` is a container-only role — descended into but not added as a navigation stop.

### Initial VoiceOver focus

After the accessibility tree is received, `notify_accessibility_tree_loaded()` (called after a 100ms delay via `QTimer::singleShot`) does three things:

1. Makes the web content view's NSView the Cocoa first responder via `[window makeFirstResponder:nsView]` — VoiceOver follows the first responder chain for initial focus.
2. Posts `AXLoadComplete` on the document root's `QMacAccessibilityElement` — tells VoiceOver that web content has loaded.
3. Calls `NSAccessibilityHandleFocusChanged()` — tells VoiceOver to re-query the focused element.

This causes VoiceOver to focus the first element in the web content (e.g., "Skip to main content"), matching Safari's behavior.

### Tree requests are debounced

Multiple navigation callbacks (`on_url_change`, `on_title_change`) fire in quick succession during page load. Each triggers a tree request via the debounced 500ms `QTimer`. `on_load_finish` requests the tree directly (not debounced) as a fast path. For pages where `on_load_finish` doesn't fire (e.g., SPA navigations), the debounced timer serves as a fallback.

## Differences from the AppKit implementation

| Aspect | AppKit | Qt |
| ------ | ------ | -- |
| Base class | `NSObject` with informal NSAccessibility protocol | `QAccessibleInterface` subclass |
| Navigation | `AXUIElementsForSearchPredicate` implemented directly | Same, via runtime swizzling of `QMacAccessibilityElement` on macOS |
| Document root role | `AXWebArea` (custom string, not a public constant) | `QAccessible::WebDocument`, swizzled to `@"AXWebArea"` on macOS |
| Container widget role | `NSAccessibilityScrollAreaRole` | `QAccessible::Grouping` (NOT `Pane`, which Qt's bridge ignores) |
| Element cache lifecycle | Cleared on every tree update, interfaces recreated | Never cleared; interfaces persist and re-query updated tree manager |
| Accessibility events | `NSAccessibilityPostNotification` for layout changes, focus, announcements | Focus events and live region announcements posted via native `NSAccessibilityPostNotification` (bypassing Qt’s event system) |
| Ignored element handling | `accessibilityIsIgnored` + `collectUnignoredChildren` | `collect_unignored_children` + `find_unignored_parent` (same logic, different API) |
| Coordinate conversion | `convertRect:toView:nil` + `convertRectToScreen:` | `QWidget::mapToGlobal()` |
| Text leaf content | AXTitle=nil, AXValue=text (VoiceOver reads Value for StaticText) | Name=text, Value=empty (Qt bridge reads Name for navigation) |
| Actions | `accessibilityPerformAction:` sends IPC | `QAccessibleActionInterface::doAction()` sends IPC |

## Known limitations

- **Element cache grows monotonically.** Interfaces are never deleted because Qt's bridge holds references. This is a memory leak for long-lived tabs.
- **Not tested on Linux with Orca.** The `QAccessibleInterface` code should work on Linux since Orca uses AT-SPI2 tree traversal. The runtime swizzling is macOS-only; on Linux, Qt's bridge handles roles natively (though without `AXWebArea` or landmark subroles, which are macOS concepts).
