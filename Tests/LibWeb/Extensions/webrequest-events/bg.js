// Records the webRequest events of each request whose URL carries a token a test page is watching, by token, and runs
// requests of its own for the test pages.
const recorded = new Map();
const heardForOtherTab = [];

function record(event, details) {
    for (const [token, events] of recorded) {
        if (details.url.includes(token)) events.push({ event, ...details });
    }
}

const options = {
    onBeforeSendHeaders: ["requestHeaders"],
    onSendHeaders: ["requestHeaders"],
    onHeadersReceived: ["responseHeaders"],
};
for (const event of [
    "onBeforeRequest",
    "onBeforeSendHeaders",
    "onSendHeaders",
    "onHeadersReceived",
    "onBeforeRedirect",
    "onResponseStarted",
    "onCompleted",
    "onErrorOccurred",
])
    browser.webRequest[event].addListener(details => record(event, details), { urls: ["<all_urls>"] }, options[event]);

// No tab has this id, so this listener hears nothing.
browser.webRequest.onBeforeRequest.addListener(details => heardForOtherTab.push(details.url), {
    urls: ["<all_urls>"],
    tabId: 999999,
});

browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "watch") {
        recorded.set(message.token, []);
        sendResponse({ tabId: sender.tab.id });
    } else if (message.type === "events") {
        sendResponse(recorded.get(message.token));
    } else if (message.type === "heardForOtherTab") {
        sendResponse(heardForOtherTab.filter(url => url.includes(message.token)).length);
    } else if (message.type === "openTab") {
        browser.tabs.create({ url: message.url, active: false }).then(tab => sendResponse({ tabId: tab.id }));
        return true;
    } else if (message.type === "closeTab") {
        browser.tabs.remove(message.tabId).then(() => sendResponse(true));
        return true;
    } else if (message.type === "fetch") {
        fetch(message.url, { mode: "no-cors" }).then(
            () => sendResponse(true),
            () => sendResponse(false)
        );
        return true;
    }
});
