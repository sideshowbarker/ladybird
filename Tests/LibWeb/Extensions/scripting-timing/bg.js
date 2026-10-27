// When the page starts loading, injects into it by default (once it's idle) and with injectImmediately.
const timing = {};
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "start") {
        const target = { tabId: sender.tab.id };
        const func = () => document.readyState;
        browser.scripting.executeScript({ target, func }).then(results => (timing.default = results[0].result));
        browser.scripting
            .executeScript({ target, func, injectImmediately: true })
            .then(results => (timing.immediately = results[0].result));
        return;
    }
    if (message.type === "timing") sendResponse("default" in timing && "immediately" in timing ? timing : null);
});
