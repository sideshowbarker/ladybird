// Inserts and removes CSS in the tab of the test page that asks, and in the frame of that tab that reported in.
const frameIds = new Map();
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const tabId = sender.tab.id;
    const settle = call => {
        Promise.resolve()
            .then(call)
            .then(
                value => sendResponse({ value: value ?? null }),
                error => sendResponse({ failed: error.message })
            );
        return true;
    };
    if (message.type === "frame-ready") {
        frameIds.set(tabId, sender.frameId);
        return;
    }
    if (message.type === "frame-id") return sendResponse(frameIds.get(tabId) ?? null);
    if (message.type === "css-call")
        return settle(() =>
            browser.tabs[message.method](tabId, {
                ...message.details,
                frameId: message.inFrame ? frameIds.get(tabId) : undefined,
            })
        );
    if (message.type === "frame-color")
        return settle(() =>
            browser.tabs.executeScript(tabId, {
                code: "getComputedStyle(document.body).color",
                frameId: frameIds.get(tabId),
            })
        );
});
