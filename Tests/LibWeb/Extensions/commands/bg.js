// Reports the extension's commands, and Firefox's menus alias, to the test page.
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const reply = promise => {
        promise.then(
            value => sendResponse({ value: value ?? null }),
            error => sendResponse({ failed: error.message })
        );
        return true;
    };
    if (message.type === "open-options") return reply(browser.runtime.openOptionsPage());
    if (message.type === "getAll") return reply(browser.commands.getAll());
    if (message.type === "update") return reply(browser.commands.update(message.details));
    if (message.type === "alias")
        return sendResponse([browser.menus === browser.contextMenus, typeof browser.commands.onCommand.addListener]);
});
