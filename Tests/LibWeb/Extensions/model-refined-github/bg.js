// Starts up as Refined GitHub's background does by default: options with defaults in storage.sync, an action context
// menu, listeners for granted permissions and the toolbar button, and a content script registered for its host. Every
// error or unhandled rejection is recorded, from before the first call.
const errors = [];
self.addEventListener("error", event => errors.push(`error: ${event.message}`));
self.addEventListener("unhandledrejection", event =>
    errors.push(`rejection: ${event.reason?.message ?? event.reason}`)
);

const defaults = { accent: "rgb(10, 20, 30)", features: ["clean-header"] };
const started = (async () => {
    // Menu items outlast a restart of the background page, so they're created once, at install.
    browser.runtime.onInstalled.addListener(async () => {
        browser.contextMenus.create(
            { id: "permission-toggle", title: "Enable on this domain", contexts: ["action"] },
            () => {
                if (browser.runtime.lastError) errors.push(`menus: ${browser.runtime.lastError.message}`);
            }
        );
        const stored = await browser.storage.sync.get(defaults);
        await browser.storage.sync.set({ ...defaults, ...stored });
    });
    browser.contextMenus.onClicked.addListener(() => {});
    browser.permissions.onAdded.addListener(() => {});
    browser.permissions.onRemoved.addListener(() => {});
    browser.action.onClicked.addListener(() => browser.runtime.openOptionsPage());
    await browser.storage.sync.get(defaults);
    const registered = await browser.scripting.getRegisteredContentScripts({ ids: ["target"] });
    if (registered.length === 0)
        await browser.scripting.registerContentScripts([
            {
                id: "target",
                matches: ["file:///*/data/model-refined-github-target.html"],
                js: ["target.js"],
                runAt: "document_start",
                // The test page loads the target in a frame.
                allFrames: true,
                persistAcrossSessions: false,
            },
        ]);
    await browser.permissions.contains({ origins: ["https://github.com/*"] });
})();

browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "started") {
        started.then(
            () => sendResponse(true),
            error => sendResponse(`failed: ${error.message}`)
        );
        return true;
    }
    if (message.type === "errors") return sendResponse(errors);
    if (message.type === "set-accent") {
        browser.storage.sync.set({ accent: message.accent }).then(() => sendResponse(true));
        return true;
    }
});
