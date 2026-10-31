// The content script relays each request from the page to the background page, or for "cs-fetch", fetches the URL
// itself, and reports the result into the DOM.
document.addEventListener("tabs-request", async event => {
    const message = JSON.parse(event.detail);
    let reply;
    if (message.type === "cs-fetch") {
        try {
            reply = { status: (await fetch(message.url)).status };
        } catch (error) {
            reply = { failed: error.name };
        }
    } else {
        reply = await browser.runtime.sendMessage(message);
    }
    document.documentElement.dataset.tabsReply = JSON.stringify(reply);
    document.dispatchEvent(new CustomEvent("tabs-reply"));
});
document.documentElement.dataset.contentScriptReady = "1";
document.dispatchEvent(new CustomEvent("content-script-ready"));
