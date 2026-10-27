// Badges the test page's tab once it has loaded, keeps what the popup and the alarm report in storage.session, and
// tells the test page what it has.
browser.tabs.onUpdated.addListener((tabId, changeInfo, tab) => {
    if (changeInfo.status === "complete" && tab.url?.endsWith("/companion-model.html"))
        browser.action.setBadgeText({ tabId, text: "3" });
});
browser.alarms.onAlarm.addListener(alarm => browser.storage.session.set({ refreshed: alarm.name }));
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "arm") {
        browser.alarms.create("refresh", { delayInMinutes: 1 / 60 }).then(() => sendResponse(true));
        return true;
    }
    if (message.type === "status") {
        browser.tabs.query({}).then(tabs => sendResponse(tabs.find(tab => tab.id === message.id)?.status ?? null));
        return true;
    }
    if (message.type === "popup-opened") {
        browser.storage.session.set({ popup: { activeUrl: message.activeUrl, created: message.created } });
    } else if (message.type === "state") {
        Promise.all([browser.action.getBadgeText({ tabId: sender.tab.id }), browser.storage.session.get()]).then(
            ([badge, session]) => sendResponse({ badge, session })
        );
        return true;
    }
});
