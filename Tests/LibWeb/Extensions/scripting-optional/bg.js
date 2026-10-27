// Injects into the asking page's tab with whatever it has been granted, and removes grants when asked.
const scripting = browser.scripting;
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const reply = promise => {
        promise.then(
            value => sendResponse({ value }),
            error => sendResponse({ failed: error.message })
        );
        return true;
    };
    if (message.type === "has") return reply(browser.permissions.contains({ permissions: message.permissions }));
    if (message.type === "namespace") return sendResponse(typeof scripting);
    if (message.type === "remove") return reply(browser.permissions.remove({ permissions: message.permissions }));
    if (message.type === "run")
        return reply(scripting.executeScript({ target: { tabId: sender.tab.id }, func: () => location.protocol }));
});
