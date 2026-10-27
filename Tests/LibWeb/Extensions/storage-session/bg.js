// Uses storage.session for the test page, records the area each of its changes came from, and sets who may use it.
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
    if (message.type === "set") return settle(browser.storage.session.set(message.items));
    if (message.type === "get") return settle(browser.storage.session.get(message.keys));
    if (message.type === "local-get") return settle(browser.storage.local.get(message.keys));
    if (message.type === "access")
        return settle(browser.storage.session.setAccessLevel({ accessLevel: message.level }));
    if (message.type === "clear") return settle(browser.storage.session.clear());
    if (message.type === "areas") sendResponse(areas.splice(0));
});
