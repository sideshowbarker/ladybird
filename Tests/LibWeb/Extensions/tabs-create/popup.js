// Calls tabs.update() with no tab id, and reports which tab it changed, as the tab that opened the popup.
browser.tabs.query({ active: true, currentWindow: true }).then(async ([opener]) => {
    const tab = await browser.tabs.update({});
    browser.runtime.sendMessage({ type: "popup-updated", openerTab: opener.id, tab: tab.id });
});
