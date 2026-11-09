// A tab a test opened, keyed by the token in its URL: It retitles or closes itself when that test asks.
const token = new URLSearchParams(location.search).get("token");
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.token !== token) return;
    if (message.type === "retitle") {
        document.title = message.title;
        sendResponse(true);
    } else if (message.type === "close") {
        sendResponse(true);
        setTimeout(() => window.close());
    }
});
