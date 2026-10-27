// Relays the page's requests to the background page, and holds a port to it open while the page asks it to.
let port = null;
document.addEventListener("tabs-request", async event => {
    const message = JSON.parse(event.detail);
    let reply;
    if (message.type === "open-port") {
        port = browser.runtime.connect();
        reply = await new Promise(resolve => {
            port.onMessage.addListener(resolve);
            port.postMessage("hello");
        });
    } else if (message.type === "close-port") {
        port.disconnect();
        port = null;
        reply = "closed";
    } else {
        try {
            reply = await browser.runtime.sendMessage(message);
        } catch (error) {
            reply = { failed: `${error.name}: ${error.message}` };
        }
    }
    document.documentElement.dataset.tabsReply = JSON.stringify(reply);
    document.dispatchEvent(new CustomEvent("tabs-reply"));
});
document.documentElement.dataset.contentScriptReady = "1";
document.dispatchEvent(new CustomEvent("content-script-ready"));
