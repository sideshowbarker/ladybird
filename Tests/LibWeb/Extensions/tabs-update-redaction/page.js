// The extension's page in the tab, which closes itself when the test that opened it asks.
const token = new URLSearchParams(location.search).get("token");
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type !== "close" || message.token !== token) return;
    sendResponse(true);
    setTimeout(() => window.close());
});
