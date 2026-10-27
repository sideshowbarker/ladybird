// Relays each test page's requests to its own popup, and keeps what each popup reported, by the tab that opened it.
// test-web runs tests at once in tabs of their own, so nothing here is shared between them.
const reports = new Map();
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "popup-hello") {
        reports.set(message.openerTab, { senderTab: sender.tab?.id ?? null });
        return;
    }
    if (message.type === "set-popup") {
        reports.delete(sender.tab.id);
        browser.action.setPopup({ tabId: sender.tab.id, popup: message.popup }).then(() => sendResponse(true));
        return true;
    }
    if (message.type === "set-popup-report-pending") {
        reports.delete(sender.tab.id);
        sendResponse(true);
        return;
    }
    if (message.type === "popup-report") {
        const report = reports.get(sender.tab.id);
        sendResponse(report ? { ...report, openedFromThisTab: true } : null);
        return;
    }
    if (message.type === "to-popup") {
        browser.runtime
            .sendMessage({ type: "popup-command", command: message.command, openerTab: sender.tab.id })
            .catch(() => {});
        sendResponse(true);
    }
});
