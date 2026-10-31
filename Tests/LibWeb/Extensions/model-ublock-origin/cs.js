// In the test page, relays its requests to the background page. In every page it runs in, asks for the cosmetic
// filters, which the background page inserts into this frame alone.
if (location.pathname.endsWith("model-ublock-origin.html")) {
    document.addEventListener("tabs-request", async event => {
        const reply = await browser.runtime.sendMessage(JSON.parse(event.detail));
        document.documentElement.dataset.tabsReply = JSON.stringify(reply ?? null);
        document.dispatchEvent(new CustomEvent("tabs-reply"));
    });
}
browser.runtime.sendMessage({ type: "cosmetic" }).then(() => {
    document.documentElement.dataset.cosmeticApplied = "1";
    if (location.pathname.endsWith("model-ublock-origin.html")) {
        document.documentElement.dataset.contentScriptReady = "1";
        document.dispatchEvent(new CustomEvent("content-script-ready"));
    }
});
