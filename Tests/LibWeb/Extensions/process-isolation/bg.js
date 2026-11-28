// Opens the extension's page in a tab, which opens the test's web page without an opener, then answers whether the web
// page runs in the extension page's process.
let pending = null;

browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "start") {
        pending = { respond: sendResponse };
        browser.tabs.create({ url: `page.html?target=${encodeURIComponent(message.target)}` });
        return true;
    }
    if (!pending) return;
    if (message.type === "extension") {
        pending.extensionPid = message.pid;
        pending.extensionTab = sender.tab.id;
    }
    if (message.type === "web") {
        pending.webPid = message.pid;
        pending.webTab = sender.tab.id;
    }
    if (pending.extensionPid === undefined || pending.webPid === undefined) return;
    const { respond, extensionPid, webPid, extensionTab, webTab } = pending;
    pending = null;
    browser.tabs
        .remove([extensionTab, webTab])
        .catch(() => {})
        .then(() => respond({ shared: extensionPid === webPid }));
});
