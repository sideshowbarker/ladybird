// The content script relays each request from the page to the background page, and reports the reply into the DOM.
document.addEventListener("tabs-request", async event => {
    let reply;
    try {
        reply = await browser.runtime.sendMessage(JSON.parse(event.detail));
    } catch (error) {
        reply = { failed: `${error.name}: ${error.message}` };
    }
    document.documentElement.dataset.tabsReply = JSON.stringify(reply);
    document.dispatchEvent(new CustomEvent("tabs-reply"));
});
document.documentElement.dataset.tabsInContentScript = typeof browser.tabs;
document.documentElement.dataset.contentScriptReady = "1";
document.dispatchEvent(new CustomEvent("content-script-ready"));
