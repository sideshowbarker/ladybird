// The panel page follows its window's active tab the way Companion's does: it reads the tab when it loads, and again
// on tabs.onActivated and when a tab finishes loading, reporting what it sees to the background page.
let activeTabId = null;
async function refresh(why) {
    const [tab] = await browser.tabs.query({ active: true, currentWindow: true });
    activeTabId = tab?.id ?? null;
    browser.runtime.sendMessage({ type: "panel-saw", why, url: tab?.url ?? null, windowId: tab?.windowId ?? null });
}
browser.tabs.onActivated.addListener(() => refresh("activated"));
browser.tabs.onUpdated.addListener((tabId, info) => {
    if (tabId === activeTabId && (info.url || info.status === "complete")) refresh("updated");
});
refresh("loaded");
