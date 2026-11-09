// Records every tab event, and opens and finds tabs for the test pages, which read the events of their own tabs.
const events = [];
browser.tabs.onActivated.addListener(info =>
    events.push({ tabId: info.tabId, type: "activated", hasWindow: typeof info.windowId === "number" })
);
browser.tabs.onUpdated.addListener((tabId, changeInfo, tab) =>
    events.push({ tabId, type: "updated", changeInfo, tabUrl: tab.url ?? null })
);
browser.tabs.onRemoved.addListener((tabId, removeInfo) => events.push({ tabId, type: "removed", removeInfo }));
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "open") {
        browser.tabs.create({ url: `page.html?token=${message.token}` }).then(tab => sendResponse(tab.id));
        return true;
    }
    if (message.type === "events") sendResponse(events.filter(event => event.tabId === message.id));
});
