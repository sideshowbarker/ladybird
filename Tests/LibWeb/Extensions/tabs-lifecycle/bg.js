// Opens tabs for the test pages and gets, reloads, and removes them, recording what their events report.
const describe = tab => ({ id: tab.id, windowId: tab.windowId, url: tab.url ?? null, active: tab.active });
const statuses = new Map();
const removed = [];
browser.tabs.onUpdated.addListener((tabId, changeInfo) => {
    if (changeInfo.status) statuses.set(tabId, [...(statuses.get(tabId) ?? []), changeInfo.status]);
});
browser.tabs.onRemoved.addListener(tabId => removed.push(tabId));
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const settle = promise => {
        promise.then(
            value => sendResponse({ value: value ?? null }),
            error => sendResponse({ failed: error.message })
        );
        return true;
    };
    if (message.type === "my-tab") return sendResponse(sender.tab.id);
    if (message.type === "create")
        return settle(browser.tabs.create({ url: message.url, active: false }).then(describe));
    if (message.type === "get") return settle(browser.tabs.get(message.id).then(describe));
    if (message.type === "query")
        return settle(browser.tabs.query({}).then(tabs => tabs.filter(tab => tab.id === message.id).map(describe)));
    if (message.type === "reload") {
        statuses.delete(message.id);
        return settle(browser.tabs.reload(message.id));
    }
    if (message.type === "statuses") return sendResponse(statuses.get(message.id) ?? []);
    if (message.type === "remove") return settle(browser.tabs.remove(message.ids));
    if (message.type === "removed") return sendResponse(removed);
});
