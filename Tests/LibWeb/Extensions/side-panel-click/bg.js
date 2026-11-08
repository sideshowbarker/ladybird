// Calls the panel APIs as the test page asks, forwards commands to the popup and the panel page, and records what the
// panel page and the sidePanel events report. Tests run in several windows at once, so the lists hold every window's
// reports and events, and each test reads the ones for its own window.
const reports = [];
const events = [];
const methodNames = object =>
    Object.getOwnPropertyNames(object)
        .filter(key => typeof object[key] === "function")
        .sort();
const eventNames = object =>
    Object.getOwnPropertyNames(object)
        .filter(key => object[key] && typeof object[key].addListener === "function")
        .sort();

if (browser.sidePanel) {
    browser.sidePanel.onOpened.addListener(info => events.push({ event: "opened", ...info }));
    browser.sidePanel.onClosed.addListener(info => events.push({ event: "closed", ...info }));
}

async function callApi(message) {
    const object = browser[message.namespace];
    try {
        const value = await object[message.method](...(message.args ?? []));
        return { value: value === undefined ? null : value };
    } catch (error) {
        return { rejected: error.message };
    }
}

browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const reply = promise => {
        promise.then(sendResponse);
        return true;
    };
    if (message.type === "namespaces") {
        return void sendResponse({
            sidePanel: browser.sidePanel
                ? { methods: methodNames(browser.sidePanel), events: eventNames(browser.sidePanel) }
                : null,
            sidebarAction: browser.sidebarAction
                ? { methods: methodNames(browser.sidebarAction), events: eventNames(browser.sidebarAction) }
                : null,
        });
    }
    if (message.type === "call") return reply(callApi(message));
    if (message.type === "reports") return void sendResponse(reports);
    if (message.type === "events") return void sendResponse(events);
    if (message.type === "panel-report" || message.type === "popup-report") {
        reports.push(message);
        return;
    }
    // A page's runtime.sendMessage reaches every extension page, the popup and the panel among them, so a command
    // from the test page counts only once it has been forwarded from here; the page it's meant for answers it.
    if ((message.type === "to-popup" || message.type === "to-panel") && !message.forwarded)
        return reply(
            browser.runtime.sendMessage({ ...message, forwarded: true }).then(
                value => value ?? null,
                error => ({ failed: error.message })
            )
        );
});
