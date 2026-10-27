// Calls the action API on the page's behalf, and answers with what it resolved or how it failed. A "$tab" argument
// stands for the id of the tab that asked.
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const api = browser.action ?? browser.browserAction;
    if (message.type === "namespaces") {
        sendResponse({
            action: typeof browser.action,
            browserAction: typeof browser.browserAction,
            setBadgeTextColor: typeof api.setBadgeTextColor,
        });
        return;
    }
    const args = JSON.parse(JSON.stringify(message.args).replaceAll('"$tab"', String(sender.tab.id)));
    Promise.resolve()
        .then(() => api[message.method](...args))
        .then(
            value => sendResponse({ value: value ?? null }),
            error => sendResponse({ error: `${error.name}: ${error.message}` })
        );
    return true;
});
