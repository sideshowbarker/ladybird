// Sets, reads, and clears alarms for the test page, and records the alarms it has heard since it last started.
const heard = [];
browser.alarms.onAlarm.addListener(alarm => heard.push(alarm));
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const settle = promise => {
        promise.then(
            value => sendResponse({ value: value ?? null }),
            error => sendResponse({ failed: error.message })
        );
        return true;
    };
    if (message.type === "create") return settle(browser.alarms.create(message.name, message.info));
    if (message.type === "get") return settle(browser.alarms.get(message.name));
    if (message.type === "getAll") return settle(browser.alarms.getAll());
    if (message.type === "clear") return settle(browser.alarms.clear(message.name));
    if (message.type === "clearAll") return settle(browser.alarms.clearAll());
    if (message.type === "heard") sendResponse(heard);
    // The callback forms, with the unnamed alarm, and whether a missing alarm is undefined rather than null.
    if (message.type === "callbacks") {
        browser.alarms
            .create({ delayInMinutes: 10 })
            .then(() =>
                browser.alarms.get(alarm =>
                    browser.alarms
                        .get("missing")
                        .then(missing =>
                            browser.alarms.clear(cleared =>
                                sendResponse({ name: alarm.name, cleared, missingIsUndefined: missing === undefined })
                            )
                        )
                )
            );
        return true;
    }
});
