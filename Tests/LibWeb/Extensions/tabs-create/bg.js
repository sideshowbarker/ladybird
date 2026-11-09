// Opens, changes, and finds tabs for the test pages, and keeps what the popup reports for the page that opened it.
const describe = tab => ({ id: tab.id, windowId: tab.windowId, url: tab.url ?? null, active: tab.active });
const popupResults = new Map();
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    // A call can also throw before it returns a promise, when its arguments are malformed.
    const settle = call => {
        new Promise(resolve => resolve(call())).then(
            tab => sendResponse(describe(tab)),
            error => sendResponse({ failed: error.message })
        );
        return true;
    };
    if (message.type === "create") return settle(() => browser.tabs.create(message.properties));
    if (message.type === "update") return settle(() => browser.tabs.update(message.tabId, message.properties));
    if (message.type === "find") {
        browser.tabs.query({}).then(tabs => {
            const tab = tabs.find(tab => tab.id === message.id);
            sendResponse(tab ? { url: tab.url ?? null, status: tab.status } : null);
        });
        return true;
    }
    if (message.type === "my-tab") sendResponse(sender.tab.id);
    else if (message.type === "popup-updated") popupResults.set(message.openerTab, message.tab);
    else if (message.type === "popup-result") sendResponse(popupResults.get(sender.tab.id) ?? null);
});
