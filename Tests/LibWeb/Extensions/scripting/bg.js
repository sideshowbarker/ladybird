// Injects scripts into the tab of the test page that asks, or into a tab it opens, and reports the results.
const settle = (promise, sendResponse) => {
    promise.then(
        value => sendResponse({ value }),
        error => sendResponse({ failed: error.message })
    );
    return true;
};
const runs = {
    add: [(a, b) => a + b + document.title, [1, 2]],
    pageGlobal: [() => typeof window.pageGlobal, []],
    later: [() => new Promise(resolve => setTimeout(() => resolve("waited"), 10)), []],
    boom: [
        () => {
            throw new Error("boom");
        },
        [],
    ],
    frameUrl: [() => location.pathname.split("/").pop() || location.href, []],
    count: [() => (window.injections = (window.injections ?? 0) + 1), []],
    cyclic: [
        () => {
            const object = {};
            object.self = object;
            return object;
        },
        [],
    ],
    bigint: [() => 1n, []],
    nothing: [() => undefined, []],
    protocol: [() => location.protocol, []],
    never: [() => new Promise(() => {}), []],
    message: [async () => await browser.runtime.sendMessage({ type: "echo" }), []],
};
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const target = { tabId: sender.tab.id, ...message.target };
    if (message.type === "func") {
        const [func, args] = runs[message.run];
        return settle(browser.scripting.executeScript({ target, func, args, world: message.world }), sendResponse);
    }
    if (message.type === "echo") return sendResponse(`pong from frame ${sender.frameId > 0}`);
    if (message.type === "bad-frame-ids") {
        try {
            browser.scripting.executeScript({ target: { tabId: sender.tab.id, frameIds: "0" }, func: () => 1 });
        } catch (error) {
            sendResponse({ failed: `${error.name}: ${error.message}` });
        }
        return;
    }
    if (message.type === "files")
        return settle(browser.scripting.executeScript({ target, files: ["inject.js"] }), sendResponse);
    if (message.type === "neither") return settle(browser.scripting.executeScript({ target }), sendResponse);
    if (message.type === "other-tab") {
        browser.tabs.create({ url: "data:text/html,unreachable", active: false }).then(async tab => {
            await new Promise(resolve => setTimeout(resolve, 100));
            settle(browser.scripting.executeScript({ target: { tabId: tab.id }, func: () => 1 }), sendResponse);
        });
        return true;
    }
});
