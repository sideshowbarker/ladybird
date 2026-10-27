// Uses storage.sync for the test page, and records the area each of its changes came from.
const areas = [];
browser.storage.onChanged.addListener((changes, areaName) => {
    if ("token" in changes) areas.push(areaName);
});
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const settle = promise => {
        promise.then(
            value => sendResponse({ value: value ?? null }),
            error => sendResponse({ failed: error.message })
        );
        return true;
    };
    const sync = browser.storage.sync;
    if (message.type === "set") return settle(sync.set(message.items));
    if (message.type === "get") return settle(sync.get(message.keys));
    if (message.type === "remove") return settle(sync.remove(message.keys));
    if (message.type === "local-get") return settle(browser.storage.local.get(message.keys));
    if (message.type === "areas") sendResponse(areas.splice(0));
    if (message.type === "limits")
        sendResponse([
            sync.QUOTA_BYTES,
            sync.QUOTA_BYTES_PER_ITEM,
            sync.MAX_ITEMS,
            sync.MAX_WRITE_OPERATIONS_PER_HOUR,
            sync.MAX_WRITE_OPERATIONS_PER_MINUTE,
        ]);
});
