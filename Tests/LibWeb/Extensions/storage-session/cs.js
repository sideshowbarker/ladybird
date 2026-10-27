// Relays the page's requests to the background page, or tries storage.session itself for a "direct-get" one.
document.addEventListener("tabs-request", async event => {
    const message = JSON.parse(event.detail);
    let reply;
    try {
        if (message.type === "direct-get") reply = { value: await browser.storage.session.get(message.keys) };
        else reply = await browser.runtime.sendMessage(message);
    } catch (error) {
        reply = { failed: error.message };
    }
    document.documentElement.dataset.tabsReply = JSON.stringify(reply);
    document.dispatchEvent(new CustomEvent("tabs-reply"));
});
document.documentElement.dataset.contentScriptReady = "1";
document.dispatchEvent(new CustomEvent("content-script-ready"));
