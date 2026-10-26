// In the top frame, relays the page's requests to the background page. In a subframe, registers with the background page
// so it can be messaged by its frame id. In both, answers the background page's tab messages with where it runs.
const where = window === top ? "top" : "subframe";
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "who")
        sendResponse(`${where}, from the background: ${sender.url.endsWith("_generated_background_page.html")}`);
});

if (where === "subframe") {
    browser.runtime.sendMessage({ type: "register" });
} else {
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
    document.documentElement.dataset.contentScriptReady = "1";
    document.dispatchEvent(new CustomEvent("content-script-ready"));
}
