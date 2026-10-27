// A tab a test opened, keyed by the token in its URL: It closes itself, or changes its own tab, when that test asks.
const token = new URLSearchParams(location.search).get("token");
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.token !== token) return;
    if (message.type === "close") {
        sendResponse(true);
        setTimeout(() => window.close());
    } else if (message.type === "update-self") {
        browser.tabs.update(message.properties).then(
            tab => sendResponse({ id: tab.id, url: tab.url ?? null }),
            error => sendResponse({ failed: error.message })
        );
        return true;
    }
});
