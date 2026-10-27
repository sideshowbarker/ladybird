// Reports into the DOM whether this extension's background page has webRequest, and whether this content script does.
browser.runtime.sendMessage("has webRequest?").then(answer => {
    document.documentElement.dataset.noPermissionWebRequest = `background: ${answer}, content script: ${typeof browser.webRequest}`;
    document.dispatchEvent(new CustomEvent("no-permission-ready"));
});
