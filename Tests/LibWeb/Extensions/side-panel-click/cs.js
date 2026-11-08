// The content script relays each request from the page to the background page, and reports the reply into the DOM,
// along with which panel namespaces a content script has (none).
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
    if (message.type === "panel-hello") document.documentElement.dataset.panelHello = JSON.stringify(message);
});
document.documentElement.dataset.contentScriptNamespaces = JSON.stringify({
    sidePanel: typeof browser.sidePanel,
    sidebarAction: typeof browser.sidebarAction,
});
document.documentElement.dataset.contentScriptReady = "1";
document.dispatchEvent(new CustomEvent("content-script-ready"));
