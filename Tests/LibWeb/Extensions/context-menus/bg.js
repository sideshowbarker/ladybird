// Creates, updates, and removes menu items for the test page, reporting each call's return value and error.
const call = run =>
    new Promise(resolve => {
        let returned;
        try {
            returned = run(() => resolve({ returned, error: browser.runtime.lastError?.message ?? null }));
        } catch (error) {
            resolve({ thrown: error.message });
        }
    });
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const reply = promise => {
        promise.then(
            value => sendResponse({ value: value ?? null }),
            error => sendResponse({ failed: error.message })
        );
        return true;
    };
    const menus = browser.contextMenus;
    if (message.type === "create") return reply(call(done => menus.create(message.properties, done)));
    if (message.type === "update") return reply(menus.update(message.id, message.properties));
    if (message.type === "remove") return reply(menus.remove(message.id));
    if (message.type === "removeAll") return reply(menus.removeAll());
    if (message.type === "shape")
        return sendResponse([
            typeof menus.onClicked.addListener,
            menus.ACTION_MENU_TOP_LEVEL_LIMIT,
            browser.menus === menus,
        ]);
});
