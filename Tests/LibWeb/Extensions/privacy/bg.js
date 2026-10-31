// Calls a privacy setting's methods as the test page asks, and records what each setting's onChange reports.
const changes = [];
const setting = path => path.split(".").reduce((object, key) => object[key], browser.privacy);
for (const path of ["network.networkPredictionEnabled"])
    setting(path).onChange.addListener(details => changes.push({ path, ...details }));

browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "changes") return sendResponse(changes.splice(0));
    if (message.type !== "call") return;
    try {
        setting(message.path)
            [message.method](message.details)
            .then(
                value => sendResponse({ value }),
                error => sendResponse({ rejected: error.message })
            );
    } catch (error) {
        sendResponse({ thrown: error.message });
    }
    return true;
});
