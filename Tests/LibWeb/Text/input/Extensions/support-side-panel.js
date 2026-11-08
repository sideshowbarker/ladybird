// Shared by the side panel tests: the fixture's id, its panel's state, the popup that opens it, and cleanup. The
// tests run in several windows at once and share the fixture's background page, so each reads only its own window's
// reports and events, and sends its commands to its own window's popup and panel.
const sidePanelFixture = () => internals.extensionId(window.sidePanelFixtureName ?? "Side Panel Fixture");

function panelState() {
    return JSON.parse(internals.extensionSidePanelState(sidePanelFixture()));
}

async function waitFor(predicate) {
    for (let i = 0; i < 400; ++i) {
        const value = await predicate();
        if (value) return value;
        await new Promise(resolve => setTimeout(resolve, 25));
    }
    return null;
}

const callApi = (namespace, method, ...args) => sendToBackground({ type: "call", namespace, method, args });
const popupOpen = () => JSON.parse(internals.extensionPopupState(sidePanelFixture())).open;

// The test's own tab and window, learned from the popup before anything else.
let myTabInfo = null;
let myWindowId = null;

// The background page keeps every window's reports and events; these read this window's, not read yet, in order.
let seenReports = 0;
let seenEvents = 0;
async function nextReport(what) {
    return waitFor(async () => {
        const all = await sendToBackground({ type: "reports" });
        for (let i = seenReports; i < all.length; ++i) {
            if (all[i].what !== what || all[i].windowId !== myWindowId) continue;
            seenReports = i + 1;
            return all[i];
        }
        return null;
    });
}
async function newEvents() {
    const all = await sendToBackground({ type: "events" });
    const fresh = all.slice(seenEvents).filter(event => event.windowId === myWindowId);
    seenEvents = all.length;
    return fresh;
}
// Waits until this window has at least this many events not read yet, without reading them.
async function eventsAtLeast(count) {
    return waitFor(
        async () =>
            (await sendToBackground({ type: "events" }))
                .slice(seenEvents)
                .filter(event => event.windowId === myWindowId).length >= count
    );
}

// Opens the fixture's popup in this window, which can then open the panel with the activation a click would give.
// The popup announces itself to this page through the content script, naming the window and tab it opened over.
let lastPopupInstance = null;
async function openPopup() {
    if (popupOpen()) return;
    internals.clickExtensionAction(sidePanelFixture());
    const hello = await waitFor(() => {
        const hello = document.documentElement.dataset.popupHello;
        if (!hello) return null;
        const parsed = JSON.parse(hello);
        return parsed.instance !== lastPopupInstance ? parsed : null;
    });
    lastPopupInstance = hello.instance;
    myWindowId = hello.windowId;
    myTabInfo = { id: hello.tabId, windowId: hello.windowId };
}

// Has this window's popup call a panel method with user activation, by default sidePanel.open() for its window. A
// popup asked to close afterwards is gone before this returns, so the next call opens a fresh one.
async function fromPopup(message) {
    await openPopup();
    const reply = await sendToBackground({
        type: "to-popup",
        windowId: myWindowId,
        activation: true,
        useCurrentWindow: true,
        ...message,
    });
    if (message.closeAfter) await waitFor(() => !popupOpen());
    return reply;
}

const toPanel = command => sendToBackground({ type: "to-panel", windowId: myWindowId, command });

// Opens the panel from the popup and returns the panel page's first report, with the call's result on it.
async function openPanel() {
    const result = await fromPopup({ closeAfter: true });
    await waitFor(() => panelState().open);
    const hello = await nextReport("hello");
    return { ...hello, result };
}

// Learns this window and tab from a fresh popup, and starts reading reports and events from here on.
async function startSidePanelTest() {
    await contentScriptReady();
    await closePanel();
    seenReports = (await sendToBackground({ type: "reports" })).length;
    seenEvents = (await sendToBackground({ type: "events" })).length;
    await openPopup();
    return myTabInfo;
}

async function closePanel() {
    if (panelState().open) {
        internals.closeExtensionSidePanel();
        await waitFor(() => !panelState().open);
    }
    if (popupOpen()) {
        internals.clickExtensionAction(sidePanelFixture());
        await waitFor(() => !popupOpen());
    }
}
