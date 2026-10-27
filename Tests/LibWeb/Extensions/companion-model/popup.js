// Reads the tab the popup opened over, and opens a tab of the extension's in the background, as Companion's links do.
browser.tabs.query({ active: true, currentWindow: true }).then(async ([active]) => {
    const created = await browser.tabs.create({ url: "page.html", active: false });
    browser.runtime.sendMessage({ type: "popup-opened", activeUrl: active.url, created: created.id });
});
