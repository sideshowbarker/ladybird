// Reads the tab the popup opened over, and opens a tab of the extension's in the background, as Companion's links do.
// Its "Keep this open beside the page" button opens the side panel for its window and closes the popup, as
// Companion's does; the test presses it through the background page, with the activation a click would give.
const active = browser.tabs.query({ active: true, currentWindow: true }).then(([tab]) => tab);
active.then(async tab => {
    // The tab is what the companion-model page checks; over any other page the popup leaves it out, since copies
    // of the side panel test run at once and each tab is a process.
    const opensTab = tab.url?.endsWith("/companion-model.html");
    const created = opensTab ? await browser.tabs.create({ url: "page.html", active: false }) : null;
    browser.runtime.sendMessage({
        type: "popup-opened",
        activeUrl: tab.url,
        tabId: tab.id,
        created: created?.id ?? null,
    });
});
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type !== "keep-open" || !message.forwarded) return;
    active.then(async tab => {
        // Copies of the test run at once; only the popup over the asking page answers, the others stay silent.
        if (message.tabId !== tab.id) return;
        internals.grantTransientActivation();
        const side = browser.sidePanel;
        const bar = browser.sidebarAction;
        try {
            await (side?.open && tab.windowId !== undefined ? side.open({ windowId: tab.windowId }) : bar.open());
            sendResponse({ opened: true });
            window.close();
        } catch (error) {
            sendResponse({ rejected: error.message });
        }
    });
    return true;
});
