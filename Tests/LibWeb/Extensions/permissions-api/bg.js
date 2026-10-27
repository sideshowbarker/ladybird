// Reads, checks, and gives up its permissions for the test page, and records each change it hears of.
const changes = [];
browser.permissions.onAdded.addListener(added => changes.push({ added }));
browser.permissions.onRemoved.addListener(removed => changes.push({ removed }));
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const settle = call => {
        new Promise(resolve => resolve(call())).then(
            value => sendResponse({ value }),
            error => sendResponse({ failed: `${error.name}: ${error.message}` })
        );
        return true;
    };
    if (message.type === "getAll") return settle(() => browser.permissions.getAll());
    if (message.type === "contains") return settle(() => browser.permissions.contains(message.permissions));
    if (message.type === "request") return settle(() => browser.permissions.request(message.permissions));
    if (message.type === "remove") return settle(() => browser.permissions.remove(message.permissions));
    if (message.type === "changes") sendResponse(changes.splice(0));
});
