// The tab the popup opened, which closes itself when the test page asks.
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type !== "close-opened-tab") return;
    sendResponse(true);
    setTimeout(() => window.close());
});
