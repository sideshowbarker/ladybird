// The panel page reports the tab it sees as active in its window when it loads and whenever that changes, with an
// instance id that tells one document from the next, and follows the commands meant for it. Tests run in several
// windows at once, so every report names the panel's window, and the page answers only its window's commands.
const instance = Math.random().toString(36).slice(2);
const myWindow = browser.tabs.query({ active: true, currentWindow: true }).then(([tab]) => tab.windowId);
async function report(what) {
    const tabs = await browser.tabs.query({ active: true, currentWindow: true });
    browser.runtime.sendMessage({
        type: "panel-report",
        what,
        instance,
        windowId: await myWindow,
        path: location.pathname + location.search,
        activeTab: tabs.length === 1 ? tabs[0].id : null,
    });
}
browser.tabs.onActivated.addListener(() => report("activated"));
browser.tabs.onUpdated.addListener((tabId, info) => {
    if (info.status === "complete") report("updated");
});
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type !== "to-panel" || !message.forwarded) return;
    (async () => {
        if (message.windowId !== (await myWindow)) return;
        if (message.command === "close") {
            sendResponse({ closing: true });
            window.close();
            return;
        }
        if (message.command === "report") {
            await report("asked");
            sendResponse({ instance });
        }
    })();
    return true;
});
report("hello");
// The panel tells the page under it who it is, through that page's content script, so the test knows its window
// without a popup, which a click doesn't open once openPanelOnActionClick is set.
browser.tabs.query({ active: true, currentWindow: true }).then(([tab]) => {
    browser.tabs.sendMessage(tab.id, { type: "panel-hello", windowId: tab.windowId, tabId: tab.id, instance });
});
