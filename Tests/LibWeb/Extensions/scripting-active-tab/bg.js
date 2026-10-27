// Injects into the asking page's tab, with whatever access activeTab gives at the time.
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "id") return sendResponse(browser.runtime.id);
    const target = { tabId: sender.tab.id, ...message.target };
    browser.scripting.executeScript({ target, func: () => location.protocol }).then(
        value => sendResponse({ value: value.map(frame => frame.result) }),
        error => sendResponse({ failed: error.message })
    );
    return true;
});
