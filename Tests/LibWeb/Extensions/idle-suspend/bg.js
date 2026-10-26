// Records in storage.local, as it unloads, how many messages it answered, so the next page to start can report it.
let count = 0;
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "count") sendResponse(++count);
    else if (message.type === "read") {
        browser.storage.local.get("suspendedAfter").then(items => sendResponse(items.suspendedAfter ?? null));
        return true;
    }
});
browser.runtime.onSuspend.addListener(() => browser.storage.local.set({ suspendedAfter: count }));
