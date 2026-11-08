// The popup opens the side panel as the test asks: with a user activation, as a click on its button would give, or
// without one, and reports what the call did. Tests run in several windows at once, so the popup answers only the
// commands meant for its own window, the one whose button opened it.
const myWindow = browser.tabs.query({ active: true, currentWindow: true }).then(([tab]) => tab.windowId);
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type !== "to-popup" || !message.forwarded) return;
    (async () => {
        const windowId = await myWindow;
        if (message.windowId !== windowId) return;
        try {
            if (message.activation) internals.grantTransientActivation();
            const object = browser[message.namespace ?? "sidePanel"];
            const args = message.args ?? [];
            if (message.useCurrentWindow) args[0] = { ...(args[0] ?? {}), windowId };
            const value = await object[message.method ?? "open"](...args);
            sendResponse({ value: value === undefined ? null : value });
        } catch (error) {
            sendResponse({ rejected: error.message });
        }
        if (message.closeAfter) window.close();
    })();
    return true;
});
// The popup tells the page under it who it is, through that page's content script, so the test knows its window.
const instance = Math.random().toString(36).slice(2);
browser.tabs.query({ active: true, currentWindow: true }).then(([tab]) => {
    browser.tabs.sendMessage(tab.id, { type: "popup-hello", windowId: tab.windowId, tabId: tab.id, instance });
});
