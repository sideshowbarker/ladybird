// The content script relays each request from the page to the background page, and reports the reply into the DOM,
// along with which panel namespaces this extension has, for the page that asks about an extension with only the
// sidebar_action key.
if (!location.pathname.endsWith("side-panel-namespaces.html"))
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
browser.runtime.onMessage.addListener(message => {
    if (message.type === "popup-hello") document.documentElement.dataset.popupHello = JSON.stringify(message);
});
browser.runtime.sendMessage({ type: "namespaces" }).then(namespaces => {
    document.documentElement.dataset.sidebarActionOnlyNamespaces = JSON.stringify({
        sidePanel: namespaces.sidePanel === null ? "undefined" : "object",
        sidebarAction: namespaces.sidebarAction?.methods ?? null,
    });
});
document.documentElement.dataset.contentScriptReady = "1";
document.dispatchEvent(new CustomEvent("content-script-ready"));
