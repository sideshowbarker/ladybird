// Starts up as Dark Reader's persistent background does by default: an uninstall URL, its context menu rebuilt with
// removeAll() then create(), its shortcuts read, the platform asked, and settings in storage.sync and storage.local.
// It answers each frame's content script by that frame's id, and injects into tabs that are already open. Every error
// or unhandled rejection is recorded, from before the first call.
const errors = [];
self.addEventListener("error", event => errors.push(`error: ${event.message}`));
self.addEventListener("unhandledrejection", event =>
    errors.push(`rejection: ${event.reason?.message ?? event.reason}`)
);

const theme = { background: "rgb(24, 26, 27)", text: "rgb(232, 230, 227)" };
const started = (async () => {
    browser.runtime.setUninstallURL("https://example.com/goodbye");
    browser.contextMenus.removeAll(() => {
        browser.contextMenus.create({ id: "top", title: "Dark Reader" }, () => {
            if (browser.runtime.lastError) errors.push(`menus: ${browser.runtime.lastError.message}`);
        });
        browser.contextMenus.create({ id: "toggle", parentId: "top", title: "Toggle everywhere" });
    });
    browser.contextMenus.onClicked.addListener(() => {});
    browser.commands.onCommand.addListener(() => {});
    const commands = await new Promise(resolve => browser.commands.getAll(resolve));
    if (commands.length !== 1) errors.push(`commands: ${JSON.stringify(commands)}`);
    await browser.runtime.getPlatformInfo();
    await browser.storage.sync.get({ enabled: true });
    await browser.storage.local.get({ siteList: [] });
    browser.tabs.onRemoved.addListener(() => {});
})();

const frames = [];
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "frame-ready") {
        frames.push(sender.frameId);
        // Each frame's theme reaches it alone, by its frame id.
        browser.tabs.sendMessage(
            sender.tab.id,
            { type: "theme", theme, frameId: sender.frameId },
            { frameId: sender.frameId }
        );
        return;
    }
    if (message.type === "started") {
        started.then(
            () => sendResponse(true),
            error => sendResponse(`failed: ${error.message}`)
        );
        return true;
    }
    if (message.type === "frames") return sendResponse(frames.length);
    if (message.type === "errors") return sendResponse(errors);
    if (message.type === "inject-open-tabs") {
        browser.tabs.query({}).then(async tabs => {
            const tab = tabs.find(tab => tab.id === sender.tab.id);
            const results = await browser.tabs.executeScript(tab.id, {
                file: "inject.js",
                allFrames: true,
                matchAboutBlank: true,
            });
            sendResponse(results);
        });
        return true;
    }
});
