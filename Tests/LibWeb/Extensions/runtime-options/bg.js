// Opens the options page and makes the small runtime and extension calls, reporting what they resolve with.
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const reply = promise => {
        promise.then(
            value => sendResponse({ value: value ?? null }),
            error => sendResponse({ failed: error.message })
        );
        return true;
    };
    const options = browser.runtime.getURL("options.html");
    if (message.type === "open") return reply(browser.runtime.openOptionsPage());
    if (message.type === "options-tabs")
        return reply(
            browser.tabs.query({}).then(tabs => tabs.filter(tab => tab.url === options).map(tab => tab.active))
        );
    if (message.type === "uninstall-url") return reply(browser.runtime.setUninstallURL(message.url));
    if (message.type === "platform")
        return reply(browser.runtime.getPlatformInfo().then(info => [typeof info.os, typeof info.arch]));
    if (message.type === "file-access") return reply(browser.extension.isAllowedFileSchemeAccess());
});
