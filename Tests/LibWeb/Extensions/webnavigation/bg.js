// Records the webNavigation events of each URL that carries a token a test page is watching, by token; and, apart, what
// a listener filtered to 127.0.0.1 hears.
const recorded = new Map();
const filtered = [];

for (const event of [
    "onBeforeNavigate",
    "onCommitted",
    "onDOMContentLoaded",
    "onCompleted",
    "onErrorOccurred",
    "onCreatedNavigationTarget",
    "onHistoryStateUpdated",
    "onReferenceFragmentUpdated",
]) {
    browser.webNavigation[event].addListener(details => {
        for (const [token, events] of recorded) {
            if (details.url.includes(token)) events.push({ event, ...details });
        }
    });
}
browser.webNavigation.onCommitted.addListener(details => filtered.push(details.url), {
    url: [{ hostEquals: "127.0.0.1" }],
});

function attempt(add) {
    try {
        add();
        return "ok";
    } catch (error) {
        return error.message;
    }
}

browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "watch") {
        recorded.set(message.token, []);
        sendResponse({ tabId: sender.tab.id });
    } else if (message.type === "events") {
        sendResponse(recorded.get(message.token));
    } else if (message.type === "filtered") {
        sendResponse(filtered.filter(url => url.includes(message.token)));
    } else if (message.type === "allFrames") {
        browser.webNavigation.getAllFrames({ tabId: message.tabId }).then(sendResponse);
        return true;
    } else if (message.type === "frame") {
        browser.webNavigation.getFrame({ tabId: message.tabId, frameId: message.frameId }).then(sendResponse);
        return true;
    } else if (message.type === "invalidFilters") {
        const noop = () => {};
        sendResponse({
            noUrl: attempt(() => browser.webNavigation.onCompleted.addListener(noop, {})),
            emptyUrl: attempt(() => browser.webNavigation.onCompleted.addListener(noop, { url: [] })),
            badKey: attempt(() => browser.webNavigation.onCompleted.addListener(noop, { url: [{ hostname: "x" }] })),
            badType: attempt(() => browser.webNavigation.onCompleted.addListener(noop, { url: [{ hostEquals: 1 }] })),
        });
    } else if (message.type === "closeTab") {
        browser.tabs.remove(message.tabId).then(
            () => sendResponse(true),
            () => sendResponse(false)
        );
        return true;
    }
});
