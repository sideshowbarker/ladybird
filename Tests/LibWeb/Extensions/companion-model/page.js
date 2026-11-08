// The tab the popup opened, which closes itself when the test page asks for it by id; copies of the test run at once,
// each with a tab of its own, and the others stay silent. The background page tells the tab its id.
const me = browser.runtime.sendMessage({ type: "my-tab-id" });
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type !== "close-opened-tab") return;
    me.then(id => {
        if (message.id !== id) return;
        sendResponse(true);
        setTimeout(() => window.close());
    });
    return true;
});
