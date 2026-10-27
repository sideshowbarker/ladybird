// Injects code, a file, and CSS into the tab of the test page that asks, with the manifest version 2 calls.
let frameId;
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const settle = promise => {
        promise.then(
            value => sendResponse({ value: value ?? null }),
            error => sendResponse({ failed: error.message })
        );
        return true;
    };
    if (message.type === "code") return settle(browser.tabs.executeScript(sender.tab.id, { code: "document.title" }));
    if (message.type === "file") return settle(browser.tabs.executeScript(sender.tab.id, { file: "inject.js" }));
    if (message.type === "frame-ready") {
        frameId = sender.frameId;
        return;
    }
    if (message.type === "frame-id") return sendResponse(frameId ?? null);
    if (message.type === "subtree")
        return settle(
            browser.tabs.executeScript(sender.tab.id, {
                code: "location.protocol",
                frameId,
                allFrames: true,
                matchAboutBlank: true,
            })
        );
    if (message.type === "no-blank")
        return settle(browser.tabs.executeScript(sender.tab.id, { code: "location.protocol", allFrames: true }));
    if (message.type === "top-throws")
        return settle(
            browser.tabs.executeScript(sender.tab.id, {
                code: "if (window === top) throw new Error('top boom'); 1",
                allFrames: true,
            })
        );
    if (message.type === "css")
        return settle(browser.tabs.insertCSS(sender.tab.id, { code: "#box { color: rgb(7, 8, 9); }" }));
});
