// Registers and unregisters content scripts for the test's frames, as the test page asks.
const target = "file:///*/data/content-scripts-register-target.html";
const registrations = {
    main: {
        matches: [`${target}*`],
        js: [{ file: "main.js" }],
        world: "MAIN",
        allFrames: true,
        runAt: "document_start",
    },
    isolated: {
        matches: [`${target}*`],
        excludeMatches: [`${target}?exclude-match`],
        includeGlobs: ["*target.html", "*target.html?exclude-*", "*target.html?unregister"],
        excludeGlobs: ["*?exclude-glob"],
        js: [{ code: "document.documentElement.dataset.registerIsolated = 'i';" }],
        allFrames: true,
        runAt: "document_start",
    },
    css: {
        matches: [`${target}*`],
        css: [{ code: "body { color: rgb(1, 2, 3); }" }],
        allFrames: true,
        runAt: "document_start",
    },
    // Pings the background page every 20ms for as long as the frame lives.
    messenger: {
        matches: [`${target}?messenger`],
        js: [{ code: "setInterval(() => browser.runtime.sendMessage({ type: 'ping' }), 20);" }],
        allFrames: true,
    },
    topOnly: { matches: [`${target}*`], js: [{ code: "document.documentElement.dataset.registerTopOnly = 't';" }] },
};
const handles = {};
let pings = 0;
let pageTab;
let pageRegistered;

browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const settle = call => {
        Promise.resolve()
            .then(call)
            .then(
                value => sendResponse({ value: value ?? null }),
                error => sendResponse({ failed: error.message })
            );
        return true;
    };
    if (message.type === "ping") {
        ++pings;
        return;
    }
    if (message.type === "pings") return sendResponse(pings);
    if (message.type === "page-registered") {
        pageRegistered?.();
        return;
    }
    if (message.type === "register")
        return settle(async () => {
            for (const name of message.names)
                handles[name] = await browser.contentScripts.register(registrations[name]);
            return typeof handles[message.names[0]].unregister;
        });
    if (message.type === "unregister") return settle(() => handles[message.name].unregister());
    // A call that fails its arguments' checks throws at once; one the extension may not make rejects.
    if (message.type === "invalid") {
        try {
            browser.contentScripts.register(message.options).then(
                () => sendResponse({ value: "registered" }),
                error => sendResponse({ rejected: error.message })
            );
        } catch (error) {
            sendResponse({ thrown: error.message });
        }
        return true;
    }
    if (message.type === "open-page")
        return settle(async () => {
            const registered = new Promise(resolve => (pageRegistered = resolve));
            pageTab = await browser.tabs.create({ url: "register-page.html", active: false });
            await registered;
        });
    if (message.type === "close-page") return settle(() => browser.tabs.remove(pageTab.id));
});
