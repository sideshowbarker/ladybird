// Records the runtime.onInstalled and runtime.onStartup events it hears in storage.session, which outlasts an idle
// unload of the page, for the test page.
async function recorded(type, value) {
    const { events = [] } = await browser.storage.session.get("events");
    events.push({ type, value });
    await browser.storage.session.set({ events });
}
browser.runtime.onInstalled.addListener(details => recorded("installed", details));
browser.runtime.onStartup.addListener(() => recorded("startup", true));
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type !== "events") return;
    browser.storage.session.get("events").then(({ events = [] }) => sendResponse(events));
    return true;
});
