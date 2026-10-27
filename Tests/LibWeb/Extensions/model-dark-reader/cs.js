// Darkens the page from its first frame, then asks the background for this frame's theme; in the top frame it also
// relays the test page's requests.
document.documentElement.style.setProperty("background-color", "rgb(24, 26, 27)");
browser.runtime.onMessage.addListener(message => {
    if (message.type !== "theme") return;
    document.documentElement.style.setProperty("color", message.theme.text);
    document.documentElement.dataset.themedFrame = String(message.frameId);
});
browser.runtime.sendMessage({ type: "frame-ready" });
if (window === top) {
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
