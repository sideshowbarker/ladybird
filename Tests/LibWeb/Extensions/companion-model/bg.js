// Badges the test page's tab once it has loaded, keeps what the popup, the panel, and the alarm report in
// storage.session, and tells the test page what it has. Tests run at once in tabs of their own, and a test runs again
// in the same tab, so a popup's record is keyed by the tab it opened over, the panel's by its window, and a test
// clears its own before it starts.
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
    if (message.type === "my-tab-id") return void sendResponse(sender.tab?.id ?? null);
    if (message.type === "status") {
        browser.tabs.query({}).then(tabs => sendResponse(tabs.find(tab => tab.id === message.id)?.status ?? null));
        return true;
    }
    if (message.type === "keep-open" && !message.forwarded) {
        // The page's message reached every extension page already; only the copy forwarded from here counts.
        browser.runtime.sendMessage({ type: "keep-open", forwarded: true, tabId: sender.tab.id }).then(
            value => sendResponse(value ?? null),
            error => sendResponse({ failed: error.message })
        );
        return true;
    }
    if (message.type === "panel-saw") {
        const key = `panel:${message.windowId}`;
        browser.storage.session
            .get({ [key]: [] })
            .then(session =>
                browser.storage.session.set({ [key]: [...session[key], { why: message.why, url: message.url }] })
            );
        return;
    }
    if (message.type === "popup-opened") {
        browser.storage.session.set({
            [`popup:${message.tabId}`]: { activeUrl: message.activeUrl, created: message.created },
        });
    } else if (message.type === "reset") {
        browser.storage.session
            .remove([`popup:${sender.tab.id}`, `panel:${sender.tab.windowId}`])
            .then(() => sendResponse(true));
        return true;
    } else if (message.type === "state") {
        Promise.all([browser.action.getBadgeText({ tabId: sender.tab.id }), browser.storage.session.get()]).then(
            ([badge, session]) =>
                sendResponse({
                    badge,
                    session,
                    popup: session[`popup:${sender.tab.id}`] ?? null,
                    panel: session[`panel:${sender.tab.windowId}`] ?? [],
                })
        );
        return true;
    }
});
