// Relays each request from the test page to the background page, and reports the reply into the DOM.
document.addEventListener("tabs-request", async event => {
    const reply = await browser.runtime.sendMessage(JSON.parse(event.detail));
    document.documentElement.dataset.tabsReply = JSON.stringify(reply ?? null);
    document.dispatchEvent(new CustomEvent("tabs-reply"));
});
document.documentElement.dataset.contentScriptReady = "1";
document.dispatchEvent(new CustomEvent("content-script-ready"));
