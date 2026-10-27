// Registers, lists, and unregisters content scripts, and grants and removes the host they need, for the test page.
const target = "file:///*/data/registered-script-target.html";
const dark = {
    id: "dark",
    matches: [target],
    js: ["registered.js"],
    runAt: "document_start",
    allFrames: true,
    matchOriginAsFallback: true,
    world: "MAIN",
};
const other = {
    id: "other",
    matches: ["file:///*/nothing.html"],
    css: ["registered.css"],
    persistAcrossSessions: false,
};
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    const settle = promise => {
        promise.then(
            value => sendResponse({ value: value ?? null }),
            error => sendResponse({ failed: error.message })
        );
        return true;
    };
    const ids = scripts => scripts.map(script => `${script.id}:${script.persistAcrossSessions}`).join(",");
    if (message.type === "id") return sendResponse(browser.runtime.id);
    if (message.type === "granted") return settle(browser.permissions.contains({ origins: [target] }));
    if (message.type === "register") return settle(browser.scripting.registerContentScripts([dark, other]));
    if (message.type === "register-dark") return settle(browser.scripting.registerContentScripts([dark]));
    if (message.type === "list") return settle(browser.scripting.getRegisteredContentScripts(message.filter).then(ids));
    if (message.type === "update")
        return settle(browser.scripting.updateContentScripts([{ id: "other", allFrames: true }]));
    if (message.type === "unregister") return settle(browser.scripting.unregisterContentScripts(message.filter));
    if (message.type === "remove-origin") return settle(browser.permissions.remove({ origins: [target] }));
});
