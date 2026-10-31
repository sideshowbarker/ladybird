// Reports, as the test page asks, what management.getSelf() resolves with or passes its callback, and the manifest.
const fields = info => {
    const { id, name, version, enabled, installType, type, mayDisable, permissions, hostPermissions } = info;
    return {
        ownId: id === browser.runtime.id,
        name,
        version,
        enabled,
        installType,
        type,
        mayDisable,
        permissions,
        hostPermissions,
    };
};
browser.runtime.onMessage.addListener((message, sender, sendResponse) => {
    if (message.type === "promise") {
        browser.management.getSelf().then(info => sendResponse(fields(info)));
        return true;
    }
    if (message.type === "callback") {
        chrome.management.getSelf(info => sendResponse(fields(info)));
        return true;
    }
    if (message.type === "others") return sendResponse(typeof browser.management.getAll);
    if (message.type === "manifest") {
        const manifest = browser.runtime.getManifest();
        return sendResponse({ settings: manifest.browser_specific_settings, applications: manifest.applications });
    }
});
