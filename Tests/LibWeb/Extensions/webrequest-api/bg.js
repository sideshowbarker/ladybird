// Probes the webRequest namespace for the test page: its enumerations, and what addListener() accepts and rejects.
const webRequest = browser.webRequest;
const noop = () => {};

function attempt(add) {
    try {
        add();
        return "ok";
    } catch (error) {
        return error.message;
    }
}

function probe() {
    const enums = {};
    for (const name of [
        "ResourceType",
        "OnBeforeRequestOptions",
        "OnBeforeSendHeadersOptions",
        "OnSendHeadersOptions",
        "OnHeadersReceivedOptions",
        "OnAuthRequiredOptions",
        "OnResponseStartedOptions",
        "OnBeforeRedirectOptions",
        "OnCompletedOptions",
    ])
        enums[name] = webRequest[name]
            ? Object.entries(webRequest[name]).map(([key, value]) => `${key}=${value}`)
            : null;

    const all = { urls: ["<all_urls>"] };
    const results = {
        enums,
        events: Object.getOwnPropertyNames(webRequest)
            .filter(key => key.startsWith("on"))
            .sort(),
        maxCalls: webRequest.MAX_HANDLER_BEHAVIOR_CHANGED_CALLS_PER_10_MINUTES,
        filterResponseData: typeof webRequest.filterResponseData,
        noFilter: attempt(() => webRequest.onBeforeRequest.addListener(noop)),
        noUrls: attempt(() => webRequest.onBeforeRequest.addListener(noop, {})),
        emptyUrls: attempt(() => webRequest.onBeforeRequest.addListener(noop, { urls: [] })),
        invalidPattern: attempt(() => webRequest.onBeforeRequest.addListener(noop, { urls: ["not a pattern"] })),
        bogusType: attempt(() => webRequest.onBeforeRequest.addListener(noop, { ...all, types: ["bogus"] })),
        bogusTypeAmongOthers: attempt(() =>
            webRequest.onBeforeRequest.addListener(noop, { ...all, types: ["bogus", "script"] })
        ),
        blocking: attempt(() => webRequest.onBeforeRequest.addListener(noop, all, ["blocking"])),
        bogusOption: attempt(() => webRequest.onBeforeRequest.addListener(noop, all, ["bogus"])),
        optionOfAnotherEvent: attempt(() => webRequest.onBeforeRequest.addListener(noop, all, ["requestHeaders"])),
        requestBody: attempt(() => webRequest.onBeforeRequest.addListener(noop, all, ["requestBody"])),
        extraHeaders: attempt(() =>
            webRequest.onSendHeaders.addListener(noop, all, ["requestHeaders", "extraHeaders"])
        ),
        ownPages: attempt(() =>
            webRequest.onBeforeRequest.addListener(noop, {
                urls: [browser.runtime.getURL("/") + "web_accessible_resources/*"],
            })
        ),
        tabAndWindow: attempt(() => webRequest.onCompleted.addListener(noop, { ...all, tabId: 1, windowId: 1 })),
    };

    const listener = () => {};
    webRequest.onErrorOccurred.addListener(listener, all);
    const before = webRequest.onErrorOccurred.hasListener(listener);
    webRequest.onErrorOccurred.removeListener(listener);
    results.hasListener = `${before} ${webRequest.onErrorOccurred.hasListener(listener)}`;
    return results;
}

browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "probe") {
        const results = probe();
        const changed = webRequest.handlerBehaviorChanged();
        results.handlerBehaviorChangedIsPromise = changed instanceof Promise;
        changed.then(value => sendResponse({ ...results, handlerBehaviorChanged: String(value) }));
        return true;
    }
});
