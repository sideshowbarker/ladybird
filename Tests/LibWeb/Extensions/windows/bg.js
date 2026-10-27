// Answers the test page with what the windows API reports about its window.
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const settle = promise => {
        promise.then(
            value => sendResponse({ value: value ?? null }),
            error => sendResponse({ failed: error.message })
        );
        return true;
    };
    const mine = windows => windows.find(window => window.id === sender.tab.windowId);
    if (message.type === "constants")
        return sendResponse([browser.windows.WINDOW_ID_NONE, browser.windows.WINDOW_ID_CURRENT]);
    if (message.type === "events")
        return sendResponse(
            ["onCreated", "onRemoved", "onFocusChanged"].map(name => typeof browser.windows[name].addListener)
        );
    if (message.type === "all") return settle(browser.windows.getAll().then(mine));
    if (message.type === "populated")
        return settle(
            browser.windows
                .getAll({ populate: true })
                .then(windows => mine(windows).tabs.some(tab => tab.id === sender.tab.id))
        );
    if (message.type === "get") return settle(browser.windows.get(message.id ?? sender.tab.windowId));
    if (message.type === "get-current")
        return settle(
            Promise.all([browser.windows.get(browser.windows.WINDOW_ID_CURRENT), browser.windows.getCurrent()]).then(
                ([named, current]) => named.id === current.id
            )
        );
    if (message.type === "current-and-last")
        return settle(
            Promise.all([browser.windows.getCurrent(), browser.windows.getLastFocused()]).then(
                ([current, last]) => current.id === last.id
            )
        );
});
